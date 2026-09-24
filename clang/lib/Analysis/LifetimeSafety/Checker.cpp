//===- Checker.cpp - C++ Lifetime Safety Checker ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the LifetimeChecker, which detects use-after-free
// errors by checking if live origins hold loans that have expired.
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/Analyses/LifetimeSafety/Checker.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Facts.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LiveOrigins.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LoanPropagation.h"
#include "clang/Analysis/Analyses/LifetimeSafety/Loans.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TimeProfiler.h"

namespace clang::lifetimes::internal {

static bool causingFactDominatesExpiry(LivenessKind K) {
  switch (K) {
  case LivenessKind::Must:
    return true;
  case LivenessKind::Maybe:
  case LivenessKind::Dead:
    return false;
  }
  llvm_unreachable("unknown liveness kind");
}

namespace {

/// Struct to store the complete context for a potential lifetime violation.
struct PendingWarning {
  SourceLocation ExpiryLoc; // Where the loan expired.
  llvm::PointerUnion<const UseFact *, const OriginEscapesFact *> CausingFact;
  const Expr *MovedExpr;
  const Expr *InvalidatedByExpr;
  bool CausingFactDominatesExpiry;
};

using AnnotationTarget =
    llvm::PointerUnion<const ParmVarDecl *, const CXXMethodDecl *>;
using EscapingTarget = LifetimeSafetySemaHelper::EscapingTarget;

class LifetimeChecker {
private:
  /// Where a parameter's `noescape` was found, for the note that accompanies
  /// the violation. \c Loc is what to point the note at, and \c AppearsInSource
  /// says whether the attribute is spelled there.
  struct NoEscapeOrigin {
    SourceLocation Loc;
    bool AppearsInSource;
  };

  /// A pending noescape violation: what the parameter escaped to, and where
  /// the `noescape` that forbids it was found.
  struct NoescapeViolation {
    EscapingTarget Target;
    NoEscapeOrigin Origin;
  };

  llvm::DenseMap<LoanID, PendingWarning> FinalWarningsMap;
  llvm::DenseMap<AnnotationTarget, EscapingTarget> AnnotationWarningsMap;
  llvm::DenseMap<const ParmVarDecl *, NoescapeViolation> NoescapeWarningsMap;
  llvm::DenseSet<const Decl *> VerifiedLiftimeboundEscapes;
  const LoanPropagationAnalysis &LoanPropagation;
  const MovedLoansAnalysis &MovedLoans;
  const LiveOriginsAnalysis &LiveOrigins;
  FactManager &FactMgr;
  LifetimeSafetySemaHelper *SemaHelper;
  ASTContext &AST;
  const CFG *Cfg;
  const Decl *FD;
  const LifetimeSafetyOpts &LSOpts;

  static SourceLocation
  GetFactLoc(llvm::PointerUnion<const UseFact *, const OriginEscapesFact *> F) {
    if (const auto *UF = F.dyn_cast<const UseFact *>())
      return UF->getUseExpr()->getExprLoc();
    if (const auto *OEF = F.dyn_cast<const OriginEscapesFact *>()) {
      if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF))
        return ReturnEsc->getReturnExpr()->getExprLoc();
      if (auto *FieldEsc = dyn_cast<FieldEscapeFact>(OEF))
        return FieldEsc->getFieldDecl()->getLocation();
    }
    llvm_unreachable("unhandled causing fact in PointerUnion");
  }

public:
  LifetimeChecker(const LoanPropagationAnalysis &LoanPropagation,
                  const MovedLoansAnalysis &MovedLoans,
                  const LiveOriginsAnalysis &LiveOrigins, FactManager &FM,
                  AnalysisDeclContext &ADC,
                  LifetimeSafetySemaHelper *SemaHelper,
                  const LifetimeSafetyOpts &LSOpts)
      : LoanPropagation(LoanPropagation), MovedLoans(MovedLoans),
        LiveOrigins(LiveOrigins), FactMgr(FM), SemaHelper(SemaHelper),
        AST(ADC.getASTContext()), Cfg(ADC.getCFG()), FD(ADC.getDecl()),
        LSOpts(LSOpts) {
    for (const CFGBlock *B : *ADC.getAnalysis<PostOrderCFGView>())
      for (const Fact *F : FactMgr.getFacts(B))
        if (const auto *EF = F->getAs<ExpireFact>())
          checkExpiry(EF);
        else if (const auto *IOF = F->getAs<InvalidateOriginFact>())
          checkInvalidation(IOF);
        else if (const auto *OEF = F->getAs<OriginEscapesFact>())
          checkAnnotations(OEF);
    issuePendingWarnings();
    suggestAnnotations();
    if (LSOpts.CheckNoescapeViolations)
      reportNoescapeViolations();
    if (LSOpts.CheckLifetimeboundViolations)
      reportLifetimeboundViolations();
    if (LSOpts.CheckMisplacedLifetimebound)
      reportMisplacedLifetimebound();
    if (LSOpts.CheckInapplicableLifetimebound)
      reportInapplicableLifetimebound();
    //  Annotation inference is currently guarded by a frontend flag. In the
    //  future, this might be replaced by a design that differentiates between
    //  explicit and inferred findings with separate warning groups.
    if (AST.getLangOpts().EnableLifetimeSafetyInference)
      inferAnnotations();
  }

  /// The origin of a NoEscapeAttr found on \p Param, or std::nullopt if it has
  /// none. API Notes build the attribute with no source location
  /// (SemaAPINotes.cpp's getPlaceholderAttrInfo), so in that case the note
  /// points at the declaration the attribute was applied to rather than at
  /// nothing.
  static std::optional<NoEscapeOrigin>
  noEscapeOriginOf(const ParmVarDecl *Param) {
    const auto *A = Param->getAttr<NoEscapeAttr>();
    if (!A)
      return std::nullopt;
    if (A->getLocation().isValid())
      return NoEscapeOrigin{A->getLocation(), /*AppearsInSource=*/true};
    return NoEscapeOrigin{Param->getLocation(), /*AppearsInSource=*/false};
  }

  /// Returns where the parameter at \p PVD's index is annotated
  /// [[clang::noescape]], looking at the definition and at every redeclaration
  /// in its chain, or std::nullopt if none is.
  ///
  /// Reading only the definition's ParmVarDecl is not enough, and this is
  /// deliberately a different question from the one the type system answers.
  /// NoEscape is a plain Attr, not an InheritableParamAttr, so
  /// mergeParamDeclAttributes never copies it onto a later declaration; and the
  /// FunctionProtoType's ExtParameterInfo bit is AND-merged across
  /// redeclarations by ASTContext::mergeExtParameterInfo -- it is the only such
  /// bit permitted to differ, and the composite type is deliberately the weaker
  /// one, so that a declaration whose annotation disagrees with the definition
  /// stays legal. When a declaration is annotated and the definition is not,
  /// the definition therefore has neither the attribute nor the type bit, and
  /// CodeGen emits no captures(address) promise. In C nothing diagnoses that.
  ///
  /// This check disagrees with the merged type on purpose. Its job is to audit
  /// the annotations that this project's cross-TU inference *trusted*, and the
  /// inference trusts a noescape found on a redeclaration's parameter or in the
  /// function type: ParameterEscape's
  /// EscapeClassifier::paramIsDeclaredNoescape looks in both places, by
  /// parameter index, and records no escape edge for a parameter it finds
  /// annotated in either. Looking anywhere less would leave those trusted
  /// inputs unaudited, so both places are checked here too:
  ///
  ///  - the attribute on a redeclaration's ParmVarDecl. API Notes reach the
  ///    audit only this way: they attach NoEscapeAttr to the imported header's
  ///    declaration (SemaAPINotes.cpp), with no source location and without
  ///    setting the type bit.
  ///  - the ExtParameterInfo bit in a redeclaration's FunctionProtoType. A
  ///    function declared through a typedef of function type reaches the audit
  ///    only this way: `typedef void S(int *__attribute__((noescape))); S f;`
  ///    gives `f` implicit ParmVarDecls that carry no attribute while its type
  ///    carries the bit.
  ///
  /// This is a strict superset of what paramIsDeclaredNoescape consults, not a
  /// match: that function walks the redeclarations' *attributes* but only one
  /// type, the FunctionDecl it was handed, whereas the loop below consults
  /// every redeclaration's type. Being a superset is precisely what makes it
  /// safe for an audit: it cannot leave a trusted annotation unchecked, because
  /// anything the inference found is also found here.
  ///
  /// It does find annotations the inference did not, and that is fine. For
  /// `typedef void S(int *__attribute__((noescape))); S f; void f(int *p);`
  /// with a definition that escapes, no NoEscapeAttr node exists anywhere --
  /// the annotation lives only in the first declaration's type sugar -- and
  /// every call resolves to the most recent declaration, whose AND-merged type
  /// has lost the bit, so paramIsDeclaredNoescape returns false at each call
  /// site and the inference never trusts it. The audit reports it anyway, and
  /// the report is correct: `noescape` was written on a declaration of `f`, and
  /// `f` escapes its parameter. The excess concerns annotations genuinely
  /// present somewhere in the chain, never fabricated ones.
  ///
  /// Within that, the *definition's* own merged type could be skipped without
  /// loss, because the AND-merge means the bit survives there only when every
  /// redeclaration had it, including the definition, whose own parameter then
  /// carries the attribute. It is the redeclarations' types that have to be
  /// consulted; including the definition's costs nothing.
  ///
  /// The divergence from the type system already existed in the other
  /// direction -- a definition-annotated function whose merged type lost the
  /// bit is warned about here while CodeGen declines the promise -- so this
  /// makes an existing asymmetry symmetric rather than introducing one.
  ///
  /// Parameters are matched by index, as suggestWithScopeForParmVar and
  /// Sema::MergeCompatibleFunctionDecls both do. The bound checks are
  /// load-bearing: an unprototyped C declaration that precedes any prototype
  /// keeps the type `T()` and is given no ParmVarDecl at all, so its parameter
  /// count is zero while the definition's is not.
  ///
  /// The loops walk the whole redecls() chain and filter nothing by position.
  /// What differs between the analysis modes is which redeclarations *exist*
  /// when the walk runs, not which of them it looks at. In per-function mode
  /// the analysis runs at the end of each function body, so a redeclaration
  /// parsed afterwards has not been created yet and cannot be in the chain; in
  /// translation-unit mode it has been, and the violation is reported. The
  /// inference trusts such an annotation either way, so per-function mode
  /// under-reports. That is inherent to analyzing a function before the TU is
  /// complete and cannot be fixed here -- and in particular, do not add a
  /// position filter to make this code look like it only considers preceding
  /// declarations, because that would break TU-mode reporting.
  ///
  /// The dyn_cast is defensive and cannot currently fail: a noescape audit is
  /// only ever reached through a PlaceholderBase carrying a ParmVarDecl, and
  /// FactsGenerator::issuePlaceholderLoans creates none unless the analyzed
  /// decl is a FunctionDecl. A block parameter and an Objective-C method
  /// parameter therefore get no noescape audit at all, before or after this
  /// change -- not a definition-only one.
  std::optional<NoEscapeOrigin>
  findNoEscapeAnnotation(const ParmVarDecl *PVD) const {
    // An attribute *written on* this parameter, handled first and deliberately
    // yielding no note location: the warning is emitted at this very parameter,
    // so a note saying the attribute is here too would be noise. This branch is
    // redundant for deciding *whether* the parameter is noescape -- redecls()
    // includes the definition and PVD is one of its parameters, so the loop
    // below would find it -- and exists only to suppress that note.
    //
    // The predicate is written-ness, not presence, and the difference is not
    // cosmetic. API Notes build the attribute with no source location and, when
    // the definition lives in a header they annotate, attach it to every
    // redeclaration including the definition. Testing hasAttr would then
    // suppress the note for a definition that spells no annotation, which is
    // exactly the case the note exists for. Falling through to the loop lets
    // noEscapeOriginOf name the declaration instead.
    if (const auto *OwnAttr = PVD->getAttr<NoEscapeAttr>();
        OwnAttr && OwnAttr->getLocation().isValid())
      return NoEscapeOrigin{SourceLocation(), /*AppearsInSource=*/true};
    const auto *Func = dyn_cast<FunctionDecl>(FD);
    if (!Func)
      return std::nullopt;
    unsigned Index = PVD->getFunctionScopeIndex();
    // A written attribute anywhere in the chain beats a type bit anywhere,
    // which is why this is two passes rather than one. A single pass points the
    // note at whichever the traversal reaches first, and the two are not
    // equally good: an unprototyped redeclaration composes to the prototype's
    // type and so carries the bit, so a chain of `f(int *noescape); f();`
    // would be explained by pointing at the `f();` and saying the annotation is
    // not written there -- while it is written one line up.
    for (const FunctionDecl *Redecl : Func->redecls())
      if (Index < Redecl->getNumParams())
        if (auto Found = noEscapeOriginOf(Redecl->getParamDecl(Index)))
          return Found;
    for (const FunctionDecl *Redecl : Func->redecls())
      if (const auto *FPT = Redecl->getType()->getAs<FunctionProtoType>())
        if (Index < FPT->getNumParams() &&
            FPT->getExtParameterInfo(Index).isNoEscape())
          return NoEscapeOrigin{Redecl->getLocation(),
                                /*AppearsInSource=*/false};
    return std::nullopt;
  }

  /// Checks if an escaping origin holds a placeholder loan, indicating a
  /// missing [[clang::lifetimebound]] annotation or a violation of
  /// [[clang::noescape]].
  void checkAnnotations(const OriginEscapesFact *OEF) {
    OriginID EscapedOID = OEF->getEscapedOriginID();
    LoanSet EscapedLoans = LoanPropagation.getLoans(EscapedOID, OEF);
    auto CheckParam = [&](const ParmVarDecl *PVD, bool IsMoved) {
      // NoEscape param should not escape.
      if (auto Origin = findNoEscapeAnnotation(PVD)) {
        if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF))
          NoescapeWarningsMap.try_emplace(
              PVD, NoescapeViolation{ReturnEsc->getReturnExpr(), *Origin});
        if (auto *FieldEsc = dyn_cast<FieldEscapeFact>(OEF))
          NoescapeWarningsMap.try_emplace(
              PVD, NoescapeViolation{FieldEsc->getFieldDecl(), *Origin});
        if (auto *GlobalEsc = dyn_cast<GlobalEscapeFact>(OEF))
          NoescapeWarningsMap.try_emplace(
              PVD, NoescapeViolation{GlobalEsc->getGlobal(), *Origin});
        // A parameter can carry both attributes. Returning without recording
        // the lifetimebound escape makes reportLifetimeboundViolations
        // conclude it never escapes, adding a second and false "could not
        // verify that the return value can be lifetime bound" about a
        // function that demonstrably does return it. Mirrors the conditions
        // in the lifetimebound branch below.
        if (!IsMoved && PVD->hasAttr<LifetimeBoundAttr>() &&
            (isa<ReturnEscapeFact>(OEF) ||
             (isa<FieldEscapeFact>(OEF) && isa<CXXConstructorDecl>(FD))))
          VerifiedLiftimeboundEscapes.insert(PVD);
        return;
      }
      // Skip annotation suggestion for moved loans, as ownership transfer
      // obscures the lifetime relationship (e.g., shared_ptr from unique_ptr).
      if (IsMoved)
        return;
      if (PVD->hasAttr<LifetimeBoundAttr>()) {
        // Track that this lifetimebound parameter correctly escapes
        // (via return or via field assignment in a constructor).
        if (isa<ReturnEscapeFact>(OEF) ||
            (isa<FieldEscapeFact>(OEF) && isa<CXXConstructorDecl>(FD)))
          VerifiedLiftimeboundEscapes.insert(PVD);
      } else {
        // Otherwise, suggest lifetimebound for parameter escaping through
        // return or a field in constructor.
        if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF))
          AnnotationWarningsMap.try_emplace(PVD, ReturnEsc->getReturnExpr());
        else if (auto *FieldEsc = dyn_cast<FieldEscapeFact>(OEF);
                 FieldEsc && isa<CXXConstructorDecl>(FD)) {
          // Disable inference for pointers being captured by an owner type,
          // as owners typically consume these pointers rather than borrow them.
          if (!isOwnerPtrCtor(dyn_cast<CXXConstructorDecl>(FD), PVD))
            AnnotationWarningsMap.try_emplace(PVD, FieldEsc->getFieldDecl());
        }
      }
      // TODO: Suggest lifetime_capture_by(this) for parameter escaping to a
      // field!
    };
    auto CheckImplicitThis = [&](const CXXMethodDecl *MD) {
      if (auto *ReturnEsc = dyn_cast<ReturnEscapeFact>(OEF)) {
        if (implicitObjectParamIsLifetimeBound(MD))
          VerifiedLiftimeboundEscapes.insert(MD);
        else
          AnnotationWarningsMap.try_emplace(MD, ReturnEsc->getReturnExpr());
      }
    };
    auto MovedAtEscape = MovedLoans.getMovedLoans(OEF);
    for (LoanID LID : EscapedLoans) {
      const Loan *L = FactMgr.getLoanMgr().getLoan(LID);
      const PlaceholderBase *PB = L->getAccessPath().getAsPlaceholderBase();
      if (!PB)
        continue;
      if (const auto *PVD = PB->getParmVarDecl())
        CheckParam(PVD, /*IsMoved=*/MovedAtEscape.lookup(LID));
      else if (const auto *MD = PB->getImplicitThisParent())
        CheckImplicitThis(MD);
    }
  }

  /// Checks for use-after-free & use-after-return errors when an access path
  /// expires (e.g., a variable goes out of scope).
  ///
  /// When a path expires, all loans prefixed by that path expire. For example,
  /// if `x` expires, loans to `x`, `x.field`, and `x.field.*` all expire.
  /// This method examines all live origins and reports warnings for loans they
  /// hold that are prefixed by the expired path.
  void checkExpiry(const ExpireFact *EF) {
    const AccessPath &ExpiredPath = EF->getAccessPath();
    LiveOriginSet Origins = LiveOrigins.getLiveOriginsAt(EF);
    for (const LivenessMap &Live : {Origins.Persistent, Origins.BlockLocal})
      for (auto &[OID, LiveInfo] : Live) {
        LoanSet HeldLoans = LoanPropagation.getLoans(OID, EF);
        for (LoanID HeldLoanID : HeldLoans) {
          const Loan *HeldLoan = FactMgr.getLoanMgr().getLoan(HeldLoanID);
          if (!ExpiredPath.isPrefixOf(HeldLoan->getAccessPath()))
            continue;
          // HeldLoan is expired because its base or itself is expired.
          PendingWarning &CurWarning = FinalWarningsMap[HeldLoan->getID()];
          const Expr *MovedExpr = nullptr;
          if (auto *ME = MovedLoans.getMovedLoans(EF).lookup(HeldLoanID))
            MovedExpr = *ME;
          // Skip if we already have a dominating causing fact.
          if (CurWarning.CausingFactDominatesExpiry)
            continue;
          if (causingFactDominatesExpiry(LiveInfo.Kind))
            CurWarning.CausingFactDominatesExpiry = true;
          CurWarning.CausingFact = LiveInfo.CausingFact;
          CurWarning.ExpiryLoc = EF->getExpiryLoc();
          CurWarning.MovedExpr = MovedExpr;
          CurWarning.InvalidatedByExpr = nullptr;
        }
      }
  }

  /// Checks for use-after-invalidation errors when a container is modified.
  ///
  /// When a container is invalidated, loans pointing into its interior are
  /// invalidated. For example, if container `v` is invalidated, iterators with
  /// loans to `v.*` are invalidated. This method finds live origins holding
  /// such loans and reports warnings. A loan is invalidated if its path extends
  /// an invalidated container's path (e.g., `v.*` extends `v`).
  void checkInvalidation(const InvalidateOriginFact *IOF) {
    OriginID InvalidatedOrigin = IOF->getInvalidatedOrigin();
    /// Get loans directly pointing to the invalidated container
    LoanSet DirectlyInvalidatedLoans =
        LoanPropagation.getLoans(InvalidatedOrigin, IOF);
    auto IsInvalidated = [&](const Loan *L) {
      for (LoanID InvalidID : DirectlyInvalidatedLoans) {
        const Loan *InvalidL = FactMgr.getLoanMgr().getLoan(InvalidID);
        if (InvalidL->getAccessPath().isPrefixOf(L->getAccessPath()))
          return true;
      }
      return false;
    };
    // For each live origin, check if it holds an invalidated loan and report.
    LiveOriginSet Origins = LiveOrigins.getLiveOriginsAt(IOF);
    for (const LivenessMap &Live : {Origins.Persistent, Origins.BlockLocal})
      for (auto &[OID, LiveInfo] : Live) {
        LoanSet HeldLoans = LoanPropagation.getLoans(OID, IOF);
        for (LoanID LiveLoanID : HeldLoans)
          if (IsInvalidated(FactMgr.getLoanMgr().getLoan(LiveLoanID))) {
            bool CurDomination = causingFactDominatesExpiry(LiveInfo.Kind);
            bool LastDomination =
                FinalWarningsMap.lookup(LiveLoanID).CausingFactDominatesExpiry;
            if (!LastDomination) {
              FinalWarningsMap[LiveLoanID] = {
                  /*ExpiryLoc=*/{},
                  /*CausingFact=*/LiveInfo.CausingFact,
                  /*MovedExpr=*/nullptr,
                  /*InvalidatedByExpr=*/IOF->getInvalidationExpr(),
                  /*CausingFactDominatesExpiry=*/CurDomination};
            }
          }
      }
  }

  void issuePendingWarnings() {
    llvm::TimeTraceScope TimeTrace("IssuePendingWarnings");
    if (!SemaHelper)
      return;
    for (const auto &[LID, Warning] : FinalWarningsMap) {
      const Loan *L = FactMgr.getLoanMgr().getLoan(LID);
      const Expr *IssueExpr = L->getIssueExpr();
      const ParmVarDecl *InvalidatedPVD = nullptr;
      if (const PlaceholderBase *PB = L->getAccessPath().getAsPlaceholderBase())
        InvalidatedPVD = PB->getParmVarDecl();

      llvm::PointerUnion<const UseFact *, const OriginEscapesFact *>
          CausingFact = Warning.CausingFact;
      const Expr *MovedExpr = Warning.MovedExpr;
      SourceLocation ExpiryLoc = Warning.ExpiryLoc;

      if (const auto *UF = CausingFact.dyn_cast<const UseFact *>()) {
        llvm::SmallVector<const Expr *> ExprChain =
            getExprChain(LoanPropagation.buildOriginFlowChain(UF, LID, Cfg));
        if (Warning.InvalidatedByExpr) {
          if (IssueExpr)
            // Use-after-invalidation of an object on stack.
            SemaHelper->reportUseAfterInvalidation(IssueExpr, UF->getUseExpr(),
                                                   Warning.InvalidatedByExpr,
                                                   ExprChain);
          else if (InvalidatedPVD)
            // Use-after-invalidation of a parameter.
            SemaHelper->reportUseAfterInvalidation(
                InvalidatedPVD, UF->getUseExpr(), Warning.InvalidatedByExpr,
                ExprChain);

        } else
          // Scope-based expiry (use-after-scope).
          SemaHelper->reportUseAfterScope(IssueExpr, UF->getUseExpr(),
                                          MovedExpr, ExpiryLoc, ExprChain);

      } else if (const auto *OEF =
                     CausingFact.dyn_cast<const OriginEscapesFact *>()) {
        if (Warning.InvalidatedByExpr) {
          if (const auto *FieldEscape = dyn_cast<FieldEscapeFact>(OEF)) {
            // Invalidated object escapes to a field.
            if (IssueExpr)
              // Invalidated object on stack escapes to a field.
              SemaHelper->reportInvalidatedField(IssueExpr,
                                                 FieldEscape->getFieldDecl(),
                                                 Warning.InvalidatedByExpr);
            else if (InvalidatedPVD)
              // Invalidated parameter escapes to a field.
              SemaHelper->reportInvalidatedField(InvalidatedPVD,
                                                 FieldEscape->getFieldDecl(),
                                                 Warning.InvalidatedByExpr);
          } else if (const auto *GlobalEscape =
                         dyn_cast<GlobalEscapeFact>(OEF)) {
            // Invalidated object escapes to global or static storage.
            if (IssueExpr)
              // Invalidated object on stack escapes to global or static
              // storage.
              SemaHelper->reportInvalidatedGlobal(IssueExpr,
                                                  GlobalEscape->getGlobal(),
                                                  Warning.InvalidatedByExpr);
            else if (InvalidatedPVD)
              // Invalidated parameter escapes to global or static storage.
              SemaHelper->reportInvalidatedGlobal(InvalidatedPVD,
                                                  GlobalEscape->getGlobal(),
                                                  Warning.InvalidatedByExpr);
          } else if (isa<ReturnEscapeFact>(OEF)) {
            // FIXME: Diagnose invalidated return escapes separately.
          } else
            llvm_unreachable("Unhandled OriginEscapesFact type");
        } else if (const auto *RetEscape = dyn_cast<ReturnEscapeFact>(OEF))
          // Return stack address.
          SemaHelper->reportUseAfterReturn(
              IssueExpr, RetEscape->getReturnExpr(), MovedExpr);
        else if (const auto *FieldEscape = dyn_cast<FieldEscapeFact>(OEF)) {
          // Dangling field.
          bool IsCapturedByLambda =
              FactMgr.isFieldCapturedByLambda(FieldEscape->getFieldDecl());
          SemaHelper->reportDanglingField(
              IssueExpr, FieldEscape->getFieldDecl(), MovedExpr,
              IsCapturedByLambda, ExpiryLoc);
        } else if (const auto *GlobalEscape = dyn_cast<GlobalEscapeFact>(OEF)) {
          // Global escape.
          bool IsMain = false;
          if (const auto *Func = dyn_cast_if_present<FunctionDecl>(FD))
            IsMain = Func->isMain();
          SemaHelper->reportDanglingGlobal(IssueExpr, GlobalEscape->getGlobal(),
                                           MovedExpr, ExpiryLoc, IsMain);
        } else
          llvm_unreachable("Unhandled OriginEscapesFact type");
      } else
        llvm_unreachable("Unhandled CausingFact type");
    }
  }

  // Returns declarations that should be annotated with lifetime attributes
  // in order to annotate FDef: the canonical declaration and the earliest
  // redeclarations in each other file. This defines the placement policy for
  // lifetime annotations. Each target is paired with its corresponding warning
  // scope.
  llvm::SmallVector<std::pair<const FunctionDecl *, WarningScope>, 2>
  getTargetDeclsForAttr(const FunctionDecl *FDef) {
    if (!FDef)
      return {};

    assert(FDef->isThisDeclarationADefinition() &&
           "Expected FunctionDecl to be a definition");

    const auto &SM = FDef->getASTContext().getSourceManager();

    auto GetFile = [&SM](const FunctionDecl *FD) {
      return SM.getFileID(SM.getExpansionLoc(FD->getLocation()));
    };

    const FileID DefFile = GetFile(FDef);
    const FunctionDecl *CanonicalDecl = FDef->getCanonicalDecl();
    llvm::SmallVector<std::pair<const FunctionDecl *, WarningScope>, 2> Targets{
        {CanonicalDecl, GetFile(CanonicalDecl) == DefFile
                            ? WarningScope::IntraTU
                            : WarningScope::CrossTU}};

    // Find the earliest redeclaration in each file other than the definition
    // file.
    auto AddCrossTUDecl = [&](const FunctionDecl *FD) {
      FileID File = GetFile(FD);
      if (File == DefFile)
        return;
      for (auto [SeenFD, _] : Targets)
        if (GetFile(SeenFD) == File)
          return;
      Targets.push_back({FD, WarningScope::CrossTU});
    };

    // We iterate in reverse order (from most recent to oldest) to find
    // the first declaration in each file.

    // Store in temporary variable to manually extend lifetime
    auto redecls = llvm::to_vector(FDef->redecls());

    for (const FunctionDecl *Redecl : llvm::reverse(redecls))
      AddCrossTUDecl(Redecl);

    return Targets;
  }

  void suggestWithScopeForParmVar(const ParmVarDecl *PVD,
                                  EscapingTarget EscapeTarget) {
    if (llvm::isa<const VarDecl *>(EscapeTarget))
      return;

    for (auto [Decl, Scope] : getTargetDeclsForAttr(cast<FunctionDecl>(FD))) {
      const auto *ParmToAnnotate =
          Decl->getParamDecl(PVD->getFunctionScopeIndex());
      SemaHelper->suggestLifetimeboundToParmVar(Scope, ParmToAnnotate,
                                                EscapeTarget);
    }
  }

  void suggestWithScopeForImplicitThis(const CXXMethodDecl *MD,
                                       const Expr *EscapeExpr) {
    for (auto [Decl, Scope] : getTargetDeclsForAttr(MD)) {
      SemaHelper->suggestLifetimeboundToImplicitThis(
          Scope, cast<CXXMethodDecl>(Decl), EscapeExpr);
    }
  }

  void suggestAnnotations() {
    if (!SemaHelper)
      return;
    if (!LSOpts.SuggestAnnotations)
      return;
    llvm::TimeTraceScope TimeTrace("SuggestAnnotations");
    for (auto [Target, EscapeTarget] : AnnotationWarningsMap) {
      if (const auto *PVD = Target.dyn_cast<const ParmVarDecl *>())
        suggestWithScopeForParmVar(PVD, EscapeTarget);
      else if (const auto *MD = Target.dyn_cast<const CXXMethodDecl *>()) {
        if (const auto *EscapeExpr = EscapeTarget.dyn_cast<const Expr *>())
          suggestWithScopeForImplicitThis(MD, EscapeExpr);
        else
          llvm_unreachable("Implicit this can only escape via Expr (return)");
      }
    }
  }

  void reportNoescapeViolations() {
    llvm::TimeTraceScope TimeTrace("ReportNoescapeViolations");
    for (auto [PVD, Violation] : NoescapeWarningsMap) {
      EscapingTarget EscapeTarget = Violation.Target;
      if (const auto *E = EscapeTarget.dyn_cast<const Expr *>())
        SemaHelper->reportNoescapeViolation(PVD, E);
      else if (const auto *FD = EscapeTarget.dyn_cast<const FieldDecl *>())
        SemaHelper->reportNoescapeViolation(PVD, FD);
      else if (const auto *G = EscapeTarget.dyn_cast<const VarDecl *>())
        SemaHelper->reportNoescapeViolation(PVD, G);
      else
        llvm_unreachable("Unhandled EscapingTarget type");
      // The warning is emitted at the definition's parameter, which for a
      // header or API Notes annotation is a line that says nothing about
      // noescape. Point at where the annotation actually is. An invalid
      // location means the definition's own parameter carries it, so the
      // warning already points at it and no note is wanted.
      if (Violation.Origin.Loc.isValid())
        SemaHelper->noteNoescapeAnnotation(Violation.Origin.Loc,
                                           Violation.Origin.AppearsInSource);
    }
  }

  void reportLifetimeboundViolations() {
    llvm::TimeTraceScope TimeTrace("ReportLifetimeboundViolations");
    if (!isa<FunctionDecl>(FD))
      return;
    if (const auto *MD = dyn_cast<CXXMethodDecl>(FD);
        MD && getImplicitObjectParamLifetimeBoundAttr(MD) &&
        !VerifiedLiftimeboundEscapes.contains(MD))
      SemaHelper->reportLifetimeboundViolation(MD);
    for (const ParmVarDecl *PVD : cast<FunctionDecl>(FD)->parameters()) {
      if (!PVD->hasAttr<LifetimeBoundAttr>())
        continue;
      bool isImplicit = PVD->getAttr<LifetimeBoundAttr>()->isImplicit();
      bool Escapes = VerifiedLiftimeboundEscapes.contains(PVD);
      assert((!isImplicit || Escapes || isInStlNamespace(FD)) &&
             "Implicit lifetimebound parameters "
             "should escape through return");
      if (!isImplicit && !Escapes)
        SemaHelper->reportLifetimeboundViolation(PVD);
    }
  }

  // Reports lifetimebound attributes that are placed on a function definition
  // but not on the corresponding declaration.
  void reportMisplacedLifetimebound() {
    llvm::TimeTraceScope TimeTrace("ReportMisplacedLifetimebound");
    const FunctionDecl *FDef = dyn_cast<FunctionDecl>(FD);
    if (!FDef)
      return;

    auto TargetDecls = getTargetDeclsForAttr(FDef);
    // Check if implicit 'this' has lifetimebound on definition but not on
    // declaration.
    if (const auto *MDef = dyn_cast<CXXMethodDecl>(FDef);
        MDef && getDirectImplicitObjectLifetimeBoundAttr(MDef))
      for (auto [Decl, Scope] : TargetDecls) {
        const auto *MDecl = cast<CXXMethodDecl>(Decl);
        if (!getDirectImplicitObjectLifetimeBoundAttr(MDecl))
          SemaHelper->reportMisplacedLifetimebound(Scope, MDef, MDecl);
      }

    // Check each parameter for explicit lifetimebound on definition but not on
    // declaration.
    for (const auto *PDef : FDef->parameters()) {
      const auto *Attr = PDef->getAttr<LifetimeBoundAttr>();
      if (!Attr || Attr->isImplicit())
        continue;
      for (auto [Decl, Scope] : TargetDecls) {
        const auto *PDecl = Decl->getParamDecl(PDef->getFunctionScopeIndex());
        if (!PDecl->hasAttr<LifetimeBoundAttr>())
          SemaHelper->reportMisplacedLifetimebound(Scope, PDef, PDecl);
      }
    }
  }

  void reportInapplicableLifetimebound() {
    llvm::TimeTraceScope TimeTrace("ReportInapplicableLifetimebound");
    const auto *FDef = dyn_cast<FunctionDecl>(FD);
    if (!FDef)
      return;

    // If analyzed function is a template definition or an implicit
    // instantiation, skip.
    if (FDef->getTemplatedKind() == FunctionDecl::TK_FunctionTemplate ||
        FDef->getTemplateSpecializationKind() == TSK_ImplicitInstantiation)
      return;

    for (const auto &PVD : FDef->parameters())
      if (PVD->hasAttr<LifetimeBoundAttr>() &&
          !FactMgr.getOriginMgr().hasOrigins(PVD->getType(),
                                             /*IntrinsicOnly=*/true))
        SemaHelper->reportInapplicableLifetimebound(PVD);
  }

  void inferAnnotations() {
    for (auto [Target, EscapeTarget] : AnnotationWarningsMap) {
      if (const auto *MD = Target.dyn_cast<const CXXMethodDecl *>()) {
        if (!implicitObjectParamIsLifetimeBound(MD))
          SemaHelper->addLifetimeBoundToImplicitThis(cast<CXXMethodDecl>(MD));
      } else if (const auto *PVD = Target.dyn_cast<const ParmVarDecl *>()) {
        const auto *FD = dyn_cast<FunctionDecl>(PVD->getDeclContext());
        if (!FD)
          continue;
        // Propagates inferred attributes via the most recent declaration to
        // ensure visibility for callers in post-order analysis.
        FD = getDeclWithMergedLifetimeBoundAttrs(FD);
        ParmVarDecl *InferredPVD = const_cast<ParmVarDecl *>(
            FD->getParamDecl(PVD->getFunctionScopeIndex()));
        if (!InferredPVD->hasAttr<LifetimeBoundAttr>())
          InferredPVD->addAttr(
              LifetimeBoundAttr::CreateImplicit(AST, PVD->getLocation()));
      }
    }
  }

  /// Extract expressions from the origin flow chain for diagnostic purposes.
  ///
  /// Given a chain of origins that shows how a loan propagates, this function
  /// extracts the corresponding expressions for each origin. Origins that refer
  /// to declarations (rather than expressions) are skipped.
  llvm::SmallVector<const Expr *>
  getExprChain(llvm::ArrayRef<OriginID> OriginFlowChain) {
    llvm::SmallVector<const Expr *> rs;
    for (const OriginID CurrOID : OriginFlowChain)
      if (const Expr *CurrExpr =
              FactMgr.getOriginMgr().getOrigin(CurrOID).getExpr())
        rs.push_back(CurrExpr);
    return rs;
  }
};
} // namespace

void runLifetimeChecker(const LoanPropagationAnalysis &LP,
                        const MovedLoansAnalysis &MovedLoans,
                        const LiveOriginsAnalysis &LO, FactManager &FactMgr,
                        AnalysisDeclContext &ADC,
                        LifetimeSafetySemaHelper *SemaHelper,
                        const LifetimeSafetyOpts &LSOpts) {
  llvm::TimeTraceScope TimeProfile("LifetimeChecker");
  LifetimeChecker Checker(LP, MovedLoans, LO, FactMgr, ADC, SemaHelper, LSOpts);
}

} // namespace clang::lifetimes::internal
