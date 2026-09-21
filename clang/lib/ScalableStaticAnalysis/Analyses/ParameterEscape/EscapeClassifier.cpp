//===- EscapeClassifier.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EscapeClassifier.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtObjC.h"
#include "clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Casting.h"

#include <cassert>

#include <utility>

using namespace clang;
using namespace clang::ssaf;

static bool hasSwiftNonEscapableAttr(const CXXRecordDecl *RD) {
  for (const auto *A : RD->specific_attrs<SwiftAttrAttr>())
    if (A->getAttribute() == "~Escapable")
      return true;
  return false;
}

/// \returns true iff \p T is a record type marked `[[gsl::Pointer]]` or
/// `swift_attr("~Escapable")`.
///
/// Used only to *refuse*: M1 defers tracked views (see #34), and a reference to
/// one is refused alongside a view passed by value. It is not the old
/// isTrackedViewType -- nothing is admitted to anything on the strength of it,
/// and it asks for no triviality or layout property, because a refusal needs
/// none.
///
/// An incomplete referent answers false, so a reference to it stays in the
/// population. That is deliberate and is not a soundness direction either way:
/// what M1 claims about a reference is that the *reference* does not escape,
/// which design section 1.1 decides identically for every record. The refusal
/// below is a scope policy while the byte-versus-pointer question (#32) is
/// open, so its unknown answer costs precision rather than correctness, and
/// treating an unnameable record as a view would refuse every opaque handle
/// type with it.
static bool isViewRecordType(QualType T) {
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  if (!RD || !RD->hasDefinition())
    return false;
  return lifetimes::isGslPointerType(T) ||
         hasSwiftNonEscapableAttr(RD->getDefinition());
}

bool clang::ssaf::isPointerCarryingType(QualType T) {
  T = T.getCanonicalType();
  // A reference to a view is refused with the view itself: `V &`, `const V &`
  // and `V &&` alike, since getNonReferenceType() strips all three.
  if (T->isReferenceType())
    return !isViewRecordType(T.getNonReferenceType());
  // isAnyPointerType() also covers ObjC object pointers, which are analyzed
  // (their facts feed callers) even though they are never annotated.
  if (T->isAnyPointerType() && !T->isFunctionPointerType())
    return true;
  return T->isBlockPointerType();
}

bool clang::ssaf::isCandidateParameterType(QualType T) {
  T = T.getCanonicalType();
  if (T->isReferenceType())
    // A reference to a function denotes no object, so `noescape` on it would
    // say nothing. Excluding it also removes an unexplained asymmetry with the
    // function pointer rejected three lines below. A reference to a view is
    // refused for the reason on isViewRecordType; it is refused here as well
    // as above so that this predicate stays a subset of isPointerCarryingType,
    // which the extractor asserts.
    return !T->isFunctionReferenceType() &&
           !isViewRecordType(T.getNonReferenceType());
  // Neither ObjC object pointers nor block pointers are PointerType, so this
  // admits object pointers only; function pointers are excluded explicitly.
  // A pointer to a view is refused here and *only* here: it stays in the
  // analysis population, because refusing it there would stop its uses being
  // classified rather than refuse them, but M1 emits no annotation for a view
  // shape while #32 is open.
  if (T->isPointerType())
    return !T->isFunctionPointerType() && !isViewRecordType(T->getPointeeType());
  return false;
}

bool clang::ssaf::isCandidateDefinition(const FunctionDecl *Def,
                                        ASTContext &Ctx) {
  // Candidacy is a property of a definition. A bodiless declaration is never a
  // candidate -- its facts, if any, would shadow the definition's. This is
  // also what excludes a deduction guide, which never has a body; a separate
  // CXXDeductionGuideDecl test could not reject anything this does not.
  if (!Def->doesThisDeclarationHaveABody())
    return false;

  // `main` is called by the runtime, not by anything this analysis can see, so
  // its parameters are never annotated.
  if (Def->isMain())
    return false;
  if (const auto *MD = dyn_cast<CXXMethodDecl>(Def); MD && MD->isVirtual())
    return false;

  // A definition whose body is not necessarily the body that runs.
  //
  // Facts extracted here describe code the program may never execute, while
  // the code it does execute is invisible to this TU. Candidacy is the only
  // gate that can catch this class: EntityLinkageType is just
  // {None, Internal, External}, and neither the linker's first-contributor-wins
  // merge nor the whole-program fixpoint models replaceable definitions.
  //
  //  - Multiversioning dispatches to a sibling body that is not in this
  //    declaration's redeclaration chain, so the loop below cannot see it.
  //  - A weak definition may be replaced at link time by a strong one from
  //    another TU; first-contributor-wins would then pick between them
  //    arbitrarily.
  //  - GVA_AvailableExternally *is* "this definition is not the one that will
  //    be emitted": `extern __inline__ __attribute__((gnu_inline))` (the glibc
  //    pattern, whose in-TU body is never emitted) and C99 `inline` without an
  //    external declaration, which C11 6.7.4p7 leaves unspecified -- "it is
  //    unspecified whether a call uses the inline definition or the external
  //    definition".
  //  - A weakref is an alias: calls run the aliasee's body, never this one.
  //    clang rejects a weakref carrying a body, so this is reachable only on
  //    an AST that contains errors -- which this extractor runs on, and which
  //    WeakRefDefinitionIsNotCandidate exercises.
  //
  // Each is its own statement rather than a disjunction, so that no one of
  // them can be dropped behind another.
  if (Def->isMultiVersion())
    return false;
  if (Def->hasAttr<WeakAttr>())
    return false;
  if (Def->hasAttr<WeakRefAttr>())
    return false;
  if (Ctx.GetGVALinkageForFunction(Def) == GVA_AvailableExternally)
    return false;

  // In a naked function the parameters are never materialized and the body is
  // inline asm that reaches the incoming registers directly, without operands.
  // Every other function is safe because asm can only touch a parameter
  // through an operand, which the classifier sinks; this is the one case where
  // it sees no use of a parameter the asm nonetheless captures.
  if (Def->hasAttr<NakedAttr>())
    return false;

  // Not templated in any sense: an instantiation shares the pattern's source,
  // so one instantiation's verdict must never edit the template. These six
  // queries overlap heavily -- of the instantiation cases, each is caught by
  // several -- and the redundancy is deliberate: every one of them can only
  // reject, and the failure they guard against is editing a template through
  // one of its instantiations.
  if (Def->isTemplated())
    return false;
  if (Def->getDescribedFunctionTemplate())
    return false;
  if (Def->getTemplateSpecializationKind() != TSK_Undeclared)
    return false;
  if (Def->isTemplateInstantiation())
    return false;
  if (Def->getInstantiatedFromMemberFunction())
    return false;
  if (Def->getTemplateInstantiationPattern())
    return false;

  if (isa_and_nonnull<CoroutineBodyStmt>(Def->getBody()))
    return false;

  const SourceManager &SM = Ctx.getSourceManager();
  for (const FunctionDecl *RD : Def->redecls()) {
    // A K&R identifier-list declaration gives no parameter list to annotate.
    if (!RD->hasWrittenPrototype())
      return false;
    // This is also the "outside dependent contexts" rule: Decl::isTemplated()
    // on a FunctionDecl is that declaration's own isDependentContext(), which
    // is implied by its DeclContext's, and which for a friend or a block-scope
    // extern declaration consults the *lexical* context -- the stricter and
    // therefore sound reading. A separate
    // getDeclContext()->isDependentContext() test could never reject anything
    // this does not.
    if (RD->isTemplated())
      return false;
    SourceLocation Loc = RD->getLocation();
    // No test pins this: every redeclaration reachable here is one the parser
    // built from source, so none has been observed without a location. It
    // rejects rather than admits because a redeclaration nothing can point at
    // is one the transformation could not rewrite.
    if (Loc.isInvalid())
      return false;
    if (Loc.isMacroID())
      return false;
    if (SM.isInSystemHeader(Loc))
      return false;
    // Only parameters we may annotate need a rewritable begin location.
    //
    // Keyed on candidacy, not on the analysis population, and deliberately so:
    // this gate exists to refuse a definition the transformation could not
    // rewrite, so it must run over exactly the parameters that will be
    // rewritten. Narrowing isCandidateParameterType therefore *widens* this
    // gate -- a macro-spelled `V *` no longer blocks the definition it sits in,
    // and its neighbours become annotatable. The alternative, gating on
    // isPointerCarryingType, would reject a definition over a parameter nobody
    // will ever edit, losing every other parameter with it.
    for (const ParmVarDecl *P : RD->parameters()) {
      if (!isCandidateParameterType(P->getType()))
        continue;
      // A declaration written through a function typedef (`typedef void
      // FT(int *); FT f;`) has no written parameter list at all: clang
      // synthesizes an implicit ParmVarDecl located at the function's own
      // name. There is nothing here to annotate, and resolving the parameter
      // through the TypeSourceInfo would put the edit inside the typedef,
      // where it would apply to every function declared through it.
      if (P->isImplicit())
        return false;
      // Unpinned for the same reason as the declaration location above: the
      // parameters that reach this point are written ones, and none has been
      // observed without a begin location. Rejecting is the direction that
      // cannot produce an edit with nowhere to land.
      if (P->getBeginLoc().isInvalid())
        return false;
      if (P->getBeginLoc().isMacroID())
        return false;
    }
  }
  return true;
}

static SourceLocationRecord recordFor(SourceLocation Loc,
                                      const SourceManager &SM) {
  PresumedLoc P = SM.getPresumedLoc(SM.getExpansionLoc(Loc));
  if (P.isInvalid())
    return SourceLocationRecord{"<unknown>", 0, 0};
  return SourceLocationRecord{P.getFilename(), P.getLine(), P.getColumn()};
}

const llvm::StringLiteral clang::ssaf::MultipleDefinitionsDetail =
    "entity has more than one definition";

void clang::ssaf::degradeToMultipleDefinitions(ParameterEscapeSummary &S,
                                               const FunctionDecl *Def,
                                               ASTContext &Ctx) {
  // Rebuild rather than edit in place. Which of the colliding summaries the
  // builder retained depends on contributor iteration order, so rewriting
  // whatever it happens to hold would make the output vary between identical
  // runs -- against the project's determinism rule. Derived from \p Def, every
  // field is a function of the source instead, and a stale FlowsTo edge or
  // ReturnsSelfAt from some other body becomes unrepresentable rather than
  // something this function has to remember to clear.
  S.IsCandidate = false;
  S.CandidateParams.clear();
  S.Params.clear();
  S.This.reset();

  // Marking it a non-candidate is not enough on its own. The whole-program
  // fixpoint rejects a caller's parameter only when the callee node it flows
  // into escapes, and it reads that from the callee's facts regardless of
  // whether the callee is a candidate. A retained fact describing the one
  // body that happens not to capture would let a caller be annotated.
  Sink Escape{EscapeReason::UnrecognizedUse,
              recordFor(Def->getLocation(), Ctx.getSourceManager()),
              MultipleDefinitionsDetail.str()};
  EscapeFact Escapes;
  Escapes.OtherSink = Escape;

  for (const ParmVarDecl *P : Def->parameters()) {
    if (!isPointerCarryingType(P->getType()))
      continue;
    S.Params[P->getFunctionScopeIndex()] = Escapes;
    if (isCandidateParameterType(P->getType()))
      S.CandidateParams.insert(P->getFunctionScopeIndex());
  }
  if (const auto *MD = dyn_cast<CXXMethodDecl>(Def); MD && MD->isInstance())
    S.This = Escapes;
}

//===--- The classifier ---------------------------------------------------===//

namespace {

/// What an expression denotes with respect to the source being analyzed.
///
/// The kinds are what separates the pointer *value* the caller handed over --
/// the thing `noescape` is a promise about -- from the storage that holds it
/// and from the object it designates. Only a Value can be stored somewhere
/// that outlives the call; a Place is read and written through, which is
/// benign (design section 1.1).
enum class AliasKind : uint8_t {
  None,      ///< unrelated to the source
  Value,     ///< a pointer/reference/view value carrying the source's identity
  Place,     ///< a glvalue denoting the pointee object or one of its subobjects
  VarLValue, ///< the lvalue of a variable (or referent) holding an alias value
};

/// The parameter being analyzed. A null Param is the implicit object
/// parameter, which is a source and a flow target but never a candidate.
struct Source {
  const ParmVarDecl *Param = nullptr;
  bool isThis() const { return Param == nullptr; }
};

/// Computes one EscapeFact per source for one function definition.
///
/// Two passes per source. The first grows the set of local variables that can
/// hold an alias, to a fixpoint. The second walks every expression, asks what
/// it denotes with respect to the source, and -- for those that denote
/// something -- classifies the use its parent makes of it. Every use must land
/// in a recognized row; the last row is a sink, so a shape this enumeration
/// does not model reports an escape rather than silence.
class Classifier {
public:
  Classifier(const FunctionDecl *Def, ASTContext &Ctx,
             TUSummaryExtractor &Extractor)
      : Def(Def), Ctx(Ctx), SM(Ctx.getSourceManager()), Extractor(Extractor) {}

  EscapeFact analyze(Source S) {
    Src = S;
    Fact = EscapeFact();
    AliasVars.clear();
    Memo.clear();
    // `this` seeds no variable: CXXThisExpr is recognized directly.
    if (Src.Param)
      AliasVars.insert(Src.Param);
    growAliasSet();
    // Kinds computed while the alias set was still growing are stale.
    Memo.clear();
    UseVisitor V(*this);
    V.TraverseDecl(const_cast<FunctionDecl *>(Def));
    // Implicit subobject destruction runs after the body, which is also where
    // first-sink-wins wants it. Not guarded on Src.isThis(): a destructor has
    // no parameters, so `this` is the only source it is ever analyzed for.
    if (const auto *DD = dyn_cast<CXXDestructorDecl>(Def)) {
      assert(Src.isThis() && "a destructor has no parameters");
      classifySubobjectDestruction(DD);
    }
    return Fact;
  }

private:
  const FunctionDecl *Def;
  ASTContext &Ctx;
  const SourceManager &SM;
  TUSummaryExtractor &Extractor;
  Source Src;
  llvm::SmallPtrSet<const VarDecl *, 8> AliasVars;
  llvm::DenseMap<const Expr *, AliasKind> Memo;
  EscapeFact Fact;

  //===--- Recording ------------------------------------------------------===//

  void sink(EscapeReason R, SourceLocation Loc, llvm::StringRef Detail = "") {
    // First sink only, and the traversal is in source order.
    if (!Fact.OtherSink)
      Fact.OtherSink = Sink{R, recordFor(Loc, SM), Detail.str()};
  }

  void recordReturn(SourceLocation Loc) {
    if (!Fact.ReturnsSelfAt)
      Fact.ReturnsSelfAt = recordFor(Loc, SM);
  }

  void flowsTo(const FunctionDecl *Callee, int Index, SourceLocation Loc) {
    std::optional<EntityId> Id = Extractor.addEntity(Callee);
    // A callee the entity model cannot name is one the fixpoint could never
    // read facts from, so the flow has to become an escape instead: dropping
    // the edge would make the use silently benign. This is not a corner:
    // getEntityName() refuses every FunctionDecl carrying a builtin id, so
    // every call to a C library function that the capture table does not
    // excuse lands here.
    if (!Id)
      return sink(EscapeReason::UnnamedCallee, Loc, Callee->getNameAsString());
    // try_emplace: the recorded location is the *first* call site.
    Fact.FlowsTo.try_emplace(FlowTarget{*Id, Index}, recordFor(Loc, SM));
  }

  //===--- Helpers --------------------------------------------------------===//

  /// A local automatic, non-reference variable that can hold an alias value.
  ///
  /// Parameters qualify: they are automatic variables the body may overwrite.
  /// References never do -- writing through one writes the referent, so a
  /// reference is a store target rather than storage.
  ///
  /// The init-capture exclusion here and the init-capture row in classifyUse()
  /// are one measure, not two: the row claims those declarations, and this
  /// keeps them out of the storage arms whose assertions rest on the growth
  /// pass having seen the variable -- which it never does for a declaration
  /// that lives in a closure object. Neither half is reachable while the other
  /// stands, so no test pins this exclusion on its own; removing both at once
  /// is what the assertion catches.
  static bool isLocalPointerStorage(const VarDecl *VD) {
    return VD->hasLocalStorage() && !VD->getType()->isReferenceType() &&
           !VD->hasAttr<BlocksAttr>() && !VD->hasAttr<CleanupAttr>() &&
           !VD->isInitCapture() && !isa<ImplicitParamDecl>(VD) &&
           isPointerCarryingType(VD->getType());
  }

  /// A local reference; it can alias only through its own initializer.
  static bool isLocalReference(const VarDecl *VD) {
    return VD->hasLocalStorage() && VD->getType()->isReferenceType() &&
           !VD->hasAttr<CleanupAttr>() && !VD->isInitCapture() &&
           !isa<ImplicitParamDecl>(VD);
  }

  /// One of the two trusted external sources: `noescape` written in source or
  /// applied by API Notes, on any redeclaration or in the function type.
  static bool paramIsDeclaredNoescape(const FunctionDecl *FD, unsigned I) {
    for (const FunctionDecl *R : FD->redecls())
      if (I < R->getNumParams() && R->getParamDecl(I)->hasAttr<NoEscapeAttr>())
        return true;
    if (const auto *FPT = FD->getType()->getAs<FunctionProtoType>())
      return I < FPT->getNumParams() && FPT->getExtParameterInfo(I).isNoEscape();
    return false;
  }

  /// True when `__attribute__((no_builtin))` on the *enclosing* definition
  /// disables \p Callee's builtin here.
  ///
  /// LibraryFunctionKnowledge documents this as a caller obligation: the
  /// attribute's subject is the function containing the call, which its
  /// callee-only signature cannot see. The rule mirrors CodeGen's
  /// addNoBuiltinAttributes(): an empty argument list is stored as the
  /// wildcard and disables everything. NoBuiltin is not an InheritableAttr,
  /// so -- as in CodeGen -- only the definition's own attribute counts.
  bool builtinIsDisabledHere(const FunctionDecl *Callee) const {
    const auto *NBA = Def->getAttr<NoBuiltinAttr>();
    if (!NBA)
      return false;
    if (llvm::is_contained(NBA->builtinNames(), "*"))
      return true;
    unsigned ID = Callee->getBuiltinID();
    // Not a builtin at all: LibraryFunctionKnowledge would not have trusted it
    // either, and `no_builtin` can only name builtins.
    if (!ID)
      return false;
    std::string BuiltinName = Ctx.BuiltinInfo.getName(ID);
    llvm::StringRef Name(BuiltinName);
    Name.consume_front("__builtin_");
    return llvm::is_contained(NBA->builtinNames(), Name);
  }

  /// An implicit trivial copy/move constructor or assignment operator. These
  /// are unnamable, so a flow edge to them could never be resolved; they are
  /// recognized at the call site and modelled as a copy of the value instead.
  static bool isTrivialImplicitCopyOrMove(const FunctionDecl *FD) {
    if (!FD->isImplicit() || !FD->isTrivial())
      return false;
    if (const auto *CD = dyn_cast<CXXConstructorDecl>(FD))
      return CD->isCopyOrMoveConstructor();
    if (const auto *MD = dyn_cast<CXXMethodDecl>(FD))
      return MD->isCopyAssignmentOperator() || MD->isMoveAssignmentOperator();
    return false;
  }

  static bool isDeallocationOperator(const FunctionDecl *FD) {
    OverloadedOperatorKind K = FD->getOverloadedOperator();
    return K == OO_Delete || K == OO_Array_Delete;
  }

  DynTypedNode parentOf(const Stmt *S) {
    DynTypedNodeList Parents = Ctx.getParentMapContext().getParents(*S);
    return Parents.empty() ? DynTypedNode() : Parents[0];
  }

  /// Whether \p FD, or the template pattern it came from, has a definition
  /// this TU can see that is not in a system header.
  bool definedOutsideSystem(const FunctionDecl *FD) const {
    const FunctionDecl *Pattern = FD->getTemplateInstantiationPattern();
    const FunctionDecl *Def = nullptr;
    if (!(Pattern ? Pattern : FD)->isDefined(Def) || !Def)
      return false;
    return !SM.isInSystemHeader(Def->getLocation());
  }

  /// \returns \p E when it is `{e}` initializing a scalar, which carries the
  /// value through rather than storing into a subobject, and null otherwise --
  /// an aggregate's initializer list has no value of its own.
  ///
  /// Deliberately not an alias kind on the list: a scalar list appears in the
  /// AST in a form ParentMapContext does not record a parent for, so giving it
  /// a kind makes that form a use nobody can account for, which then sinks.
  /// Both places that care -- the growth pass and the use rule -- look through
  /// it explicitly instead.
  static const InitListExpr *asScalarBraceInit(const Expr *E) {
    const auto *ILE = dyn_cast_or_null<InitListExpr>(E);
    if (!ILE || ILE->getNumInits() != 1)
      return nullptr;
    QualType T = ILE->getType();
    if (!isPointerCarryingType(T) || T->isRecordType())
      return nullptr;
    return ILE;
  }

  /// Whether \p A is the same operand as \p B. resolveCallSite() reports the
  /// argument as written, which may carry conversions the classified
  /// expression does not.
  static bool sameExpr(const Expr *A, const Expr *B) {
    return A == B || A->IgnoreParenImpCasts() == B->IgnoreParenImpCasts();
  }

  //===--- Alias kinds (design section 5.2 derivations) -------------------===//

  AliasKind kind(const Expr *E) {
    if (!E)
      return AliasKind::None;
    auto It = Memo.find(E);
    if (It != Memo.end())
      return It->second;
    AliasKind K = computeKind(E);
    Memo[E] = K;
    return K;
  }

  AliasKind kindOfVarRef(const VarDecl *VD) {
    if (!AliasVars.count(VD))
      return AliasKind::None;
    QualType T = VD->getType();
    // A reference *is* its referent: naming it denotes the referent, which is
    // either a holder of an alias value or the pointee object itself.
    if (T->isReferenceType())
      return isPointerCarryingType(T.getNonReferenceType())
                 ? AliasKind::VarLValue
                 : AliasKind::Place;
    return AliasKind::VarLValue;
  }

  AliasKind kindOfCast(const CastExpr *C) {
    AliasKind S = kind(C->getSubExpr());
    switch (C->getCastKind()) {
    case CK_LValueToRValue:
      // Loading out of storage that holds an alias yields the alias; loading
      // through a Place yields whatever the pointee happened to contain, which
      // is a different object's value (design section 1.1).
      return (S == AliasKind::VarLValue || S == AliasKind::Value)
                 ? AliasKind::Value
                 : AliasKind::None;
    case CK_ArrayToPointerDecay:
      // An interior pointer into the pointee.
      return S == AliasKind::Place ? AliasKind::Value : S;
    case CK_PointerToBoolean:
    case CK_PointerToIntegral:
    case CK_ToVoid:
    case CK_IntegralToPointer:
    case CK_NullToPointer:
    case CK_FunctionToPointerDecay:
    case CK_BuiltinFnToFnPtr:
      // The result carries no provenance. The operand's own use is still
      // classified, because a None-kinded parent never covers its child.
      return AliasKind::None;
    default:
      return (isPointerCarryingType(C->getType()) || C->isGLValue())
                 ? S
                 : AliasKind::None;
    }
  }

  AliasKind kindOfMember(const MemberExpr *ME) {
    if (!isa<FieldDecl>(ME->getMemberDecl()))
      return AliasKind::None; // a method reference: classified as a call
    switch (kind(ME->getBase())) {
    case AliasKind::None:
      return AliasKind::None;
    case AliasKind::Value:
    case AliasKind::Place:
      // A subobject of the object the alias designates. No record type is
      // field-sensitive: what a member of a *record* holds is reached by a
      // load, and a load through a place is fresh (design section 1.1).
      return AliasKind::Place;
    case AliasKind::VarLValue:
      // Unreachable today: no pointer-carrying type has members, so no member
      // access has a VarLValue base. Answered Place rather than None because
      // this is the arm that widens if the pointer-carrying population ever
      // does (see the M1 scope note in the header) -- Place keeps the address
      // of such a member a Value that sinks, where None would lose it in
      // silence, and nothing pins an arm nothing can reach.
      return AliasKind::Place;
    }
    llvm_unreachable("covered");
  }

  /// Every pointer-carrying call result computed from an alias is an alias.
  /// No annotation is consulted: this over-approximation is what makes
  /// `v.data()`, `s[i]` and `v.begin()` sound without `lifetimebound`.
  AliasKind kindOfCall(const Expr *E) {
    std::optional<CallSite> CS = resolveCallSite(E);
    if (!CS)
      return AliasKind::None;
    bool Carries = isPointerCarryingType(E->getType());
    if (!Carries && !E->isGLValue())
      return AliasKind::None;
    bool AnyAlias =
        CS->ImplicitObjectArg && kind(CS->ImplicitObjectArg) != AliasKind::None;
    for (auto [Arg, Idx] : CS->Arguments)
      AnyAlias |= kind(Arg) != AliasKind::None;
    for (const Expr *Arg : CS->UnmatchedArgs)
      AnyAlias |= kind(Arg) != AliasKind::None;
    if (!AnyAlias)
      return AliasKind::None;
    if (E->isGLValue())
      return Carries ? AliasKind::VarLValue : AliasKind::Place;
    return AliasKind::Value;
  }

  AliasKind computeKind(const Expr *E) {
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
        return kindOfVarRef(VD);
      if (const auto *BD = dyn_cast<BindingDecl>(DRE->getDecl()))
        return kind(BD->getBinding());
      return AliasKind::None;
    }
    if (isa<CXXThisExpr>(E))
      return Src.isThis() ? AliasKind::Value : AliasKind::None;
    if (const auto *PE = dyn_cast<ParenExpr>(E))
      return kind(PE->getSubExpr());
    if (const auto *FE = dyn_cast<FullExpr>(E)) // ExprWithCleanups, ConstantExpr
      return kind(FE->getSubExpr());
    if (const auto *MTE = dyn_cast<MaterializeTemporaryExpr>(E))
      return kind(MTE->getSubExpr());
    if (const auto *BTE = dyn_cast<CXXBindTemporaryExpr>(E))
      return kind(BTE->getSubExpr());
    if (const auto *DAE = dyn_cast<CXXDefaultArgExpr>(E))
      return kind(DAE->getExpr());
    if (const auto *DIE = dyn_cast<CXXDefaultInitExpr>(E))
      return kind(DIE->getExpr());
    if (const auto *OVE = dyn_cast<OpaqueValueExpr>(E))
      return kind(OVE->getSourceExpr());
    if (const auto *GSE = dyn_cast<GenericSelectionExpr>(E))
      return GSE->isResultDependent() ? AliasKind::None
                                      : kind(GSE->getResultExpr());
    if (const auto *CE = dyn_cast<ChooseExpr>(E))
      return kind(CE->getChosenSubExpr());
    if (const auto *SE = dyn_cast<StmtExpr>(E)) {
      const CompoundStmt *Body = SE->getSubStmt();
      // ValueStmt::getExprStmt() is the query Sema::BuildStmtExpr() itself uses
      // to give the statement expression its type, and it looks through the
      // LabelStmt and AttributedStmt wrappers that `({ lbl: p; })` puts in the
      // way. Asking the same question is what keeps the two in step: taking
      // body_back() as an Expr instead reports no kind for a labelled last
      // statement, the wrapped expression then reads as a discarded value under
      // the statement allow list below, and the store the statement expression
      // feeds is classified by nobody.
      const Expr *Value = nullptr;
      if (!Body->body_empty())
        if (const auto *VS = dyn_cast<ValueStmt>(Body->body_back()))
          Value = VS->getExprStmt();
      if (Value)
        return kind(Value);
      // No test pins this and none can today: the line above asks clang the
      // very question that decided this expression's type, so a
      // pointer-carrying statement expression always has a value here. It is
      // the backstop for the two drifting apart -- a pointer-carrying value
      // nobody can account for must not read as fresh. Value rather than a
      // sink because computeKind() also runs during the growth pass, where
      // recording a sink would put it out of source order.
      return isPointerCarryingType(E->getType()) ? AliasKind::Value
                                                 : AliasKind::None;
    }
    if (const auto *C = dyn_cast<CastExpr>(E))
      return kindOfCast(C);
    if (const auto *ME = dyn_cast<MemberExpr>(E))
      return kindOfMember(ME);
    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      AliasKind S = kind(UO->getSubExpr());
      switch (UO->getOpcode()) {
      case UO_AddrOf:
        // Taking the address of the pointee is an interior pointer; taking the
        // address of the *variable* is a sink, classified as a use.
        return S == AliasKind::Place ? AliasKind::Value : AliasKind::None;
      case UO_Deref:
        if (S != AliasKind::Value)
          return AliasKind::None;
        return AliasKind::Place;
      case UO_PostInc:
      case UO_PostDec:
      case UO_PreInc:
      case UO_PreDec:
        // Incrementing storage that holds an alias yields an alias.
        return S == AliasKind::VarLValue ? AliasKind::Value : AliasKind::None;
      case UO_Plus:
      case UO_Extension:
        // `__extension__ e` is a transparent wrapper. Without propagating
        // through it the wrapped alias would have no kind, and the use its
        // parent makes of it would never be classified.
        return S;
      default:
        return AliasKind::None;
      }
    }
    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      switch (BO->getOpcode()) {
      case BO_Add:
      case BO_Sub:
        if (!isPointerCarryingType(BO->getType()))
          return AliasKind::None; // pointer difference
        return (kind(BO->getLHS()) == AliasKind::Value ||
                kind(BO->getRHS()) == AliasKind::Value)
                   ? AliasKind::Value
                   : AliasKind::None;
      case BO_Assign:
        return kind(BO->getLHS());
      case BO_Comma:
        return kind(BO->getRHS());
      default:
        if (isa<CompoundAssignOperator>(BO))
          return kind(BO->getLHS());
        return AliasKind::None;
      }
    }
    if (const auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
      AliasKind T = kind(CO->getTrueExpr()), F = kind(CO->getFalseExpr());
      if (T == AliasKind::None && F == AliasKind::None)
        return AliasKind::None;
      if (!E->isGLValue())
        return AliasKind::Value;
      return T != AliasKind::None ? T : F;
    }
    if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
      AliasKind B = kind(ASE->getBase());
      return (B == AliasKind::Value || B == AliasKind::Place)
                 ? AliasKind::Place
                 : AliasKind::None;
    }
    // An Objective-C instance variable reached through the parameter is a
    // subobject of the object it designates, exactly like `a->m`.
    if (const auto *IRE = dyn_cast<ObjCIvarRefExpr>(E)) {
      AliasKind B = kind(IRE->getBase());
      return (B == AliasKind::Value || B == AliasKind::Place)
                 ? AliasKind::Place
                 : AliasKind::None;
    }
    if (isa<CallExpr, CXXConstructExpr>(E))
      return kindOfCall(E);
    return AliasKind::None;
  }

  //===--- Growth (locals that receive aliases) ---------------------------===//

  bool growByInit(const VarDecl *VD, AliasKind K) {
    if (K == AliasKind::None)
      return false;
    if (isLocalReference(VD)) {
      // Binding a reference to the pointer *variable* is an escape of the
      // variable, handled by the use rule; the reference is then an alias of
      // the storage, which this analysis does not model.
      //
      // No test pins this and none can: the initializer that would add the
      // variable is also the use that sinks AddressTaken, and that sink is
      // recorded before any use of the reference could be reached, so letting
      // it in would only add redundant flow edges to a node that has already
      // escaped.
      if (K == AliasKind::VarLValue)
        return false;
      return AliasVars.insert(VD).second;
    }
    if (isLocalPointerStorage(VD))
      return AliasVars.insert(VD).second;
    return false;
  }

  bool growByAssign(const VarDecl *VD, AliasKind K) {
    if (K == AliasKind::None || !isLocalPointerStorage(VD))
      return false;
    return AliasVars.insert(VD).second;
  }

  /// Finds the variables that can hold an alias. Flow-insensitive: a variable
  /// that ever receives an alias is treated as holding one everywhere, which
  /// over-approximates and so can only add uses to classify.
  class GrowthVisitor : public DynamicRecursiveASTVisitor {
    Classifier &C;

  public:
    bool Changed = false;
    explicit GrowthVisitor(Classifier &C) : C(C) {
      ShouldVisitImplicitCode = true;
    }
    bool TraverseDecl(Decl *D) override {
      if (D && D != C.Def &&
          isa<FunctionDecl, RecordDecl, BlockDecl, ObjCMethodDecl>(D))
        return true;
      return DynamicRecursiveASTVisitor::TraverseDecl(D);
    }
    // A lambda's or block's *body* reaches an enclosing local only through a
    // capture, and every capture of an alias is a sink, so nothing inside it
    // can grow the alias set. An init-capture's initializer is different: it
    // runs in the enclosing function, so it is traversed.
    bool TraverseLambdaExpr(LambdaExpr *LE) override {
      for (Expr *Init : LE->capture_inits())
        if (Init && !TraverseStmt(Init))
          return false;
      return true;
    }
    // Same shape as the lambda above, for the same reason: the body is skipped,
    // but a capture's *copy expression* runs in the enclosing function. See the
    // UseVisitor's TraverseBlockExpr() for what puts an entire LambdaExpr --
    // and with it an init-capture initializer this pass has to see -- inside
    // one.
    bool TraverseBlockExpr(BlockExpr *BE) override {
      for (const BlockDecl::Capture &Cap : BE->getBlockDecl()->captures())
        if (Cap.hasCopyExpr() && !TraverseStmt(Cap.getCopyExpr()))
          return false;
      return true;
    }
    bool VisitVarDecl(VarDecl *VD) override {
      if (!VD->hasInit())
        return true;
      const Expr *Init = VD->getInit();
      if (const InitListExpr *ILE = asScalarBraceInit(Init))
        Init = ILE->getInit(0);
      Changed |= C.growByInit(VD, C.kind(Init));
      return true;
    }
    bool VisitBinaryOperator(BinaryOperator *BO) override {
      if (BO->getOpcode() != BO_Assign)
        return true;
      if (const auto *DRE = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts()))
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
          Changed |= C.growByAssign(VD, C.kind(BO->getRHS()));
      return true;
    }
  };

  void growAliasSet() {
    for (;;) {
      // Kinds depend on the alias set, which just grew.
      Memo.clear();
      GrowthVisitor V(*this);
      V.TraverseDecl(const_cast<FunctionDecl *>(Def));
      if (!V.Changed)
        return;
    }
  }

  //===--- Use classification (design section 5.2 table) ------------------===//

  /// True when \p PE -- an alias-kinded parent -- already accounts for \p E's
  /// use, so E itself needs no classification.
  ///
  /// Every entry is a value-preserving derivation from computeKind(): the
  /// operand's value is consumed only to compute the parent's, which is itself
  /// an alias and is classified by *its* parent. Call arguments, implicit
  /// object arguments and assignment right-hand sides are never pass-through,
  /// which is what makes `int *q = id(p)` record a flow as well as an alias,
  /// and `*pp = pp` record a store.
  bool isCoveredByParent(const Expr *E, const Expr *PE) {
    if (kind(PE) == AliasKind::None)
      return false;
    if (isa<ParenExpr, FullExpr, MaterializeTemporaryExpr, CXXBindTemporaryExpr,
            OpaqueValueExpr, CXXDefaultArgExpr, CXXDefaultInitExpr,
            GenericSelectionExpr, ChooseExpr, CastExpr, MemberExpr,
            ObjCIvarRefExpr, ArraySubscriptExpr, AbstractConditionalOperator,
            UnaryOperator>(PE))
      return true;
    if (const auto *BO = dyn_cast<BinaryOperator>(PE)) {
      if (BO->getOpcode() == BO_Assign || isa<CompoundAssignOperator>(BO))
        return BO->getLHS() == E; // the RHS is a store and must be classified
      // Only the right operand carries the comma's value. The left one is
      // discarded, which the use rule below reports as benign, so no input
      // distinguishes this from `true` -- it states the intent rather than
      // deciding an outcome.
      if (BO->getOpcode() == BO_Comma)
        return BO->getRHS() == E;
      return true; // pointer arithmetic
    }
    return false;
  }

  class UseVisitor : public DynamicRecursiveASTVisitor {
    Classifier &C;

  public:
    explicit UseVisitor(Classifier &C) : C(C) { ShouldVisitImplicitCode = true; }
    bool TraverseDecl(Decl *D) override {
      if (D && D != C.Def &&
          isa<FunctionDecl, RecordDecl, BlockDecl, ObjCMethodDecl>(D))
        return true;
      return DynamicRecursiveASTVisitor::TraverseDecl(D);
    }
    bool TraverseLambdaExpr(LambdaExpr *LE) override {
      // Every capture also has an initializer, and the loop over those catches
      // strictly more -- a capture of a structured binding, whose captured
      // declaration is not a VarDecl, for one. No test fires this loop alone;
      // it is the backstop for a capture whose initializer is absent, and it
      // only adds sinks.
      for (const LambdaCapture &Cap : LE->captures()) {
        if (Cap.capturesThis() && C.Src.isThis())
          C.sink(EscapeReason::Capture, LE->getBeginLoc());
        if (Cap.capturesVariable())
          if (const auto *VD = dyn_cast<VarDecl>(Cap.getCapturedVar());
              VD && C.AliasVars.count(VD))
            C.sink(EscapeReason::Capture, Cap.getLocation());
      }
      for (Expr *Init : LE->capture_inits()) {
        if (!Init)
          continue;
        if (C.kind(Init) != AliasKind::None)
          C.sink(EscapeReason::Capture, Init->getBeginLoc());
        // An init-capture's initializer is evaluated in the *enclosing*
        // function, so it is ordinary code and its subexpressions are ordinary
        // uses: `[n = record(p)]` passes p to record() before any capture
        // happens. Checking only the initializer's own kind would see nothing
        // there, because the initializer is an `int`.
        if (!TraverseStmt(Init))
          return false;
      }
      // The *body* is not traversed: it can reach an enclosing local only
      // through a capture, and every capture of an alias has just sunk.
      return true;
    }
    // The subobject a constructor initializer builds is reached before the
    // initializer expression's own uses, which is where it happens in source
    // order too -- and first-sink-wins reads that order.
    bool TraverseConstructorInitializer(CXXCtorInitializer *Init) override {
      if (Init && C.Src.isThis())
        C.classifySubobjectConstruction(Init->getInit(),
                                        Init->getSourceLocation());
      return DynamicRecursiveASTVisitor::TraverseConstructorInitializer(Init);
    }
    bool TraverseBlockExpr(BlockExpr *BE) override {
      const BlockDecl *BD = BE->getBlockDecl();
      for (const BlockDecl::Capture &Cap : BD->captures())
        if (C.AliasVars.count(Cap.getVariable()))
          C.sink(EscapeReason::Capture, BE->getBeginLoc());
      if (BD->capturesCXXThis() && C.Src.isThis())
        C.sink(EscapeReason::Capture, BE->getBeginLoc());
      // The block's *body* is not traversed, for the same reason a lambda's is
      // not: it reaches an enclosing local only through a capture, and every
      // capture of an alias has just sunk. A capture's copy expression is a
      // different thing entirely -- it is evaluated in the *enclosing*
      // function, in terms of the captured variable -- and skipping it was
      // unsound.
      //
      // The shape that makes it unsound is a C++ lambda converted to a block.
      // Clang does not leave the LambdaExpr where it was written: it wraps the
      // conversion in a BlockExpr whose BlockDecl captures a synthetic
      // temporary of the *closure's* type, and hangs the LambdaExpr off that
      // capture's copy expression. The loop above then asks whether that
      // temporary is an alias -- it never is -- and the lambda's own captures,
      // which are where the parameter actually goes, sat behind a `return
      // true`. `Blk b = [p]{ ... }; g_blk = b;` reported no sink, no flow and
      // no return: the shape that means "provably does not escape".
      //
      // Recorded after the direct captures so that first-sink-wins still
      // reports the capture itself where there is one, rather than whatever
      // the copy expression does with the same variable.
      for (const BlockDecl::Capture &Cap : BD->captures())
        if (Cap.hasCopyExpr() && !TraverseStmt(Cap.getCopyExpr()))
          return false;
      return true;
    }
    // DynamicRecursiveASTVisitor has no VisitExpr.
    bool VisitStmt(Stmt *S) override {
      const auto *E = dyn_cast<Expr>(S);
      if (!E)
        return true;
      AliasKind K = C.kind(E);
      if (K == AliasKind::None)
        return true;
      DynTypedNode P = C.parentOf(E);
      if (const auto *PE = P.get<Expr>(); PE && C.isCoveredByParent(E, PE))
        return true;
      C.classifyUse(E, K, P);
      return true;
    }
  };

  /// Classify a store of an alias into \p Target.
  void classifyStoreInto(const Expr *Target, SourceLocation Loc) {
    const Expr *T = Target->IgnoreParenImpCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(T)) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (isLocalPointerStorage(VD)) {
          // Benign only because the growth pass put the variable in the alias
          // set, so that what is stored here is classified at the variable's
          // own uses. Assert it rather than assume it: when the two passes
          // disagree about what writes a local, this arm is where the escape
          // would be dropped.
          assert(AliasVars.count(VD) &&
                 "a local that receives an alias must have joined the alias "
                 "set during the growth pass");
          return;
        }
        // Writing through a reference writes whatever it was bound to, which
        // this analysis does not track.
        if (VD->getType()->isReferenceType())
          return sink(EscapeReason::StoreThroughPointer, Loc,
                      VD->getNameAsString());
        if (VD->hasAttr<BlocksAttr>())
          return sink(EscapeReason::Capture, Loc);
        if (VD->hasAttr<CleanupAttr>())
          return sink(EscapeReason::AddressTaken, Loc);
        return sink(EscapeReason::StoreToGlobal, Loc, VD->getNameAsString());
      }
      // Not a variable (a non-type template parameter, say). Unpinned, and the
      // sink direction is the safe one.
      return sink(EscapeReason::StoreThroughPointer, Loc);
    }
    if (const auto *ME = dyn_cast<MemberExpr>(T)) {
      return sink(EscapeReason::StoreToField, Loc,
                  ME->getMemberDecl()->getNameAsString());
    }
    return sink(EscapeReason::StoreThroughPointer, Loc);
  }

  /// Classify \p M's use as an argument or implicit object of \p Call.
  void classifyCallUse(const Expr *M, AliasKind K, const Expr *Call) {
    SourceLocation Loc = M->getBeginLoc();
    std::optional<CallSite> CS = resolveCallSite(Call);
    if (!CS || !CS->Callee)
      return sink(EscapeReason::IndirectCall, Loc);
    const FunctionDecl *Callee = CS->Callee;
    // No callee parameter to attribute the flow to. resolveCallSite() reports
    // an argument as unmatched for a variadic tail, an unprototyped callee, an
    // arity mismatch and a static member operator's object expression alike;
    // only a prototyped variadic callee can have a variadic tail, so that is
    // what separates the VarArgs row from the rest.
    if (llvm::any_of(CS->UnmatchedArgs,
                     [&](const Expr *A) { return sameExpr(A, M); }))
      return sink(Callee->isVariadic() ? EscapeReason::VarArgs
                                       : EscapeReason::UnmatchedArgument,
                  Loc);
    // `noescape` also forbids deallocating through the parameter, so a
    // deallocator is refused even where LLVM proves it does not capture. This
    // row and the two benign rows below consume the same table with opposite
    // polarities: there, not recognizing the callee must withhold trust; here,
    // not recognizing it must not withhold the refusal. isDeallocationFunction
    // is ungated for that reason -- a body that frees through the pointer is
    // indistinguishable from one that merely writes through it.
    if (isDeallocationOperator(Callee) ||
        LibraryFunctionKnowledge::isDeallocationFunction(Callee, Ctx))
      return sink(EscapeReason::Deallocation, Loc);
    const auto *MD = dyn_cast<CXXMethodDecl>(Callee);
    if (CS->ImplicitObjectArg && sameExpr(CS->ImplicitObjectArg, M)) {
      // The body that runs is the override's, which this TU need not contain.
      if (MD && MD->isVirtual())
        return sink(EscapeReason::VirtualCall, Loc);
      // A trivial copy or move reads the object, or -- as the left operand of
      // `v2 = v1` -- overwrites it. Neither leaks the object itself, and
      // overwriting is benign for the same reason that `out = p` is benign for
      // a reference parameter `out`: the analysis is flow-insensitive and goes
      // on treating the old value as live.
      if (isTrivialImplicitCopyOrMove(Callee))
        return;
      return flowsTo(Callee, ThisParamIndex, Loc);
    }
    for (auto [Arg, Idx] : CS->Arguments) {
      if (!sameExpr(Arg, M))
        continue;
      // `std::move`/`std::forward` and friends only re-type their operand; the
      // result refers to the same object, kindOfCall() makes it an alias, and
      // its own uses are classified. Recording a flow instead would name a
      // callee the entity model refuses -- clang models these as builtins --
      // so the use would degrade to UnnamedCallee and no parameter that is
      // ever moved or forwarded could be annotated.
      if (lifetimes::isStdReferenceCast(Callee) && !definedOutsideSystem(Callee))
        return;
      if (paramIsDeclaredNoescape(Callee, Idx))
        return;
      if (!builtinIsDisabledHere(Callee) &&
          LibraryFunctionKnowledge::parameterDoesNotEscape(Callee, Idx, Ctx))
        return;
      if (isTrivialImplicitCopyOrMove(Callee)) {
        if (K == AliasKind::Place)
          return; // copying the pointee by value is a load
        return classifyStoreInto(
            CS->ImplicitObjectArg ? CS->ImplicitObjectArg : Call, Loc);
      }
      if (MD && MD->isVirtual())
        return sink(EscapeReason::VirtualCall, Loc);
      // A parameter outside the analyzed population has no node to flow into,
      // and an edge recorded against one cannot be resolved: the callee's
      // summary holds no fact for it, so whatever the callee does with it is
      // lost rather than deferred. That matters even where the *type* was
      // refused on scope grounds -- `void keeps(V &v) { g_pv = &v; }` is an
      // ordinary pointer escape, and its caller must not be told the argument
      // merely flows somewhere. Refusing a type stays reject-only only because
      // this row turns its uses into sinks.
      if (!isPointerCarryingType(Callee->getParamDecl(Idx)->getType()))
        return sink(EscapeReason::UnrecognizedUse, Loc,
                    "callee parameter outside the analyzed population");
      return flowsTo(Callee, Idx, Loc);
    }
    // M is nested inside an argument without being the argument: the argument
    // expression's own kind covers it.
  }

  /// Classify \p M's use as a member, base or delegating initializer of \p CD.
  ///
  /// ParentMapContext does not model CXXCtorInitializer, so the parent of a
  /// member initializer's expression is the constructor itself.
  void classifyCtorInitUse(const Expr *M, const CXXConstructorDecl *CD) {
    SourceLocation Loc = M->getBeginLoc();
    for (const CXXCtorInitializer *CI : CD->inits()) {
      if (CI->getInit() != M)
        continue;
      // A delegating initializer's own expression is a CXXConstructExpr of
      // the class type, and its arguments are classified as an ordinary call
      // (DelegatingInitializerIsAnOrdinaryCall) while the object it builds is
      // recorded for `this` by classifySubobjectConstruction().
      //
      // No test pins this arm and none can today: reaching it needs that
      // CXXConstructExpr to have an alias kind of its own, and kindOfCall()
      // gives a prvalue one only when its type is pointer-carrying -- which no
      // record type is since #12 took tracked views out. Weakening it alone to
      // a sink leaves the whole suite green, which is how that was measured.
      // It is kept rather than deleted because it is the arm that decides the
      // shape if the pointer-carrying population ever grows again (#34), and
      // because the alternative, falling through to the row below, would claim
      // that delegating to another constructor stores into a field.
      if (CI->isDelegatingInitializer())
        return;
      return sink(EscapeReason::StoreToField, Loc);
    }
    // An expression whose parent is a constructor but which is none of its
    // initializers -- a default member initializer evaluated here, for
    // instance. Unmodelled, so it sinks.
    sink(EscapeReason::UnrecognizedUse, Loc, "constructor initializer");
  }

  /// Record the implicit object argument of a constructor that one of the
  /// analyzed definition's own constructor initializers runs.
  ///
  /// A base, member or delegating initializer calls a constructor on a
  /// subobject of `*this` -- on `*this` itself, when delegating -- and design
  /// section 1.1 counts a pointer to any subobject of the designated object as
  /// derived from the source. The AST spells no operand for it: a
  /// CXXConstructExpr carries arguments but no object expression, so
  /// resolveCallSite() reports no ImplicitObjectArg and the use rules, which
  /// only ever classify expressions, can never see it. Design section 6's
  /// first invariant makes classification exhaustive over *expressions*, so
  /// an implicit object argument with no expression falls outside it and has
  /// to be recorded here rather than in classifyUse().
  ///
  /// Constructor initializers are one of exactly two such arguments. The
  /// other is implicit subobject *destruction*, which has no AST node at all
  /// -- not even a CXXConstructExpr to find -- and which
  /// classifySubobjectDestruction() answers.
  ///
  /// Only for the `this` source. A *parameter* reaches the constructed
  /// subobject only through the initializer's arguments, which the ordinary
  /// call rules classify, and through the initializer's own expression, which
  /// classifyCtorInitUse() answers.
  ///
  /// The arms below are computeKind()'s, restricted to the ones that can
  /// deliver a *newly constructed* object rather than name an existing one.
  /// Deliberately omitted, each because the object it yields is not the
  /// subobject being initialized:
  ///
  ///  - the arms that denote an existing object -- DeclRefExpr, MemberExpr,
  ///    ArraySubscriptExpr, UnaryOperator, assignment, pointer arithmetic.
  ///    Initializing a member from one of those copies it, and the copy is a
  ///    CXXConstructExpr wrapping them, which the last arm answers.
  ///  - CXXDefaultArgExpr, which cannot be an initializer's own expression.
  ///  - MaterializeTemporaryExpr and OpaqueValueExpr. A materialized temporary
  ///    is never a subobject of `*this`, so an arm that descended into one
  ///    would record a constructor that ran on something else. Measured on the
  ///    one shape that puts them in this position, the GNU `a ?: b`: its true
  ///    branch initializes the member by *copying* the materialized temporary,
  ///    and that copy is a CXXConstructExpr the last arm answers -- skipped
  ///    when trivial, recorded against the copy constructor when not.
  ///    SubobjectConstructionValueShapes asserts exactly that, and asserts
  ///    that the temporary's own constructor is *not* recorded.
  ///  - CallExpr. `: m(makeM())` builds the member through the callee's
  ///    return slot, and the analysis models no return slot anywhere (#38).
  ///    This is *not* the same approximation as `M m = makeM();` in a
  ///    statement, though the two are answered the same way: there the return
  ///    slot is a local and `this` is genuinely unaffected, while here the
  ///    return slot is a subobject of `*this`, so `this` genuinely escapes if
  ///    the callee keeps a pointer to what it builds, and nothing records it.
  void classifySubobjectConstruction(const Expr *E, SourceLocation InitLoc) {
    if (!E)
      return;
    if (const auto *PE = dyn_cast<ParenExpr>(E))
      return classifySubobjectConstruction(PE->getSubExpr(), InitLoc);
    if (const auto *FE = dyn_cast<FullExpr>(E)) // ExprWithCleanups
      return classifySubobjectConstruction(FE->getSubExpr(), InitLoc);
    if (const auto *DIE = dyn_cast<CXXDefaultInitExpr>(E))
      return classifySubobjectConstruction(DIE->getExpr(), InitLoc);
    // A prvalue of a type with a destructor keeps its CXXBindTemporaryExpr
    // here even where the copy is elided and the constructor builds the
    // subobject in place. Looking through it can only add edges, and an
    // argument -- the one place a genuine temporary appears -- is never
    // reached, because no arm below descends into one.
    if (const auto *BTE = dyn_cast<CXXBindTemporaryExpr>(E))
      return classifySubobjectConstruction(BTE->getSubExpr(), InitLoc);
    // `Leak(6)` with an argument list is a functional cast around the
    // construction, where `Leak()` is a CXXTemporaryObjectExpr on its own.
    // Only CK_ConstructorConversion: it alone names a constructor, and it
    // constructs the object this initializer initializes.
    if (const auto *CE = dyn_cast<CastExpr>(E);
        CE && CE->getCastKind() == CK_ConstructorConversion)
      return classifySubobjectConstruction(CE->getSubExpr(), InitLoc);
    // An aggregate member or an array member initializes its own elements,
    // each of which is a subobject of `*this` in turn.
    if (const auto *ILE = dyn_cast<InitListExpr>(E)) {
      for (const Expr *Init : ILE->inits())
        classifySubobjectConstruction(Init, InitLoc);
      return classifySubobjectConstruction(ILE->getArrayFiller(), InitLoc);
    }
    if (const auto *PLIE = dyn_cast<CXXParenListInitExpr>(E)) {
      for (const Expr *Init : PLIE->getInitExprs())
        classifySubobjectConstruction(Init, InitLoc);
      return;
    }
    if (const auto *AILE = dyn_cast<ArrayInitLoopExpr>(E))
      return classifySubobjectConstruction(AILE->getSubExpr(), InitLoc);
    // Both branches: the analysis is flow-insensitive, and either may run.
    // AbstractConditionalOperator rather than ConditionalOperator so that the
    // GNU `a ?: b` spelling is answered by the same arm; its true branch is
    // reached through the OpaqueValueExpr and MaterializeTemporaryExpr arms
    // below.
    if (const auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
      classifySubobjectConstruction(CO->getTrueExpr(), InitLoc);
      return classifySubobjectConstruction(CO->getFalseExpr(), InitLoc);
    }
    // Only the right operand carries the comma's value; the left one is
    // discarded, and any construction in it builds a temporary.
    if (const auto *BO = dyn_cast<BinaryOperator>(E);
        BO && BO->getOpcode() == BO_Comma)
      return classifySubobjectConstruction(BO->getRHS(), InitLoc);
    // Asked exactly as computeKind() asks it, and for the same reason:
    // ValueStmt::getExprStmt() is the query Sema::BuildStmtExpr() used to give
    // the statement expression its type, so it looks through the LabelStmt and
    // AttributedStmt wrappers that `({ lbl: M(); })` puts in the way. Taking
    // body_back() as an Expr instead would miss a labelled last statement.
    if (const auto *SE = dyn_cast<StmtExpr>(E)) {
      const CompoundStmt *Body = SE->getSubStmt();
      if (!Body->body_empty())
        if (const auto *VS = dyn_cast<ValueStmt>(Body->body_back()))
          return classifySubobjectConstruction(VS->getExprStmt(), InitLoc);
      return;
    }
    if (const auto *CE = dyn_cast<ChooseExpr>(E))
      return classifySubobjectConstruction(CE->getChosenSubExpr(), InitLoc);
    if (const auto *GSE = dyn_cast<GenericSelectionExpr>(E)) {
      // Mirrors computeKind()'s guard, and is unpinned there for the same
      // reason: a dependent selection has no result to look at, and a
      // definition in a dependent context is one this classifier is not asked
      // about.
      if (GSE->isResultDependent())
        return;
      return classifySubobjectConstruction(GSE->getResultExpr(), InitLoc);
    }
    const auto *CCE = dyn_cast<CXXConstructExpr>(E);
    if (!CCE)
      return;
    const CXXConstructorDecl *Callee = CCE->getConstructor();
    // A trivial constructor runs no code that could keep the object: a trivial
    // default constructor performs no initialization at all, and a trivial
    // copy or move constructor copies the *source* object's bytes into the
    // subobject without ever forming a durable pointer to the destination.
    // Recognized here for the same reason isTrivialImplicitCopyOrMove() is
    // recognized at an ordinary call site -- these constructors are also the
    // ones the entity model most often cannot name, so flowing into them would
    // degrade to UnnamedCallee and make `this` escape out of every aggregate
    // member.
    if (Callee->isTrivial())
      return;
    SourceLocation Loc = CCE->getBeginLoc();
    if (Loc.isInvalid())
      Loc = InitLoc;
    // Not a virtual call: a constructor is never virtual, so the body that
    // runs is always this one. Arguments are left to the ordinary rules.
    flowsTo(Callee, ThisParamIndex, Loc);
  }

  /// Record the implicit object argument of every destructor \p DD runs on one
  /// of its own subobjects.
  ///
  /// The mirror of classifySubobjectConstruction(), and the second of the two
  /// implicit object arguments the use rules can never see -- this one with
  /// less to go on, because a base's or a member's destruction has no AST node
  /// at all, not even a call expression. The subobjects are read off the class
  /// instead.
  ///
  /// Unlike construction, this direction is reachable from an ordinary
  /// candidate parameter today: `h->~Holder()` is a CXXMemberCallExpr, so
  /// classifyCallUse() records `flowsTo(~Holder, ThisParamIndex)` from a
  /// parameter, and `~Holder`'s own fact is what the fixpoint then reads. A
  /// `~Holder` that reports clean with no edges is the shape that means
  /// "provably does not escape", so a base destructor that publishes the
  /// object it is destroying would let the caller's parameter be annotated
  /// `noescape` -- which CodeGen lowers to `captures(none)`. That spelling is
  /// what every pool allocator and slab uses; libc++'s own `__destroy_at` is
  /// `p->~T()`.
  ///
  /// A destructor has no parameters, so `this` is its only source.
  ///
  /// The edges recorded here are only as good as the entity model's ability to
  /// name the destructor at the other end, and an *implicit* destructor is
  /// usually unnamable: `struct A { ~A(); }; struct B { A a; }; struct X { B
  /// b; ~X() { } };` gives `~X` a sink of UnnamedCallee rather than a flow,
  /// because `~B` is implicit. That is the safe direction -- it reports an
  /// escape where a flow would have deferred the question -- but it means the
  /// chain stays precise only while every intermediate class declares its
  /// destructor. Widening what the entity model can name is not this
  /// function's to do.
  void classifySubobjectDestruction(const CXXDestructorDecl *DD) {
    const CXXRecordDecl *RD = DD->getParent();
    if (!RD || !RD->hasDefinition())
      return;
    RD = RD->getDefinition();

    auto record = [&](QualType T, SourceLocation Loc) {
      // An array member is destroyed element by element; the element type is
      // what carries the destructor.
      const CXXRecordDecl *Sub =
          Ctx.getBaseElementType(T)->getAsCXXRecordDecl();
      // Unreachable on well-formed input -- a base or a member of class type
      // must be complete for the enclosing class to be defined -- but this
      // extractor runs as an ASTConsumer whatever the diagnostics said, and
      // clang's recovery keeps a FieldDecl whose type is still incomplete. All
      // of `Inc m;`, `Inc m[2];` and `U<int> m;` reach here, and with this
      // line removed getDefinition() answers null and hasTrivialDestructor()
      // dereferences it: the process dies rather than the suite going red.
      // IncompleteSubobjectsAreSkipped is what fires it.
      if (!Sub || !Sub->hasDefinition())
        return;
      Sub = Sub->getDefinition();
      // A trivial destructor runs no code, by the same argument that lets
      // classifySubobjectConstruction() skip a trivial constructor -- and it
      // is likewise the shape the entity model most often cannot name, so
      // flowing into it would degrade to UnnamedCallee and make `this` escape
      // out of every scalar member.
      if (Sub->hasTrivialDestructor())
        return;
      // A separate claim from the one above, and a weaker one: this guard is
      // pinned by nothing and no input has been found that fires it.
      // getDestructor() declares the implicit destructor on demand, and a
      // class that reaches this line has a non-trivial -- hence existing --
      // one, so null would need a deleted, inaccessible or otherwise invalid
      // destructor that hasTrivialDestructor() nonetheless called non-trivial.
      // Incomplete, deleted, private, unnamed and C++20 constrained
      // destructors with no eligible candidate were all measured and none
      // reaches it. It stays because returning is the direction that cannot
      // record an edge to nothing.
      const CXXDestructorDecl *SubDD = Sub->getDestructor();
      if (!SubDD)
        return;
      // A base subobject's destructor is called non-virtually even when it is
      // virtual, so the body that runs is this one and a flow edge is exact.
      flowsTo(SubDD, ThisParamIndex, Loc.isValid() ? Loc : DD->getLocation());
    };

    // Direct bases, then all virtual bases -- a direct virtual base is in
    // both, and the duplicate edge is dropped by try_emplace.
    for (const CXXBaseSpecifier &B : RD->bases())
      record(B.getType(), B.getBeginLoc());
    for (const CXXBaseSpecifier &B : RD->vbases())
      record(B.getType(), B.getBeginLoc());
    // A union's variant members are *not* destroyed implicitly, so an edge
    // recorded for one describes a call that never happens. No arm excludes
    // them: an edge too many can only report an escape that cannot occur,
    // while an arm that excluded them would be one more thing to get right,
    // and a union holding a non-trivially-destructible member needs a
    // user-written destructor that destroys it by hand anyway.
    for (const FieldDecl *F : RD->fields())
      record(F->getType(), F->getLocation());
  }

  /// The table of design section 5.2, read top to bottom. \p M denotes
  /// something with respect to the source; \p P is its parent node.
  void classifyUse(const Expr *M, AliasKind K, const DynTypedNode &P) {
    SourceLocation Loc = M->getBeginLoc();

    if (const auto *RS = P.get<ReturnStmt>())
      return recordReturn(RS->getBeginLoc());
    if (const auto *CD = P.get<CXXConstructorDecl>())
      return classifyCtorInitUse(M, CD);
    if (const auto *VD = P.get<VarDecl>()) {
      // An init-capture's variable lives in the closure object, not in this
      // function, so initializing it is a capture rather than a store into
      // local storage. The two storage predicates exclude it for the same
      // reason, which is what the assertions below rest on -- the growth pass
      // never visits these declarations.
      //
      // No test can pin this row on its own: it is reached only when the
      // initializer has an alias kind of its own, which is exactly when
      // TraverseLambdaExpr() has already recorded the same Capture sink. It is
      // here so that the classification of this use does not depend on that
      // ordering, and so that the storage predicates above have somewhere to
      // hand the declarations they exclude.
      if (VD->isInitCapture())
        return sink(EscapeReason::Capture, Loc);
      if (isLocalReference(VD)) {
        // Binding a reference to the pointer variable exposes the storage;
        // binding it to the pointee is an ordinary alias derivation.
        if (K == AliasKind::VarLValue)
          return sink(EscapeReason::AddressTaken, Loc);
        assert(AliasVars.count(VD) && "a local reference bound to an alias must "
                                      "have joined the alias set");
        return;
      }
      if (isLocalPointerStorage(VD)) {
        assert(AliasVars.count(VD) && "a local initialized with an alias must "
                                      "have joined the alias set");
        return; // the variable joined the alias set
      }
      if (VD->hasAttr<BlocksAttr>())
        return sink(EscapeReason::Capture, Loc);
      if (VD->hasAttr<CleanupAttr>())
        return sink(EscapeReason::AddressTaken, Loc);
      if (VD->hasGlobalStorage())
        return sink(EscapeReason::StoreToGlobal, Loc, VD->getNameAsString());
      // A local of a type that cannot hold an alias, initialized from one --
      // a C aggregate copy of the pointee, say. Unmodelled, so it sinks.
      return sink(EscapeReason::UnrecognizedUse, Loc, "variable initializer");
    }
    // A structured binding's own binding expression defines the alias rather
    // than using it.
    if (P.get<BindingDecl>())
      return;
    if (P.get<GCCAsmStmt>())
      return sink(EscapeReason::Asm, Loc);
    // Statement parents are an allow list: these read or discard the value.
    // Anything else -- an ObjC fast enumeration, an OpenMP captured region --
    // reaches the default sink below rather than passing for a discarded
    // value.
    if (P.get<CompoundStmt>() || P.get<IfStmt>() || P.get<SwitchStmt>() ||
        P.get<WhileStmt>() || P.get<DoStmt>() || P.get<ForStmt>() ||
        P.get<CXXForRangeStmt>() || P.get<SwitchCase>() ||
        P.get<LabelStmt>() || P.get<AttributedStmt>())
      return;
    if (const auto *PS = P.get<Stmt>(); PS && !P.get<Expr>())
      return sink(EscapeReason::UnrecognizedUse, Loc, PS->getStmtClassName());

    const Expr *PE = P.get<Expr>();
    // No parent at all: the expression is outside anything the parent map
    // built, so nothing can say what is done with it.
    if (!PE)
      return sink(EscapeReason::UnrecognizedUse, Loc, "no parent");

    if (isa<CXXThrowExpr>(PE))
      return sink(EscapeReason::Throw, Loc);
    if (isa<CXXNewExpr>(PE))
      return sink(EscapeReason::HeapAllocation, Loc);
    if (isa<CXXDeleteExpr>(PE))
      return sink(EscapeReason::Deallocation, Loc);
    if (const auto *ILE = dyn_cast<InitListExpr>(PE)) {
      // `int *q = {p}` is `int *q = p`: the list is transparent, so the use is
      // whatever is done with the list itself -- an initialization of local
      // storage here, an argument or a return elsewhere.
      if (asScalarBraceInit(ILE))
        return classifyUse(ILE, K, parentOf(ILE));
      return sink(EscapeReason::StoreToField, Loc);
    }
    if (isa<DesignatedInitExpr, CXXParenListInitExpr>(PE))
      return sink(EscapeReason::StoreToField, Loc);
    if (isa<ObjCMessageExpr>(PE))
      return sink(EscapeReason::ObjCMessage, Loc);
    // Unevaluated operands: the value is never formed. This covers only an
    // alias that is a *direct* child, which is the whole of `sizeof(*p)` but
    // almost none of `noexcept(f(p))` -- there the call is classified on its
    // own and records a flow that cannot happen at run time. That is the safe
    // direction, and this row must not be read as covering it.
    if (isa<UnaryExprOrTypeTraitExpr, CXXNoexceptExpr>(PE))
      return;
    if (const auto *C = dyn_cast<CastExpr>(PE)) {
      switch (C->getCastKind()) {
      case CK_LValueToRValue:
      case CK_PointerToBoolean:
      case CK_ToVoid:
        return;
      default:
        return sink(EscapeReason::CastToNonPointer, Loc, C->getCastKindName());
      }
    }
    if (const auto *ME = dyn_cast<MemberExpr>(PE)) {
      if (isa<CXXMethodDecl>(ME->getMemberDecl())) {
        DynTypedNode GP = parentOf(ME);
        if (const auto *MCE = GP.get<CXXMemberCallExpr>())
          return classifyCallUse(M, K, MCE);
        // Forming a pointer to member of an alias object. Unpinned; the sink
        // direction is the safe one.
        return sink(EscapeReason::CallableUse, Loc);
      }
      return; // reading a field that carries no pointer
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(PE)) {
      if (UO->getOpcode() == UO_AddrOf)
        return sink(EscapeReason::AddressTaken, Loc);
      // `!p` reads the value and yields a bool. C++ converts the operand with
      // a CK_PointerToBoolean cast, which the cast row above answers, but C
      // has no such cast and the alias is the direct operand.
      if (UO->getOpcode() == UO_LNot)
        return;
      // A read-modify-write *through* a place -- `(*p)++`, `++p[i]` -- writes
      // the pointee and yields a value loaded from it, neither of which is the
      // pointer that designates it. (When the operand is storage rather than a
      // place the result is an alias, so the operand is covered by its parent
      // and never reaches this row.)
      if (UO->isIncrementDecrementOp())
        return;
    }
    if (const auto *BO = dyn_cast<BinaryOperator>(PE)) {
      if (BO->getOpcode() == BO_Assign && BO->getRHS() == M)
        return classifyStoreInto(BO->getLHS(), Loc);
      if (BO->isComparisonOp() || BO->isLogicalOp() ||
          (BO->getOpcode() == BO_Comma && BO->getLHS() == M) ||
          (BO->getOpcode() == BO_Sub && !isPointerCarryingType(BO->getType())))
        return; // isLogicalOp: `p && q` in C, for the same reason as `!p`
      return sink(EscapeReason::UnrecognizedUse, Loc,
                  BO->getOpcodeStr().str());
    }
    // The condition of `p ? a : b`, again a value read that C spells without a
    // conversion. Only the condition: a branch is the conditional's value, and
    // a conditional whose value is an alias covers its branches as a
    // pass-through parent instead of reaching here.
    if (const auto *CO = dyn_cast<AbstractConditionalOperator>(PE);
        CO && CO->getCond() == M)
      return;
    if (const auto *CE = dyn_cast<CallExpr>(PE)) {
      if (sameExpr(CE->getCallee(), M))
        return sink(EscapeReason::CallableUse, Loc);
      return classifyCallUse(M, K, CE);
    }
    if (isa<CXXConstructExpr>(PE)) {
      if (parentOf(PE).get<CXXNewExpr>())
        return sink(EscapeReason::HeapAllocation, Loc);
      return classifyCallUse(M, K, PE);
    }
    return sink(EscapeReason::UnrecognizedUse, Loc, PE->getStmtClassName());
  }
};

} // namespace

FunctionEscapeFacts
clang::ssaf::classifyFunctionEscapes(const FunctionDecl *Def, ASTContext &Ctx,
                                     TUSummaryExtractor &Extractor) {
  FunctionEscapeFacts Facts;
  const SourceManager &SM = Ctx.getSourceManager();
  const auto *MD = dyn_cast<CXXMethodDecl>(Def);
  bool HasThis = MD && MD->isInstance();

  // A coroutine's parameters are copied into the coroutine frame, which
  // outlives the call, and the body the classifier would walk is the
  // transformed one. Nothing here is analyzable, so everything sinks.
  if (isa_and_nonnull<CoroutineBodyStmt>(Def->getBody())) {
    Sink S{EscapeReason::Coroutine, recordFor(Def->getBody()->getBeginLoc(), SM),
           ""};
    for (const ParmVarDecl *P : Def->parameters())
      if (isPointerCarryingType(P->getType())) {
        EscapeFact F;
        F.OtherSink = S;
        Facts.Params[P->getFunctionScopeIndex()] = std::move(F);
      }
    if (HasThis) {
      EscapeFact F;
      F.OtherSink = S;
      Facts.This = std::move(F);
    }
    return Facts;
  }

  Classifier C(Def, Ctx, Extractor);
  // A fact for every pointer-carrying parameter, clean or not: CandidateParams
  // is a subset of these keys, and a candidate parameter without a fact would
  // be dropped with an "empty" summary.
  for (const ParmVarDecl *P : Def->parameters())
    if (isPointerCarryingType(P->getType()))
      Facts.Params[P->getFunctionScopeIndex()] = C.analyze(Source{P});
  if (HasThis)
    Facts.This = C.analyze(Source{nullptr});
  return Facts;
}
