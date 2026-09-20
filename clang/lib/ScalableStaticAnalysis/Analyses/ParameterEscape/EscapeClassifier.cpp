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
#include "clang/AST/StmtCXX.h"
#include "clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/Support/Casting.h"

#include <utility>

using namespace clang;
using namespace clang::ssaf;

static bool hasSwiftNonEscapableAttr(const CXXRecordDecl *RD) {
  for (const auto *A : RD->specific_attrs<SwiftAttrAttr>())
    if (A->getAttribute() == "~Escapable")
      return true;
  return false;
}

bool clang::ssaf::isViewLikeRecordType(QualType T) {
  T = T.getNonReferenceType();
  const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
  // hasDefinition() is crash-preventing, not merely conservative:
  // getDefinition() below returns null for an incomplete record, and both this
  // function and isTrackedViewType() then dereference it. No test pins it,
  // because neither predicate can reach an incomplete record -- such a type can
  // only be a parameter by pointer or reference, and both return earlier -- but
  // it must not be read as a rule that could simply be dropped.
  if (!RD || !RD->hasDefinition())
    return false;
  return lifetimes::isGslPointerType(T) ||
         hasSwiftNonEscapableAttr(RD->getDefinition());
}

bool clang::ssaf::isTrackedViewType(QualType T) {
  if (!isViewLikeRecordType(T))
    return false;
  const CXXRecordDecl *RD =
      T.getNonReferenceType()->getAsCXXRecordDecl()->getDefinition();
  // hasTrivialDestructor() is implied by isTriviallyCopyable() -- a class with
  // a non-trivial destructor is never trivially copyable -- so it can never
  // reject on its own. It is spelled out because the design states both
  // requirements, and because a view whose destructor runs could stash the
  // pointer even if copying it were trivial.
  return RD->isTriviallyCopyable() && RD->hasTrivialDestructor();
}

bool clang::ssaf::isPointerCarryingType(QualType T) {
  T = T.getCanonicalType();
  if (T->isReferenceType())
    return true;
  // isAnyPointerType() also covers ObjC object pointers, which are analyzed
  // (their facts feed callers) even though they are never annotated.
  if (T->isAnyPointerType() && !T->isFunctionPointerType())
    return true;
  if (T->isBlockPointerType())
    return true;
  return isTrackedViewType(T);
}

bool clang::ssaf::isCandidateParameterType(QualType T) {
  T = T.getCanonicalType();
  if (T->isReferenceType())
    // A reference to a function denotes no object, so `noescape` on it would
    // say nothing. Excluding it also removes an unexplained asymmetry with the
    // function pointer rejected three lines below.
    return !T->isFunctionReferenceType();
  // Neither ObjC object pointers nor block pointers are PointerType, so this
  // admits object pointers only; function pointers are excluded explicitly.
  if (T->isPointerType())
    return !T->isFunctionPointerType();
  return isTrackedViewType(T);
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

/// Detail recorded on every stub sink, so a stub fact is distinguishable from
/// one the real classifier produced.
static constexpr llvm::StringLiteral StubDetail = "classifier stub";

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

// Sound placeholder until the real classifier lands: every pointer-carrying
// parameter, and `this`, escapes. This over-approximates, so no annotation can
// be inferred while the stub is in the tree.
FunctionEscapeFacts
clang::ssaf::classifyFunctionEscapes(const FunctionDecl *Def, ASTContext &Ctx,
                                     TUSummaryExtractor &) {
  FunctionEscapeFacts Facts;
  const SourceManager &SM = Ctx.getSourceManager();
  bool IsCoroutine = isa_and_nonnull<CoroutineBodyStmt>(Def->getBody());
  EscapeReason R =
      IsCoroutine ? EscapeReason::Coroutine : EscapeReason::UnrecognizedUse;
  Sink S{R, recordFor(Def->getLocation(), SM), StubDetail.str()};

  for (const ParmVarDecl *P : Def->parameters()) {
    if (!isPointerCarryingType(P->getType()))
      continue;
    EscapeFact F;
    F.OtherSink = S;
    Facts.Params[P->getFunctionScopeIndex()] = std::move(F);
  }
  if (const auto *MD = dyn_cast<CXXMethodDecl>(Def); MD && MD->isInstance()) {
    EscapeFact F;
    F.OtherSink = S;
    Facts.This = std::move(F);
  }
  return Facts;
}
