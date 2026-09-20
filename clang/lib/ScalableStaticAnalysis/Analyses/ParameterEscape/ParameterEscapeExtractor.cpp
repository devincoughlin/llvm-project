//===- ParameterEscapeExtractor.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EscapeClassifier.h"
#include "SSAFAnalysesCommon.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/ASTEntityMapping.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/ExtractorRegistry.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryBuilder.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryExtractor.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
using namespace clang;
using namespace clang::ssaf;

class ParameterEscapeTUSummaryExtractor : public TUSummaryExtractor {
  /// What is known about one entity of the model, accumulated across every
  /// contributor group that maps onto it.
  ///
  /// Distinct functions can share one EntityName -- and so one EntityId --
  /// without ever appearing in one contributor group: multiversioned variants
  /// are separate groups, two `enable_if` overloads with identical parameter
  /// types share a USR, and a C USR carries no parameter types at all.
  /// extractAndAddSummaries keeps whichever summary arrives first and only
  /// logs the drop, so without this the surviving facts silently describe one
  /// of several differing bodies.
  struct EntityRecord {
    /// The summary handed to the builder and kept by it, or null while every
    /// summary for this entity has been empty -- extractAndAddSummaries drops
    /// those, so their addresses must not be retained.
    ParameterEscapeSummary *Retained = nullptr;
    /// Every definition of this entity in this TU, in whatever order the
    /// contributor groups arrived. The post-pass takes the minimum by source
    /// position, so what it picks does not depend on that order.
    llvm::SmallVector<const FunctionDecl *, 2> Definitions;
    /// Whether more than one contributor group, or more than one body within
    /// one group, has been seen under this identity.
    bool Collides = false;
  };

  /// Keyed on EntityName rather than EntityId so that looking an entity up
  /// does not register it: addEntity() would add id_table and linkage_table
  /// rows for definitions that contribute no summary at all. std::map, so the
  /// post-pass below runs in a reproducible order, and so that the pointers
  /// handed out here stay valid as later entities are inserted.
  std::map<EntityName, EntityRecord> Entities;

  /// A strict weak order on definitions by source position.
  ///
  /// isBeforeInTranslationUnit() asserts on an invalid location, so one has to
  /// be handled here. A definition without a location sorts *after* one with a
  /// location, rather than merely never comparing less: that keeps the order
  /// total, and a total order is what makes the minimum below independent of
  /// the sequence the definitions were collected in. No definition without a
  /// location has been observed, so nothing pins this.
  static bool isLexicallyBefore(const FunctionDecl *A, const FunctionDecl *B,
                                const SourceManager &SM) {
    SourceLocation LA = SM.getExpansionLoc(A->getLocation());
    SourceLocation LB = SM.getExpansionLoc(B->getLocation());
    if (LA.isInvalid())
      return false;
    if (LB.isInvalid())
      return true;
    return SM.isBeforeInTranslationUnit(LA, LB);
  }

  /// The definition earliest in the translation unit.
  ///
  /// A minimum rather than a running "keep the first one that is earlier":
  /// the result is a function of the set, not of the order it was built in,
  /// which is what makes the output reproducible when contributor iteration
  /// order is not.
  static const FunctionDecl *
  lexicallyFirst(llvm::ArrayRef<const FunctionDecl *> Defs,
                 const SourceManager &SM) {
    assert(!Defs.empty() && "no definition to choose between");
    return *llvm::min_element(
        Defs, [&SM](const FunctionDecl *A, const FunctionDecl *B) {
          return isLexicallyBefore(A, B, SM);
        });
  }

public:
  using TUSummaryExtractor::TUSummaryExtractor;

  /// \return a non-null unique pointer to a ParameterEscapeSummary. The
  /// summary is empty (and therefore dropped by extractAndAddSummaries) when
  /// the entity contributes no definition with a pointer-carrying parameter.
  std::unique_ptr<ParameterEscapeSummary>
  extractEntitySummary(const std::vector<const NamedDecl *> &Decls,
                       ASTContext &Ctx) {
    auto S = std::make_unique<ParameterEscapeSummary>();
    const SourceManager &SM = Ctx.getSourceManager();

    // Register the entity before anything can return. A group of nothing but
    // bodiless declarations still has to be counted: when this TU declares one
    // overload it does not define and defines another that collides with it,
    // the definition's facts describe only one of the two functions that will
    // share the EntityId, and nothing else would notice.
    EntityRecord *Rec = nullptr;
    if (std::optional<EntityName> Name = getEntityName(Decls[0])) {
      auto [It, Inserted] = Entities.try_emplace(*Name);
      Rec = &It->second;
      if (!Inserted)
        Rec->Collides = true;
    }

    // Facts belong to the definition. A bodiless declaration contributes
    // nothing: a summary for it would shadow the definition's facts under the
    // linker's first-contributor-wins merge.
    llvm::SmallVector<const FunctionDecl *, 2> GroupDefs;
    for (const NamedDecl *D : Decls)
      if (const auto *FD = dyn_cast<FunctionDecl>(D);
          FD && FD->doesThisDeclarationHaveABody())
        GroupDefs.push_back(FD);

    if (Rec) {
      // Two bodies in one redeclaration chain is a redefinition error, so this
      // is reachable only on an AST that contains errors -- which this
      // extractor runs on.
      if (GroupDefs.size() > 1)
        Rec->Collides = true;
      Rec->Definitions.append(GroupDefs.begin(), GroupDefs.end());
    }

    if (GroupDefs.empty())
      return S;
    // Which body the facts come from is only ever observable for a group with
    // exactly one definition, where this is that definition: a group holding
    // more than one set Collides above, so its summary is rebuilt by the
    // post-pass and these facts are discarded.
    const FunctionDecl *Def = lexicallyFirst(GroupDefs, SM);

    S->IsCandidate = isCandidateDefinition(Def, Ctx);
    for (const ParmVarDecl *P : Def->parameters())
      if (isCandidateParameterType(P->getType()))
        S->CandidateParams.insert(P->getFunctionScopeIndex());

    FunctionEscapeFacts Facts = classifyFunctionEscapes(Def, Ctx, *this);
    S->Params = std::move(Facts.Params);
    S->This = std::move(Facts.This);

    // Every candidate parameter type is pointer-carrying, and the classifier
    // records a fact for every pointer-carrying parameter. That invariant is
    // what makes it safe for extractAndAddSummaries to drop a summary whose
    // `empty()` is true: such a summary has no candidate parameter either, so
    // no annotation could ever have come from it.
    assert(llvm::all_of(S->CandidateParams,
                        [&S](unsigned I) { return S->Params.count(I) == 1; }) &&
           "a candidate parameter without an escape fact would be silently "
           "dropped with the summary");

    // Record the address only for a summary that will actually be retained.
    // extractAndAddSummaries drops -- and so destroys -- any summary whose
    // empty() is true, and the post-pass below would then write through a
    // dangling pointer. The colliding summaries are not always empty together:
    // a C USR carries no parameter types (`void f(int x)` and `void f(int *p)`
    // are both `c:@F@f`), and this extractor runs on ASTs that contain errors,
    // so one can be empty while the other is not. The entity is recorded above
    // either way, so the collision is still seen.
    //
    // CollidingDefinitionsOfDifferingEmptiness reaches that state, but only
    // when the empty group is processed first, which depends on contributor
    // iteration order. Dropping `!S->empty()` is therefore caught by
    // *repeated* runs of the scoped suite rather than by any single one:
    // measured over 20 runs, 9 segfault under
    // DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib and 7 fail without it,
    // against 0 of 20 with the condition in place. A single green run is not
    // evidence that this condition can go.
    if (Rec && !Rec->Retained && !S->empty())
      Rec->Retained = S.get();

    return S;
  }

  void HandleTranslationUnit(ASTContext &Ctx) override {
    extractAndAddSummaries(
        *this, SummaryBuilder, Ctx,
        [&](const std::vector<const NamedDecl *> &Decls) {
          return extractEntitySummary(Decls, Ctx);
        },
        "ParameterEscape");

    // One degradation site, run once every contributor has been seen. Doing it
    // here rather than as each collision is discovered means the result does
    // not depend on which colliding group arrived first: the retained summary
    // is rebuilt from the lexically first definition either way.
    const SourceManager &SM = Ctx.getSourceManager();
    for (auto &[Name, Rec] : Entities) {
      // A non-null Retained implies a non-empty Definitions: the append above
      // runs before the early return that Retained is assigned past.
      if (!Rec.Collides || !Rec.Retained)
        continue;
      degradeToMultipleDefinitions(*Rec.Retained,
                                   lexicallyFirst(Rec.Definitions, SM), Ctx);
    }
  }
};
} // namespace

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int ParameterEscapeExtractorAnchorSource = 0;
} // namespace clang::ssaf

static TUSummaryExtractorRegistry::Add<ParameterEscapeTUSummaryExtractor>
    RegisterExtractor(ParameterEscapeSummary::Name,
                      "Extract per-parameter escape facts for noescape "
                      "inference");
