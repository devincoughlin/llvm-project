//===- InferNoescape.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Where an annotation may be written.
//
// A proven parameter is identified by (function EntityId, parameter index).
// The attribute has to reach *every* declaration site of that function in
// this translation unit, because in C++ the noescape attribute is part of the
// function type: a redeclaration that keeps the unannotated type is a
// `conflicting types` error, not a warning. In C the redeclarations merge to
// a composite type without the attribute, which silently loses the guarantee
// for that translation unit. Both outcomes make a *partial* annotation the
// dangerous one.
//
// Every site is resolved as FD->getParamDecl(I)->getBeginLoc() -- the
// ParmVarDecl's own written location. The site is never resolved through the
// declaration's TypeSourceInfo or a FunctionTypeLoc: for `typedef void
// FT(int *); FT f;` that type location lies inside the typedef, and an edit
// there would annotate every function declared through FT.
//
// A site can fail to be writable for eight reasons, each of which skips that
// site and emits one noescape-skipped warning:
//
//  1. There is no parameter at the index. An unprototyped C declaration
//     (`void f();`) carries none; any other occurrence means the result file
//     does not describe this source.
//  2. The parameter's type cannot carry the attribute. Sema's
//     handleNoEscapeAttr diagnoses those and does *not* add the attribute, so
//     an edit here produces -Wignored-attributes in every translation unit
//     that sees the declaration -- an error under -Werror -- and the site is
//     not idempotent on a later run. Candidacy never proposes one; this is
//     the same "the result file does not describe this source" case as (1).
//  3. The parameter already carries a NoEscapeAttr, but one that is not
//     written in this declaration's bytes -- API Notes supply the attribute
//     to the AST without editing the header. An attribute genuinely written
//     here is the one skip that stays silent, because the source is already
//     annotated and the remaining sites still get their edits.
//  4. The declaration has no written prototype (a K&R definition, whose
//     ParmVarDecls sit in the declaration list after the parenthesis rather
//     than in a parameter list).
//  5. The ParmVarDecl is implicit: the declaration was written through a
//     function typedef and has no written parameter list at all. Clang
//     synthesizes the ParmVarDecl at the function's own name.
//  6. The begin location is inside a macro expansion. More than one
//     parameter can share one expansion location -- `#define PARAMS int *p,
//     int n` gives both of them the same one -- so the expansion location
//     does not identify *this* parameter, and inserting there could annotate
//     a different one than the result named.
//  7. The begin location is inside a system header, which the pipeline does
//     not own.
//  8. The location cannot be turned into an applicable Replacement.
//
// Templated entities are excluded before any of this, and exclusion covers
// template specializations as well as dependent contexts: an explicit
// function-template specialization and an out-of-line member specialization
// are both non-dependent and do have a written parameter list.
//
// A parameter whose sites are only partially writable is still annotated at
// the writable ones, and each unwritable one is reported. Withholding the
// writable insertions is not better: it would silently drop the annotation
// for a header that is a system header in this translation unit and a
// project header in the one that does edit it (design section 5.5). The
// all-or-nothing obligation lives one stage earlier, in candidacy, which
// vetoes a function with an unrewritable redeclaration visible from the
// defining translation unit, and one stage later, in the pipeline policy
// that refuses to apply a merged edit set in which a noescape-skipped site
// has no noescape-inserted result at the same location.
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/Sarif.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/SSAFOptions.h"
#include "clang/Lex/Lexer.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"
#include "clang/ScalableStaticAnalysis/Core/ASTEntityMapping.h"
#include "clang/ScalableStaticAnalysis/Core/Model/BuildNamespace.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityIdTable.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/SourceTransformation/TransformationRegistry.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::ssaf;

namespace {

/// The rejections of one function, in Node order.
using RejectionList =
    std::vector<std::pair<Node, const NonEscapingParametersResult::Rejection *>>;

std::string usrOf(const FunctionDecl *FD) {
  llvm::SmallString<128> Buf;
  if (index::generateUSRForDecl(FD, Buf))
    return "<unknown>";
  return Buf.str().str();
}

class NoescapeRewriter : public DynamicRecursiveASTVisitor {
public:
  NoescapeRewriter(ASTContext &Ctx, const NonEscapingParametersResult &Result,
                   const std::map<EntityName, EntityId> &NameToId,
                   const std::map<EntityId, RejectionList> &RejectedByFunction,
                   NestedBuildNamespace TUNs, NestedBuildNamespace LUNs,
                   llvm::StringRef Spelling, SourceEditEmitter &Edits,
                   TransformationReportEmitter &Report)
      : Ctx(Ctx), SM(Ctx.getSourceManager()), Result(Result),
        NameToId(NameToId), RejectedByFunction(RejectedByFunction),
        TUNs(std::move(TUNs)), LUNs(std::move(LUNs)), Spelling(Spelling),
        Edits(Edits), Report(Report) {}

  bool VisitFunctionDecl(FunctionDecl *FD) override {
    // M1 candidacy excludes templated entities; a pattern has no single
    // instantiated parameter list to annotate and an instantiation has no
    // written one. isTemplated() alone is isDependentContext(), which is
    // false for an explicit function-template specialization and for an
    // out-of-line member specialization -- both of which do have a written
    // parameter list and would otherwise be edited, although their parameter
    // types must match the primary template, so an added attribute is a hard
    // error rather than a mismatch. Mirrors design section 5.2's wording.
    if (FD->isTemplated() ||
        FD->getTemplateSpecializationKind() != TSK_Undeclared)
      return true;
    std::optional<EntityName> Name = getQualifiedEntityName(FD, TUNs, LUNs);
    if (!Name)
      return true;
    auto IdIt = NameToId.find(*Name);
    if (IdIt == NameToId.end())
      return true;
    const EntityId Id = IdIt->second;

    auto NonEscapingIt = Result.NonEscaping.find(Id);
    const bool HasRejections =
        FD->doesThisDeclarationHaveABody() &&
        RejectedByFunction.find(Id) != RejectedByFunction.end();
    if (NonEscapingIt == Result.NonEscaping.end() && !HasRejections)
      return true;

    const std::string USR = usrOf(FD);
    if (NonEscapingIt != Result.NonEscaping.end())
      for (unsigned Index : NonEscapingIt->second)
        annotate(FD, Index, USR);

    // A rejection is reported once per link unit, at the definition, so the
    // precision-tuning feed does not carry one copy per redeclaration. This
    // gates on the body alone, so an inline function defined in a system
    // header has its note ranged at a system-header location -- the one place
    // this transformation names one. It is a note, never an edit, so the
    // section 5.4 system-header policy does not apply to it.
    if (HasRejections)
      for (const auto &[N, Rej] : RejectedByFunction.find(Id)->second)
        reportRejected(FD, N, *Rej, USR);
    return true;
  }

private:
  void annotate(FunctionDecl *FD, unsigned Index, llvm::StringRef USR) {
    const std::string Site =
        llvm::formatv("entity={0} param={1}", USR, Index).str();

    // (1) No parameter at this index: an unprototyped C declaration, or a
    // result file that does not describe this source. Either way there is
    // nothing here to annotate, and annotating some other index would be
    // worse than annotating nothing.
    if (Index >= FD->getNumParams()) {
      reportSkipped(
          Lexer::getAsCharRange(FD->getSourceRange(), SM, Ctx.getLangOpts()),
          llvm::formatv("no parameter at index {0} in this declaration", Index)
              .str(),
          Site);
      return;
    }
    ParmVarDecl *P = FD->getParamDecl(Index);
    const CharSourceRange Range = rangeOf(P);

    // (2) A type the attribute cannot apply to. The predicate is clang's own,
    // called through the same public entry point Sema's handleNoEscapeAttr
    // uses, so the two cannot drift: an index that would produce
    // -Wignored-attributes is unrepresentable as an edit here rather than
    // trusted from upstream. Record types are accepted, so `struct S s` is
    // annotated as before.
    if (!isValidPointerAttrType(P->getType(), /*RefOkay=*/true) &&
        !P->getType()->isRecordType()) {
      reportSkipped(Range,
                    "the parameter's type cannot carry the attribute, so the "
                    "edit would be diagnosed and dropped by every compilation "
                    "that sees it",
                    Site);
      return;
    }

    // Idempotent, but decided on the source text rather than on the AST. The
    // attribute is not inherited across redeclarations, so this is decided
    // per site.
    if (const auto *A = P->getAttr<NoEscapeAttr>()) {
      if (isWrittenIn(A, FD))
        return;
      // (3) The attribute is in the AST but not in the bytes: API Notes
      // inject a NoEscapeAttr with an empty SourceRange (SemaAPINotes.cpp's
      // getPlaceholderAttrInfo), so a header can read as annotated here while
      // carrying nothing. Returning silently would drop a required edit with
      // no report at all, which is the one outcome section 5.5's pipeline
      // backstop cannot refuse -- it would have nothing to refuse on.
      reportSkipped(Range,
                    "the parameter is noescape in this compilation but the "
                    "attribute is not written in this declaration (API Notes, "
                    "for example, supply it without editing the header)",
                    Site);
      return;
    }

    // (4) K&R definition: the ParmVarDecls are in the declaration list, not
    // in a parameter list, so an insertion there would not produce a
    // prototype.
    if (!FD->hasWrittenPrototype()) {
      reportSkipped(Range, "the declaration has no written prototype", Site);
      return;
    }

    // (5) Declared through a function typedef: clang synthesized this
    // ParmVarDecl at the function's own name and there is no written
    // parameter list to insert into.
    if (P->isImplicit()) {
      reportSkipped(Range,
                    "the declaration has no written parameter list (it is "
                    "written through a function type name)",
                    Site);
      return;
    }

    const SourceLocation Loc = P->getBeginLoc();

    // (6) More than one parameter can share one expansion location: for
    // `#define PARAMS int *p, int n` both parameters expand at PARAMS, so the
    // expansion location does not identify *this* parameter and inserting
    // there could annotate the wrong one. Measured. (Note that "the edit
    // would land in the macro definition" is *not* the reason, and would be
    // false for a prefix macro such as `#define CONSTQ const` used as
    // `void f(CONSTQ int *p)`, whose expansion location is an ordinary
    // project-header location -- do not narrow this guard on that argument.)
    if (Loc.isMacroID()) {
      reportSkipped(Range, "the parameter begins in a macro expansion", Site);
      return;
    }

    // (7) The pipeline does not own system headers.
    if (SM.isInSystemHeader(Loc)) {
      reportSkipped(Range, "the parameter is declared in a system header",
                    Site);
      return;
    }

    tooling::Replacement R(SM, Loc, 0, (Spelling + " ").str());
    // (8) Defensive final net, matching CppBoundedBuffers: a location with no
    // file behind it yields a Replacement with an empty path, which must
    // never reach the edit file. No test reaches it, because no input was
    // found that can: a parameter in <command line> or a scratch buffer is a
    // macro ID and is caught by (4) above, and `clang -x c++ -c -` gives
    // <stdin> a FileEntryRef, so even that produces applicable edits.
    if (!R.isApplicable()) {
      reportSkipped(Range, "the edit location does not resolve to a file",
                    Site);
      return;
    }
    Edits.addReplacement(std::move(R));
    Report.addResult(NoescapeInsertedRuleId, SarifResultLevel::Note, Range,
                     llvm::formatv("inserted {0} ({1})", Spelling, Site).str());
  }

  void reportSkipped(CharSourceRange Range, llvm::StringRef Why,
                     llvm::StringRef Site) {
    Report.addResult(
        NoescapeSkippedRuleId, SarifResultLevel::Warning, Range,
        llvm::formatv("cannot insert noescape here: {0}; a redeclaration "
                      "edited elsewhere may now mismatch ({1})",
                      Why, Site)
            .str());
  }

  void reportRejected(FunctionDecl *FD, const Node &N,
                      const NonEscapingParametersResult::Rejection &Rej,
                      llvm::StringRef USR) {
    // ThisParamIndex is representable in a rejection Node but names no
    // written parameter, so it has no site to report against.
    // Compared in the signed domain on purpose: casting ParamIndex to
    // unsigned first would fold the two rejections into one, with -1 caught
    // only incidentally by wrapping to UINT_MAX.
    if (N.ParamIndex < 0 ||
        N.ParamIndex >= static_cast<int>(FD->getNumParams()))
      return;
    const std::string DetailStr =
        Rej.Detail.empty() ? std::string() : " (" + Rej.Detail + ")";
    const std::string BlameStr =
        Rej.Blame ? llvm::formatv(" via callee parameter {0}",
                                  Rej.Blame->ParamIndex)
                        .str()
                  : std::string();
    Report.addResult(
        NoescapeRejectedRuleId, SarifResultLevel::Note,
        rangeOf(FD->getParamDecl(static_cast<unsigned>(N.ParamIndex))),
        llvm::formatv("parameter {0} of {1} not annotated: {2} at {3}:{4}:{5}"
                      "{6}{7}",
                      N.ParamIndex, USR, escapeReasonName(Rej.Reason),
                      Rej.Location.FilePath, Rej.Location.Line,
                      Rej.Location.Column, DetailStr, BlameStr)
            .str());
  }

  /// The reporting range for \p P, never invalid.
  ///
  /// Lexer::getAsCharRange cannot measure the last token when the parameter's
  /// end sits inside a macro expansion that continues past it -- `#define
  /// PARAMS int *p, int n` is enough -- and the SARIF writer drops a result
  /// whose range is invalid, silently. A located skip is what section 5.5's
  /// policy is keyed on, so degrade to a zero-width range at the expansion
  /// location rather than to no location at all.
  CharSourceRange rangeOf(const ParmVarDecl *P) const {
    CharSourceRange R =
        Lexer::getAsCharRange(P->getSourceRange(), SM, Ctx.getLangOpts());
    if (R.isValid())
      return R;
    const SourceLocation Loc = SM.getExpansionLoc(P->getBeginLoc());
    return CharSourceRange::getCharRange(Loc, Loc);
  }

  /// Whether \p A is written in the bytes of \p D's own declaration.
  ///
  /// Containment is tested at expansion locations and against the whole
  /// declaration rather than against the parameter, because two written forms
  /// fall outside a ParmVarDecl's own SourceRange and both were measured: an
  /// attribute after the declarator (`void f(int *p NS_NOESCAPE)`), whose
  /// expansion follows the parameter's end, and a macro-written one, which is
  /// spelled in the macro definition and only expands at the site.
  bool isWrittenIn(const Attr *A, const Decl *D) const {
    // One validity test for the attribute, not two overlapping ones: an
    // earlier `AttrRange.isInvalid()` early return would be shadowed by this
    // one, since getExpansionLoc maps an invalid location to itself, and a
    // guard that cannot be weakened on its own is pinned by nothing.
    const SourceLocation AttrLoc = SM.getExpansionLoc(A->getRange().getBegin());
    if (AttrLoc.isInvalid())
      return false; // API Notes: in the AST, not in the bytes.
    const CharSourceRange DeclRange = Lexer::getAsCharRange(
        SM.getExpansionRange(D->getSourceRange()), SM, Ctx.getLangOpts());
    // Defensive, like isApplicable() below: no input was found that makes a
    // function declaration's expansion range unmeasurable.
    if (DeclRange.isInvalid())
      return false;
    return SM.isPointWithin(AttrLoc, DeclRange.getBegin(), DeclRange.getEnd());
  }

  ASTContext &Ctx;
  const SourceManager &SM;
  const NonEscapingParametersResult &Result;
  const std::map<EntityName, EntityId> &NameToId;
  const std::map<EntityId, RejectionList> &RejectedByFunction;
  NestedBuildNamespace TUNs;
  NestedBuildNamespace LUNs;
  llvm::StringRef Spelling;
  SourceEditEmitter &Edits;
  TransformationReportEmitter &Report;
};

} // namespace

namespace clang::ssaf {

void InferNoescape::HandleTranslationUnit(ASTContext &Ctx) {
  llvm::Expected<const NonEscapingParametersResult &> Result =
      Suite.get<NonEscapingParametersResult>();
  if (!Result) {
    llvm::consumeError(Result.takeError());
    return;
  }

  std::map<EntityName, EntityId> NameToId;
  Suite.getIdTable().forEach([&NameToId](const EntityName &Name, EntityId Id) {
    NameToId.emplace(Name, Id);
  });

  // std::map over a std::map keeps the emission order of the reports a
  // function of the result, not of the order declarations happen to be
  // visited in.
  std::map<EntityId, RejectionList> RejectedByFunction;
  for (const auto &[N, Rej] : Result->Rejected)
    RejectedByFunction[N.Function].push_back({N, &Rej});

  const llvm::StringRef Spelling =
      Opts.NoescapeSpelling.empty() ? llvm::StringRef(DefaultNoescapeSpelling)
                                    : llvm::StringRef(Opts.NoescapeSpelling);

  NoescapeRewriter(
      Ctx, *Result, NameToId, RejectedByFunction,
      NestedBuildNamespace::makeCompilationUnit(Opts.CompilationUnitId),
      NestedBuildNamespace::makeLinkUnit(Opts.LinkUnitId), Spelling, Edits,
      Report)
      .TraverseDecl(Ctx.getTranslationUnitDecl());
}

// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int InferNoescapeAnchorSource = 0;

} // namespace clang::ssaf

static clang::ssaf::TransformationRegistry::Add<clang::ssaf::InferNoescape>
    RegisterInferNoescape(
        "infer-noescape",
        "Inserts noescape on parameters proven non-escaping");
