# Cross-TU `noescape` Inference (M1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Infer `noescape` for pointer/reference/view parameters across translation units and emit it as clang-reforge source edits, soundly (anything unmodeled is an escape).

**Architecture:** A new SSAF analysis family `ParameterEscape`: a per-TU extractor classifies every use of every parameter alias into benign / flows-to-callee-parameter / sink; a whole-program fixpoint closes `FlowsTo` edges; a transformation inserts `__attribute__((noescape))` on every redeclaration. A shared call-site resolver lands first and `PointerFlow` migrates to it.

**Tech Stack:** C++17, clang AST (`DynamicRecursiveASTVisitor`, `ParentMapContext`), SSAF Core (`TUSummaryExtractor`, `SummaryAnalysis`, `DerivedAnalysis`, `Transformation`, JSON `FormatInfo`), gtest, lit/FileCheck. LLVM's `TargetLibraryInfo`/`BuildLibCalls` are used **only in a unit test** to cross-check a static libcall table.

**Spec:** `devincoughlin/features/noescape-inference/specs/2026-09-19-cross-tu-noescape-inference-design.md` — read it first; every rule below argues from it. Version: v2 (revised after an adversarial review of v1; see "Revision notes" at the end).

## Global Constraints

- Soundness: the use classifier's default case is a sink; only two external sources are trusted — declared `noescape` (source or API Notes; read from any redeclaration's `ParmVarDecl` attribute or the `FunctionProtoType::ExtParameterInfo`) and a static table of C library functions whose parameters LLVM's libcall inference marks `captures(none)`, excluding deallocation/reallocation functions. The table is verified against LLVM by a unit test.
- No reliance on `lifetimebound`. Every pointer-carrying call result with an alias argument or alias implicit object is an alias.
- Loads through a non-view alias are fresh values. Field sensitivity only for *tracked views*: `isGslPointerType(T) || swift_attr("~Escapable")`, trivially copyable and trivially destructible.
- Reference-typed variables (including reference parameters) are **never** local alias storage for stores: writing through one writes the referent. They join the alias set only by their own initialization.
- M1 candidates: non-virtual, non-templated (pattern, instantiation, or specialization), non-coroutine, prototyped C/C++ function definitions whose every redeclaration visible in the defining TU is outside system headers, macro expansions, and dependent contexts, and whose candidate-typed parameters begin outside macros. Candidate parameter types: object pointers (not function/block/ObjC-object pointers), references, tracked views.
- Node identity is `(function EntityId, parameter index)` with `ThisParamIndex == -1`.
- Default spelling is `__attribute__((noescape))` in every language; one `--ssaf-noescape-spelling` value per link unit.
- Determinism: `std::map`/`std::set` everywhere in results; stable JSON key order.
- The `PointerFlow` migration is behavior-preserving: existing `clang/test/Analysis/Scalable/PointerFlow` and `PointerFlow*` unit tests pass with zero output diff.
- Build directory is `build/` (RelWithDebInfo, assertions on, Ninja). Unit test binary: `build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests` (if absent after building, `find build -name ClangScalableAnalysisTests -type f`). Lit runner: `build/bin/llvm-lit`. Assertions are ON, so registry lookups of unknown names abort rather than fail gracefully.
- Commit after every task; every commit message ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

---

## File structure

| Path | Responsibility |
|---|---|
| `clang/include/clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h`, `clang/lib/ScalableStaticAnalysis/Analyses/CallSiteResolution.cpp` | `CallSite` + `resolveCallSite` (Task 1); consumed by `PointerFlow` (Task 2) and the escape classifier |
| `clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h`, `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.cpp` | Summary data types, `EscapeReason` names |
| `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeJSON.h`, `ParameterEscapeFormat.cpp` | JSON helpers + `FormatInfo` for the summary |
| `clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h`, `.../LibraryFunctionKnowledge.cpp` | Static libcall capture table (no LLVM IR dependency) |
| `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.h`, `.cpp` | Alias derivation + use classification for one function definition |
| `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractor.cpp` | Candidacy, per-definition driver, registration |
| `clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h`, `.../ParameterEscapeAnalysis.cpp` | `Node`, results, `ParameterEscapeAnalysis`, `NonEscapingParametersAnalysis`, result JSON |
| `clang/include/clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h`, `clang/lib/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.cpp` | The `infer-noescape` transformation |
| `clang/include/clang/Frontend/SSAFOptions.h`, `clang/include/clang/Options/Options.td`, `clang/lib/Driver/ToolChains/Clang.cpp` | `--ssaf-noescape-spelling=` |
| `clang/include/clang/ScalableStaticAnalysis/BuiltinAnchorSources.def` | Force-linker anchors |
| `clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt`, `clang/lib/ScalableStaticAnalysis/SourceTransformation/CMakeLists.txt`, `clang/unittests/ScalableStaticAnalysis/CMakeLists.txt`, `clang/test/CMakeLists.txt` | Build wiring |
| `clang/unittests/ScalableStaticAnalysis/Analyses/CallSiteResolverTest.cpp`, `.../Analyses/ParameterEscape/*Test.cpp`, `.../WholeProgramAnalysis/NonEscapingParametersAnalysisTest.cpp`, `.../SourceTransformation/InferNoescapeTest.cpp`, `.../Serialization/JSONFormatTest/ParameterEscapeFormatTest.cpp` | Unit tests |
| `clang/test/Analysis/Scalable/ParameterEscape/`, `clang/test/Analysis/Scalable/source-edit-generation/infer-noescape-*.test` | Lit tests |
| `clang/utils/ssaf/validate_escape_corpus.py`, `clang/utils/ssaf/noescape_exit_report.py` | Corpus validator, M1 exit report |
| `clang/docs/ScalableStaticAnalysis/user-docs/NoescapeInference.md` | User docs |

---

### Task 1: Shared call-site resolver

**Files:**
- Create: `clang/include/clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h`
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/CallSiteResolution.cpp`
- Modify: `clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt` (add `CallSiteResolution.cpp`)
- Test: `clang/unittests/ScalableStaticAnalysis/Analyses/CallSiteResolverTest.cpp`
- Modify: `clang/unittests/ScalableStaticAnalysis/CMakeLists.txt` (add the test file)

**Interfaces:**
- Produces:
  ```c++
  namespace clang::ssaf {
  struct CallSite {
    const FunctionDecl *Callee = nullptr;          // null ⇒ unresolvable
    const Expr *ImplicitObjectArg = nullptr;       // member calls, member operator calls
    llvm::SmallVector<std::pair<const Expr *, unsigned>, 8> Arguments;
    llvm::SmallVector<const Expr *, 4> UnmatchedArgs;
  };
  std::optional<CallSite> resolveCallSite(const Stmt *S); // nullopt if S is not a call/construct
  }
  ```

- [ ] **Step 1: Write the failing tests**

`clang/unittests/ScalableStaticAnalysis/Analyses/CallSiteResolverTest.cpp`:

```c++
#include "clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ssaf;

namespace {

template <typename StmtT> const StmtT *findFirst(ASTContext &Ctx) {
  struct Finder : DynamicRecursiveASTVisitor {
    const StmtT *Found = nullptr;
    bool VisitStmt(Stmt *S) override {
      if (!Found)
        Found = dyn_cast<StmtT>(S);
      return Found == nullptr;
    }
  } F;
  F.TraverseDecl(Ctx.getTranslationUnitDecl());
  return F.Found;
}

struct Parsed {
  std::unique_ptr<ASTUnit> AST;
  ASTContext &ctx() { return AST->getASTContext(); }
};

Parsed parseCXX(StringRef Code) {
  return {tooling::buildASTFromCodeWithArgs(Code, {"-std=c++23"})};
}
Parsed parseC(StringRef Code) {
  return {tooling::buildASTFromCodeWithArgs(
      Code, {"-x", "c", "-std=c17", "-Wno-deprecated-non-prototype"})};
}

TEST(CallSiteResolver, FreeCallPairsArgumentsByPosition) {
  Parsed P = parseCXX("void g(int *, int *); void f(int *a, int *b) { g(a, b); }");
  auto CS = resolveCallSite(findFirst<CallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  EXPECT_EQ(CS->Callee->getNameAsString(), "g");
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 2u);
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_EQ(CS->Arguments[1].second, 1u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, MemberCallExposesImplicitObject) {
  Parsed P = parseCXX("struct S { void m(int *); }; void f(S s, int *p) { s.m(p); }");
  auto CS = resolveCallSite(findFirst<CXXMemberCallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  EXPECT_EQ(CS->Callee->getNameAsString(), "m");
  ASSERT_NE(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 1u);
  EXPECT_EQ(CS->Arguments[0].second, 0u);
}

TEST(CallSiteResolver, MemberOperatorCallShiftsFirstArgument) {
  Parsed P = parseCXX("struct S { int *operator+(int *); }; void f(S s, int *p) { s + p; }");
  auto CS = resolveCallSite(findFirst<CXXOperatorCallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 1u);
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, FreeOperatorCallDoesNotShift) {
  Parsed P = parseCXX("struct S {}; int *operator+(S, int *); void f(S s, int *p) { s + p; }");
  auto CS = resolveCallSite(findFirst<CXXOperatorCallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 2u);
  EXPECT_EQ(CS->Arguments[1].second, 1u);
}

TEST(CallSiteResolver, ExplicitObjectMemberOperatorPairsFromZero) {
  Parsed P = parseCXX("struct S { int *operator()(this S, int *p); }; void f(S s, int *p) { s(p); }");
  auto CS = resolveCallSite(findFirst<CXXOperatorCallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 2u);
  EXPECT_EQ(CS->Arguments[0].second, 0u); // the explicit object parameter
  EXPECT_EQ(CS->Arguments[1].second, 1u);
}

TEST(CallSiteResolver, ConstructorPairsArguments) {
  Parsed P = parseCXX("struct S { S(int *, int); }; void f(int *p) { S s(p, 1); }");
  auto CS = resolveCallSite(findFirst<CXXConstructExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  EXPECT_TRUE(isa<CXXConstructorDecl>(CS->Callee));
  ASSERT_EQ(CS->Arguments.size(), 2u);
  EXPECT_EQ(CS->Arguments[0].second, 0u);
}

TEST(CallSiteResolver, VariadicTailIsUnmatched) {
  Parsed P = parseCXX("void v(int, ...); void f(int *p, int *q) { v(1, p, q); }");
  auto CS = resolveCallSite(findFirst<CallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  ASSERT_EQ(CS->Arguments.size(), 1u);
  EXPECT_EQ(CS->UnmatchedArgs.size(), 2u);
}

TEST(CallSiteResolver, IndirectCallHasNoCallee) {
  Parsed P = parseCXX("void f(void (*fp)(int *), int *p) { fp(p); }");
  auto CS = resolveCallSite(findFirst<CallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  EXPECT_EQ(CS->Callee, nullptr);
  EXPECT_TRUE(CS->Arguments.empty());
  EXPECT_EQ(CS->UnmatchedArgs.size(), 1u);
}

TEST(CallSiteResolver, UnprototypedCCallHasOnlyUnmatchedArgs) {
  Parsed P = parseC("void g(); void f(int *p) { g(p, 2); }");
  auto CS = resolveCallSite(findFirst<CallExpr>(P.ctx()));
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  EXPECT_TRUE(CS->Arguments.empty());
  EXPECT_EQ(CS->UnmatchedArgs.size(), 2u);
}

TEST(CallSiteResolver, NonCallStatementIsNullopt) {
  Parsed P = parseCXX("void f(int *p) { *p = 1; }");
  EXPECT_FALSE(resolveCallSite(findFirst<BinaryOperator>(P.ctx())).has_value());
}

} // namespace
```

Add `Analyses/CallSiteResolverTest.cpp` to the source list in `clang/unittests/ScalableStaticAnalysis/CMakeLists.txt` (after `Analyses/CallGraph/CallGraphExtractorTest.cpp`).

- [ ] **Step 2: Run the tests to verify they fail to build**

Run: `ninja -C build ClangScalableAnalysisTests 2>&1 | tail -5`
Expected: compile error, `CallSiteResolution.h` not found.

- [ ] **Step 3: Implement the resolver**

`clang/include/clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h`:

```c++
//===- CallSiteResolution.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Resolves a call-like statement to its callee and pairs arguments with
// parameter indices. Shared by the PointerFlow and ParameterEscape extractors.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_CALLSITERESOLUTION_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_CALLSITERESOLUTION_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>
#include <utility>

namespace clang::ssaf {

struct CallSite {
  /// The statically known callee, or null when the call is indirect.
  const FunctionDecl *Callee = nullptr;
  /// The implicit object argument of a member call or a member operator call.
  const Expr *ImplicitObjectArg = nullptr;
  /// Arguments paired with the callee parameter index they initialize.
  llvm::SmallVector<std::pair<const Expr *, unsigned>, 8> Arguments;
  /// Arguments with no corresponding parameter: variadic tails, unprototyped
  /// callees, arity mismatches, and every argument of an indirect call.
  llvm::SmallVector<const Expr *, 4> UnmatchedArgs;
};

/// Returns the resolved call site for a CallExpr (including member and
/// operator calls) or CXXConstructExpr, and std::nullopt for any other
/// statement.
std::optional<CallSite> resolveCallSite(const Stmt *S);

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_CALLSITERESOLUTION_H
```

`clang/lib/ScalableStaticAnalysis/Analyses/CallSiteResolution.cpp`:

```c++
//===- CallSiteResolution.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "llvm/ADT/ArrayRef.h"

using namespace clang;

static void pairArguments(ssaf::CallSite &CS, llvm::ArrayRef<const Expr *> Args) {
  unsigned NumParams = CS.Callee->getNumParams();
  for (unsigned I = 0; I < Args.size(); ++I) {
    if (I < NumParams)
      CS.Arguments.push_back({Args[I], I});
    else
      CS.UnmatchedArgs.push_back(Args[I]);
  }
}

std::optional<ssaf::CallSite> ssaf::resolveCallSite(const Stmt *S) {
  CallSite CS;
  if (const auto *CCE = dyn_cast<CXXConstructExpr>(S)) {
    CS.Callee = CCE->getConstructor();
    pairArguments(CS, llvm::ArrayRef(CCE->getArgs(), CCE->getNumArgs()));
    return CS;
  }
  const auto *CE = dyn_cast<CallExpr>(S);
  if (!CE)
    return std::nullopt;

  llvm::ArrayRef<const Expr *> Args(CE->getArgs(), CE->getNumArgs());
  CS.Callee = CE->getDirectCallee();
  if (!CS.Callee) {
    CS.UnmatchedArgs.append(Args.begin(), Args.end());
    return CS;
  }
  if (const auto *MCE = dyn_cast<CXXMemberCallExpr>(CE)) {
    CS.ImplicitObjectArg = MCE->getImplicitObjectArgument();
  } else if (isa<CXXOperatorCallExpr>(CE)) {
    // A member operator receives its object as the first argument, unless it
    // is an explicit-object member function, whose object is parameter 0.
    // This intentionally mirrors PointerFlowExtractor's historical pairing.
    if (const auto *MD = dyn_cast<CXXMethodDecl>(CS.Callee);
        MD && !MD->isExplicitObjectMemberFunction() && !Args.empty()) {
      CS.ImplicitObjectArg = Args.front();
      Args = Args.drop_front();
    }
  }
  pairArguments(CS, Args);
  return CS;
}
```

Add `CallSiteResolution.cpp` to `add_clang_library(clangScalableStaticAnalysisAnalyses ...)` in `clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt` (alphabetically, before `CallGraph/CallGraphExtractor.cpp`).

- [ ] **Step 4: Run the tests**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='CallSiteResolver*'`
Expected: 10 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add clang/include/clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h clang/lib/ScalableStaticAnalysis/Analyses/CallSiteResolution.cpp clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt clang/unittests/ScalableStaticAnalysis/Analyses/CallSiteResolverTest.cpp clang/unittests/ScalableStaticAnalysis/CMakeLists.txt
git commit -m "[SSAF] Add shared call-site resolution helper

Pairs call arguments with callee parameter indices and exposes the
implicit object argument and unmatched arguments explicitly, so the
PointerFlow and upcoming ParameterEscape extractors share one
resolution routine.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Migrate `PointerFlow` to `resolveCallSite` (behavior-preserving)

**Files:**
- Modify: `clang/lib/ScalableStaticAnalysis/Analyses/PointerFlow/PointerFlowExtractor.cpp` (the `CallExpr`/`CXXConstructExpr` branches of `PointerFlowMatcher::matchesStmt`, and the `matchesArgsWithParams` template)
- Test: existing `clang/unittests/ScalableStaticAnalysis/Analyses/PointerFlow/PointerFlowTest.cpp`, `clang/test/Analysis/Scalable/PointerFlow/`

**Interfaces:**
- Consumes: `resolveCallSite` (Task 1).

- [ ] **Step 1: Record the baseline**

Run: `ninja -C build ClangScalableAnalysisTests clang clang-ssaf-linker clang-ssaf-analyzer FileCheck split-file not count && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='PointerFlow*:UnsafeBufferReachable*' && build/bin/llvm-lit -sv clang/test/Analysis/Scalable/PointerFlow clang/test/Analysis/Scalable/UnsafeBufferUsage`
Expected: all PASS. Note the counts.

- [ ] **Step 2: Replace the hand-written pairing**

In `PointerFlowExtractor.cpp` add `#include "clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h"`, delete the `matchesArgsWithParams` member template, and replace the two branches in `matchesStmt` (the `if (const auto *CE = dyn_cast<CallExpr>(S))` block and the `if (const auto *CCE = dyn_cast<CXXConstructExpr>(S))` block) with:

```c++
  // Match arg-to-param passing for calls and constructions of any pointer
  // type argument. Indirect calls contribute nothing; the implicit object
  // argument and unmatched arguments are intentionally ignored here.
  if (isa<CallExpr, CXXConstructExpr>(S)) {
    std::optional<CallSite> CS = resolveCallSite(S);
    if (!CS || !CS->Callee)
      return llvm::Error::success();
    for (auto [Arg, ParamIdx] : CS->Arguments) {
      const ParmVarDecl *PD = CS->Callee->getParamDecl(ParamIdx);
      if (PD && hasPtrOrArrType(PD))
        if (auto Err = addEdges(DeclPointerLevelVec{toDPL(PD)}, toDPL(Arg)))
          return Err;
    }
    return llvm::Error::success();
  }
```

- [ ] **Step 3: Re-run the baseline**

Run the Step 1 command again.
Expected: identical PASS counts; no new failures.

- [ ] **Step 4: Commit**

```bash
git add clang/lib/ScalableStaticAnalysis/Analyses/PointerFlow/PointerFlowExtractor.cpp
git commit -m "[SSAF][PointerFlow] Use the shared call-site resolver

No behavior change: edges are produced for the same argument/parameter
pairs as before; the implicit object and unmatched arguments the
resolver now exposes are ignored by PointerFlow.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: `ParameterEscape` summary types and JSON format

**Files:**
- Create: `clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h`
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.cpp`
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeJSON.h`
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeFormat.cpp`
- Modify: `clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt`, `clang/include/clang/ScalableStaticAnalysis/BuiltinAnchorSources.def`
- Test: `clang/unittests/ScalableStaticAnalysis/Serialization/JSONFormatTest/ParameterEscapeFormatTest.cpp`
- Modify: `clang/unittests/ScalableStaticAnalysis/CMakeLists.txt`

**Interfaces:**
- Produces (all in `namespace clang::ssaf`):
  ```c++
  constexpr int ThisParamIndex = -1;
  struct FlowTarget { EntityId Callee; int ParamIndex; };                 // operator<, ==
  enum class EscapeReason : uint8_t { Return, StoreToField, StoreToGlobal, StoreThroughPointer,
    AddressTaken, IndirectCall, UnmatchedArgument, CastToNonPointer, Throw, Coroutine,
    Deallocation, HeapAllocation, NonTrivialView, Asm, VarArgs, Capture, VirtualCall,
    ObjCMessage, CallableUse, UnnamedCallee, UnanalyzedCallee, EscapesViaCallee, UnrecognizedUse };
  llvm::StringRef escapeReasonName(EscapeReason);
  std::optional<EscapeReason> parseEscapeReason(llvm::StringRef);
  struct Sink { EscapeReason Reason; SourceLocationRecord Location; std::string Detail; };
  struct EscapeFact { std::map<FlowTarget, SourceLocationRecord> FlowsTo;   // target → first call site
                      std::optional<SourceLocationRecord> ReturnsSelfAt;
                      std::optional<Sink> OtherSink; bool returnsSelf() const; };
  class ParameterEscapeSummary final : public EntitySummary {
    static constexpr llvm::StringLiteral Name = "ParameterEscape";
    static SummaryName summaryName(); SummaryName getSummaryName() const override;
    std::map<unsigned, EscapeFact> Params; std::optional<EscapeFact> This;
    bool IsCandidate = false; std::set<unsigned> CandidateParams;
    bool empty() const; bool operator==(const ParameterEscapeSummary &) const; };
  ```
  `SourceLocationRecord` is reused from `clang/ScalableStaticAnalysis/Analyses/SharedLexicalRepresentation/SharedLexicalRepresentation.h`.
- JSON helpers in the private header `ParameterEscapeJSON.h`: `sourceLocationRecordToJSON`, `sourceLocationRecordFromJSON`, `escapeFactToJSON`, `escapeFactFromJSON` (used again in Task 9).

- [ ] **Step 1: Write the failing round-trip test**

`clang/unittests/ScalableStaticAnalysis/Serialization/JSONFormatTest/ParameterEscapeFormatTest.cpp`:

```c++
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "../../TestFixture.h"
#include "clang/Frontend/SSAFOptions.h"
#include "clang/ScalableStaticAnalysis/Core/Model/BuildNamespace.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummary.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ssaf;

namespace {

class ParameterEscapeFormatTest : public TestFixture {
protected:
  NestedBuildNamespace NS = NestedBuildNamespace::makeCompilationUnit("cu");
  EntityName nameOf(StringRef USR) { return EntityName(USR.str(), "", NS); }
};

TEST_F(ParameterEscapeFormatTest, RoundTripsAllFields) {
  SSAFOptions Opts;
  TUSummary TUSum(llvm::Triple("arm64-apple-macosx"),
                  BuildNamespace(BuildNamespaceKind::CompilationUnit, "cu"));
  TUSummaryBuilder Builder(TUSum, Opts);
  EntityId F = Builder.addEntity(nameOf("c:@F@f#*I#*I#"), EntityLinkageType::External);
  EntityId G = Builder.addEntity(nameOf("c:@F@g#*I#"), EntityLinkageType::External);

  ParameterEscapeSummary S;
  S.IsCandidate = true;
  S.CandidateParams = {0, 1};
  EscapeFact P0;
  P0.FlowsTo[FlowTarget{G, 0}] = SourceLocationRecord{"/src/a.cpp", 2, 3};
  P0.FlowsTo[FlowTarget{G, ThisParamIndex}] = SourceLocationRecord{"/src/a.cpp", 2, 9};
  S.Params[0] = P0;
  EscapeFact P1;
  P1.ReturnsSelfAt = SourceLocationRecord{"/src/a.cpp", 3, 10};
  P1.OtherSink = Sink{EscapeReason::StoreToGlobal, SourceLocationRecord{"/src/a.cpp", 4, 5}, ""};
  S.Params[1] = P1;
  EscapeFact ThisFact;
  ThisFact.OtherSink = Sink{EscapeReason::UnrecognizedUse, SourceLocationRecord{"/src/a.cpp", 9, 1}, "AtomicExpr"};
  S.This = ThisFact;
  ASSERT_TRUE(Builder.addSummary(F, std::make_unique<ParameterEscapeSummary>(S)).second);

  llvm::SmallString<128> Path;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("pe", "json", Path));
  llvm::FileRemover Remover(Path);
  JSONFormat Format;
  ASSERT_THAT_ERROR(Format.writeTUSummary(TUSum, Path), llvm::Succeeded());
  llvm::Expected<TUSummary> Read = Format.readTUSummary(Path);
  ASSERT_THAT_EXPECTED(Read, llvm::Succeeded());

  EntityId ReadF = getIdTable(*Read).getId(nameOf("c:@F@f#*I#*I#"));
  EntityId ReadG = getIdTable(*Read).getId(nameOf("c:@F@g#*I#"));
  auto &Data = getData(*Read)[ParameterEscapeSummary::summaryName()];
  ASSERT_EQ(Data.count(ReadF), 1u);
  const auto &RS = static_cast<const ParameterEscapeSummary &>(*Data[ReadF]);
  ParameterEscapeSummary Expected = S;
  Expected.Params[0].FlowsTo.clear();
  Expected.Params[0].FlowsTo[FlowTarget{ReadG, 0}] = SourceLocationRecord{"/src/a.cpp", 2, 3};
  Expected.Params[0].FlowsTo[FlowTarget{ReadG, ThisParamIndex}] = SourceLocationRecord{"/src/a.cpp", 2, 9};
  EXPECT_TRUE(RS == Expected);
}

TEST_F(ParameterEscapeFormatTest, ReasonNamesRoundTrip) {
  for (unsigned I = 0; I <= static_cast<unsigned>(EscapeReason::UnrecognizedUse); ++I) {
    auto R = static_cast<EscapeReason>(I);
    EXPECT_EQ(parseEscapeReason(escapeReasonName(R)), std::optional(R));
  }
  EXPECT_FALSE(parseEscapeReason("Bogus").has_value());
}

} // namespace
```

Add `Serialization/JSONFormatTest/ParameterEscapeFormatTest.cpp` to the unittest CMake source list.

- [ ] **Step 2: Run to verify it fails to build**

Run: `ninja -C build ClangScalableAnalysisTests 2>&1 | tail -3`
Expected: `ParameterEscape.h` not found.

- [ ] **Step 3: Implement the types**

`clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h`:

```c++
//===- ParameterEscape.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-translation-unit summary of how each parameter of a function definition
// may escape: the callee parameters it flows into, whether it is returned, and
// the first non-return sink. See
// devincoughlin/features/noescape-inference/specs/2026-09-19-cross-tu-noescape-inference-design.md.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPE_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPE_H

#include "clang/ScalableStaticAnalysis/Analyses/SharedLexicalRepresentation/SharedLexicalRepresentation.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Model/SummaryName.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/EntitySummary.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace clang::ssaf {

/// Parameter index of the implicit object parameter in FlowTarget and Node.
constexpr int ThisParamIndex = -1;

struct FlowTarget {
  EntityId Callee;
  int ParamIndex;

  bool operator<(const FlowTarget &O) const {
    return std::tie(Callee, ParamIndex) < std::tie(O.Callee, O.ParamIndex);
  }
  bool operator==(const FlowTarget &O) const {
    return std::tie(Callee, ParamIndex) == std::tie(O.Callee, O.ParamIndex);
  }
};

enum class EscapeReason : uint8_t {
  Return,
  StoreToField,
  StoreToGlobal,
  StoreThroughPointer,
  AddressTaken,
  IndirectCall,
  UnmatchedArgument,
  CastToNonPointer,
  Throw,
  Coroutine,
  Deallocation,
  HeapAllocation,
  NonTrivialView,
  Asm,
  VarArgs,
  Capture,
  VirtualCall,
  ObjCMessage,
  CallableUse,
  UnnamedCallee,
  UnanalyzedCallee,
  EscapesViaCallee,
  UnrecognizedUse, // must stay last
};

llvm::StringRef escapeReasonName(EscapeReason R);
std::optional<EscapeReason> parseEscapeReason(llvm::StringRef Name);

struct Sink {
  EscapeReason Reason;
  SourceLocationRecord Location;
  /// Free-form detail, e.g. the statement class for UnrecognizedUse.
  std::string Detail;

  bool operator==(const Sink &O) const {
    return std::tie(Reason, Location, Detail) ==
           std::tie(O.Reason, O.Location, O.Detail);
  }
};

struct EscapeFact {
  /// Callee parameters this parameter flows into, with the first call site.
  std::map<FlowTarget, SourceLocationRecord> FlowsTo;
  std::optional<SourceLocationRecord> ReturnsSelfAt;
  std::optional<Sink> OtherSink;

  bool returnsSelf() const { return ReturnsSelfAt.has_value(); }
  bool operator==(const EscapeFact &O) const {
    return std::tie(FlowsTo, ReturnsSelfAt, OtherSink) ==
           std::tie(O.FlowsTo, O.ReturnsSelfAt, O.OtherSink);
  }
};

class ParameterEscapeSummary final : public EntitySummary {
public:
  static constexpr llvm::StringLiteral Name = "ParameterEscape";
  static SummaryName summaryName() { return SummaryName(Name.str()); }
  SummaryName getSummaryName() const override { return summaryName(); }

  /// Facts for pointer-carrying parameters, keyed by parameter index.
  std::map<unsigned, EscapeFact> Params;
  /// Fact for the implicit object parameter of an instance method.
  std::optional<EscapeFact> This;
  /// Whether the definition is eligible for annotation (spec §5.2).
  bool IsCandidate = false;
  /// Parameter indices of candidate type.
  std::set<unsigned> CandidateParams;

  bool empty() const { return Params.empty() && !This; }
  bool operator==(const ParameterEscapeSummary &O) const {
    return std::tie(Params, This, IsCandidate, CandidateParams) ==
           std::tie(O.Params, O.This, O.IsCandidate, O.CandidateParams);
  }
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPE_H
```

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.cpp`:

```c++
//===- ParameterEscape.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include <iterator>

using namespace clang::ssaf;

static constexpr llvm::StringLiteral ReasonNames[] = {
    "Return",        "StoreToField",   "StoreToGlobal",   "StoreThroughPointer",
    "AddressTaken",  "IndirectCall",   "UnmatchedArgument", "CastToNonPointer",
    "Throw",         "Coroutine",      "Deallocation",    "HeapAllocation",
    "NonTrivialView", "Asm",           "VarArgs",         "Capture",
    "VirtualCall",   "ObjCMessage",    "CallableUse",     "UnnamedCallee",
    "UnanalyzedCallee", "EscapesViaCallee", "UnrecognizedUse",
};

static_assert(std::size(ReasonNames) ==
                  static_cast<size_t>(EscapeReason::UnrecognizedUse) + 1,
              "ReasonNames must cover every EscapeReason");

llvm::StringRef clang::ssaf::escapeReasonName(EscapeReason R) {
  return ReasonNames[static_cast<size_t>(R)];
}

std::optional<EscapeReason> clang::ssaf::parseEscapeReason(llvm::StringRef Name) {
  for (size_t I = 0; I < std::size(ReasonNames); ++I)
    if (ReasonNames[I] == Name)
      return static_cast<EscapeReason>(I);
  return std::nullopt;
}
```

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeJSON.h`:

```c++
//===- ParameterEscapeJSON.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEJSON_H
#define LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEJSON_H

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace clang::ssaf {
llvm::json::Object sourceLocationRecordToJSON(const SourceLocationRecord &R);
llvm::Expected<SourceLocationRecord>
sourceLocationRecordFromJSON(const llvm::json::Object &O);
llvm::json::Object escapeFactToJSON(const EscapeFact &F,
                                    JSONFormat::EntityIdToJSONFn IdToJSON);
llvm::Expected<EscapeFact>
escapeFactFromJSON(const llvm::json::Object &O,
                   JSONFormat::EntityIdFromJSONFn IdFromJSON);
} // namespace clang::ssaf

#endif
```

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeFormat.cpp`:

```c++
//===- ParameterEscapeFormat.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ParameterEscapeJSON.h"
#include "SSAFAnalysesCommon.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

using namespace clang;
using namespace ssaf;
using Array = llvm::json::Array;
using Object = llvm::json::Object;

namespace {
constexpr llvm::StringLiteral ParamsKey = "params";
constexpr llvm::StringLiteral IndexKey = "index";
constexpr llvm::StringLiteral ThisKey = "this";
constexpr llvm::StringLiteral IsCandidateKey = "is_candidate";
constexpr llvm::StringLiteral CandidateParamsKey = "candidate_params";
constexpr llvm::StringLiteral FlowsToKey = "flows_to";
constexpr llvm::StringLiteral CalleeKey = "callee";
constexpr llvm::StringLiteral ParamKey = "param";
constexpr llvm::StringLiteral LocationKey = "location";
constexpr llvm::StringLiteral ReturnsSelfAtKey = "returns_self_at";
constexpr llvm::StringLiteral SinkKey = "sink";
constexpr llvm::StringLiteral ReasonKey = "reason";
constexpr llvm::StringLiteral DetailKey = "detail";
constexpr llvm::StringLiteral FileKey = "file";
constexpr llvm::StringLiteral LineKey = "line";
constexpr llvm::StringLiteral ColumnKey = "column";
} // namespace

Object clang::ssaf::sourceLocationRecordToJSON(const SourceLocationRecord &R) {
  return Object{{FileKey.data(), R.FilePath},
                {LineKey.data(), static_cast<int64_t>(R.Line)},
                {ColumnKey.data(), static_cast<int64_t>(R.Column)}};
}

llvm::Expected<SourceLocationRecord>
clang::ssaf::sourceLocationRecordFromJSON(const Object &O) {
  auto File = O.getString(FileKey.data());
  auto Line = O.getInteger(LineKey.data());
  auto Column = O.getInteger(ColumnKey.data());
  if (!File || !Line || !Column)
    return makeSawButExpectedError(O, "a location object with file/line/column");
  return SourceLocationRecord{File->str(), static_cast<unsigned>(*Line),
                              static_cast<unsigned>(*Column)};
}

Object clang::ssaf::escapeFactToJSON(const EscapeFact &F,
                                     JSONFormat::EntityIdToJSONFn IdToJSON) {
  Array Flows;
  for (const auto &[T, Site] : F.FlowsTo)
    Flows.push_back(Object{{CalleeKey.data(), IdToJSON(T.Callee)},
                           {ParamKey.data(), static_cast<int64_t>(T.ParamIndex)},
                           {LocationKey.data(), sourceLocationRecordToJSON(Site)}});
  Object O{{FlowsToKey.data(), std::move(Flows)}};
  if (F.ReturnsSelfAt)
    O[ReturnsSelfAtKey.data()] = sourceLocationRecordToJSON(*F.ReturnsSelfAt);
  if (F.OtherSink)
    O[SinkKey.data()] = Object{
        {ReasonKey.data(), escapeReasonName(F.OtherSink->Reason).str()},
        {DetailKey.data(), F.OtherSink->Detail},
        {LocationKey.data(), sourceLocationRecordToJSON(F.OtherSink->Location)}};
  return O;
}

llvm::Expected<EscapeFact>
clang::ssaf::escapeFactFromJSON(const Object &O,
                                JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  EscapeFact F;
  const Array *Flows = O.getArray(FlowsToKey.data());
  if (!Flows)
    return makeSawButExpectedError(O, "an object with a key %s", FlowsToKey.data());
  for (const auto &V : *Flows) {
    const Object *T = V.getAsObject();
    const Object *Callee = T ? T->getObject(CalleeKey.data()) : nullptr;
    auto Param = T ? T->getInteger(ParamKey.data()) : std::nullopt;
    const Object *LocObj = T ? T->getObject(LocationKey.data()) : nullptr;
    if (!Callee || !Param || !LocObj)
      return makeSawButExpectedError(V, "a flow target with callee, param, location");
    llvm::Expected<EntityId> Id = IdFromJSON(*Callee);
    if (!Id)
      return Id.takeError();
    auto Loc = sourceLocationRecordFromJSON(*LocObj);
    if (!Loc)
      return Loc.takeError();
    F.FlowsTo[FlowTarget{*Id, static_cast<int>(*Param)}] = *Loc;
  }
  if (const Object *Ret = O.getObject(ReturnsSelfAtKey.data())) {
    auto Loc = sourceLocationRecordFromJSON(*Ret);
    if (!Loc)
      return Loc.takeError();
    F.ReturnsSelfAt = *Loc;
  }
  if (const Object *S = O.getObject(SinkKey.data())) {
    auto Reason = S->getString(ReasonKey.data());
    const Object *LocObj = S->getObject(LocationKey.data());
    if (!Reason || !LocObj)
      return makeSawButExpectedError(*S, "a sink with reason and location");
    auto R = parseEscapeReason(*Reason);
    if (!R)
      return makeSawButExpectedError(*S, "a known escape reason");
    auto Loc = sourceLocationRecordFromJSON(*LocObj);
    if (!Loc)
      return Loc.takeError();
    F.OtherSink = Sink{*R, *Loc, S->getString(DetailKey.data()).value_or("").str()};
  }
  return F;
}

static Object serialize(const EntitySummary &ES, JSONFormat::EntityIdToJSONFn IdToJSON) {
  const auto &S = static_cast<const ParameterEscapeSummary &>(ES);
  Array Params;
  for (const auto &[Index, Fact] : S.Params) {
    Object P = escapeFactToJSON(Fact, IdToJSON);
    P[IndexKey.data()] = static_cast<int64_t>(Index);
    Params.push_back(std::move(P));
  }
  Array Candidates;
  for (unsigned I : S.CandidateParams)
    Candidates.push_back(static_cast<int64_t>(I));
  Object O{{ParamsKey.data(), std::move(Params)},
           {IsCandidateKey.data(), S.IsCandidate},
           {CandidateParamsKey.data(), std::move(Candidates)}};
  if (S.This)
    O[ThisKey.data()] = escapeFactToJSON(*S.This, IdToJSON);
  return O;
}

static llvm::Expected<std::unique_ptr<EntitySummary>>
deserialize(const Object &Data, EntityIdTable &, JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  auto S = std::make_unique<ParameterEscapeSummary>();
  const Array *Params = Data.getArray(ParamsKey.data());
  auto IsCandidate = Data.getBoolean(IsCandidateKey.data());
  const Array *Candidates = Data.getArray(CandidateParamsKey.data());
  if (!Params || !IsCandidate || !Candidates)
    return makeSawButExpectedError(Object(Data), "a ParameterEscape summary object");
  S->IsCandidate = *IsCandidate;
  for (const auto &V : *Candidates) {
    auto I = V.getAsInteger();
    if (!I)
      return makeSawButExpectedError(V, "a parameter index");
    S->CandidateParams.insert(static_cast<unsigned>(*I));
  }
  for (const auto &V : *Params) {
    const Object *P = V.getAsObject();
    auto Index = P ? P->getInteger(IndexKey.data()) : std::nullopt;
    if (!P || !Index)
      return makeSawButExpectedError(V, "a parameter fact with an index");
    auto Fact = escapeFactFromJSON(*P, IdFromJSON);
    if (!Fact)
      return Fact.takeError();
    S->Params[static_cast<unsigned>(*Index)] = std::move(*Fact);
  }
  if (const Object *T = Data.getObject(ThisKey.data())) {
    auto Fact = escapeFactFromJSON(*T, IdFromJSON);
    if (!Fact)
      return Fact.takeError();
    S->This = std::move(*Fact);
  }
  return std::move(S);
}

namespace {
struct ParameterEscapeJSONFormatInfo final : JSONFormat::FormatInfo {
  ParameterEscapeJSONFormatInfo()
      : JSONFormat::FormatInfo(ParameterEscapeSummary::summaryName(), serialize, deserialize) {}
};
} // namespace

static llvm::Registry<JSONFormat::FormatInfo>::Add<ParameterEscapeJSONFormatInfo>
    RegisterParameterEscapeJSONFormatInfo(ParameterEscapeSummary::Name,
                                          "JSON Format info for ParameterEscapeSummary");

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int ParameterEscapeJSONFormatAnchorSource = 0;
} // namespace clang::ssaf
```

Wiring:
- `clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt`: add `ParameterEscape/ParameterEscape.cpp` and `ParameterEscape/ParameterEscapeFormat.cpp` to the source list.
- `clang/include/clang/ScalableStaticAnalysis/BuiltinAnchorSources.def`: add `ANCHOR(ParameterEscapeJSONFormatAnchorSource)` in alphabetical order (after `JSONFormatAnchorSource`).

- [ ] **Step 4: Run the tests**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='ParameterEscapeFormat*'`
Expected: 2 PASS.

- [ ] **Step 5: Commit**

```bash
git add clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt clang/include/clang/ScalableStaticAnalysis/BuiltinAnchorSources.def clang/unittests/ScalableStaticAnalysis/Serialization/JSONFormatTest/ParameterEscapeFormatTest.cpp clang/unittests/ScalableStaticAnalysis/CMakeLists.txt
git commit -m "[SSAF][ParameterEscape] Add summary data types and JSON format

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Library-function capture knowledge (static table, LLVM-verified by test)

**Files:**
- Create: `clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h`
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.cpp`
- Modify: `clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt` (add the `.cpp`; **no** new LLVM components)
- Modify: `clang/unittests/ScalableStaticAnalysis/CMakeLists.txt` (`LLVM_COMPONENTS` += `Analysis Core TransformUtils` — test only)
- Test: `clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledgeTest.cpp`

**Interfaces:**
- Produces (`namespace clang::ssaf`):
  ```c++
  struct LibraryFunctionFact { llvm::StringLiteral Name; unsigned Arity; unsigned NonCapturingParams /*bitmask*/; bool IsDeallocator; };
  llvm::ArrayRef<LibraryFunctionFact> libraryFunctionFacts();   // the table, exposed for the cross-check test
  class LibraryFunctionKnowledge {
  public:
    /// True iff FD is a recognized bodiless C library function (or the matching
    /// builtin), builtins are not disabled, FD is not a deallocator, and the
    /// table marks ParamIndex non-capturing.
    static bool parameterDoesNotEscape(const FunctionDecl *FD, unsigned ParamIndex, ASTContext &Ctx);
    static bool isDeallocationFunction(const FunctionDecl *FD, ASTContext &Ctx);
  };
  ```

Why a table and not a runtime LLVM query: `clangScalableStaticAnalysisAnalyses` is linked into `clangDriver`, `clangFrontendTool`, `clang-ssaf-linker`, `clang-ssaf-analyzer`, `clang-ssaf-format` and the unit tests; pulling `LLVMCore`/`LLVMAnalysis`/`LLVMTransformUtils` into all of them for ~25 facts is unjustified layering. The unit test links those components and proves every table row against `inferNonMandatoryLibFuncAttrs`, so the table cannot drift from LLVM unnoticed.

- [ ] **Step 1: Write the failing tests**

`clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledgeTest.cpp`:

```c++
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h"
#include "../../FindDecl.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ssaf;

namespace {

constexpr const char *Decls = R"c(
typedef __SIZE_TYPE__ size_t;
typedef struct __sFILE FILE;
size_t strlen(const char *s);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
int puts(const char *s);
int fputs(const char *s, FILE *f);
int printf(const char *fmt, ...);
long strtol(const char *s, char **end, int base);
void free(void *p);
void *realloc(void *p, size_t n);
int mine(char *p);
size_t use(const char *s) { return strlen(s) + __builtin_strlen(s); }
size_t defined_here(const char *s);
size_t defined_here(const char *s) { return 0; }
)c";

class LibraryFunctionKnowledgeTest : public ::testing::Test {
protected:
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Decls, {"-x", "c", "-std=c17"});
  ASTContext &Ctx = AST->getASTContext();

  const FunctionDecl *fn(StringRef Name) { return findFnByName(Name, Ctx); }
  bool noEscape(StringRef Name, unsigned I) {
    return LibraryFunctionKnowledge::parameterDoesNotEscape(fn(Name), I, Ctx);
  }
};

TEST_F(LibraryFunctionKnowledgeTest, KnownNonCapturingParameters) {
  EXPECT_TRUE(noEscape("strlen", 0));
  EXPECT_TRUE(noEscape("memcpy", 1));
  EXPECT_TRUE(noEscape("memmove", 1));
  EXPECT_TRUE(noEscape("memcmp", 0));
  EXPECT_TRUE(noEscape("memcmp", 1));
  EXPECT_TRUE(noEscape("strcmp", 0));
  EXPECT_TRUE(noEscape("strcpy", 1));
  EXPECT_TRUE(noEscape("puts", 0));
  EXPECT_TRUE(noEscape("fputs", 0));
  EXPECT_TRUE(noEscape("printf", 0));
  EXPECT_TRUE(noEscape("strtol", 1));
}

TEST_F(LibraryFunctionKnowledgeTest, ParametersLLVMDoesNotMarkAreNotTrusted) {
  EXPECT_FALSE(noEscape("memcpy", 0)); // only 'returned' in LLVM's model
  EXPECT_FALSE(noEscape("memset", 0));
  EXPECT_FALSE(noEscape("strchr", 0)); // result aliases the argument
  EXPECT_FALSE(noEscape("strcpy", 0));
  EXPECT_FALSE(noEscape("strtol", 0)); // stored through *end
  EXPECT_FALSE(noEscape("strlen", 1)); // out of range
}

TEST_F(LibraryFunctionKnowledgeTest, DeallocatorsAreExcluded) {
  EXPECT_TRUE(LibraryFunctionKnowledge::isDeallocationFunction(fn("free"), Ctx));
  EXPECT_FALSE(noEscape("free", 0));
  EXPECT_TRUE(LibraryFunctionKnowledge::isDeallocationFunction(fn("realloc"), Ctx));
  EXPECT_FALSE(noEscape("realloc", 0));
  EXPECT_FALSE(LibraryFunctionKnowledge::isDeallocationFunction(fn("strlen"), Ctx));
}

TEST_F(LibraryFunctionKnowledgeTest, UnknownAndDefinedFunctionsAreNotTrusted) {
  EXPECT_FALSE(noEscape("mine", 0));
  EXPECT_FALSE(LibraryFunctionKnowledge::isDeallocationFunction(fn("mine"), Ctx));
  // A function defined in this TU is analyzed, never trusted by name.
  const FunctionDecl *Def = nullptr;
  for (const FunctionDecl *R : fn("defined_here")->redecls())
    if (R->doesThisDeclarationHaveABody())
      Def = R;
  ASSERT_NE(Def, nullptr);
  EXPECT_FALSE(LibraryFunctionKnowledge::parameterDoesNotEscape(Def, 0, Ctx));
}

TEST_F(LibraryFunctionKnowledgeTest, BuiltinAliasIsRecognized) {
  const FunctionDecl *Builtin = nullptr;
  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls())
    if (const auto *FD = dyn_cast<FunctionDecl>(D);
        FD && FD->getBuiltinID() &&
        Ctx.BuiltinInfo.getName(FD->getBuiltinID()) == "__builtin_strlen")
      Builtin = FD;
  ASSERT_NE(Builtin, nullptr);
  EXPECT_TRUE(LibraryFunctionKnowledge::parameterDoesNotEscape(Builtin, 0, Ctx));
}

TEST_F(LibraryFunctionKnowledgeTest, NoBuiltinDisablesTrust) {
  std::unique_ptr<ASTUnit> NB =
      tooling::buildASTFromCodeWithArgs(Decls, {"-x", "c", "-std=c17", "-fno-builtin"});
  ASTContext &C2 = NB->getASTContext();
  EXPECT_FALSE(LibraryFunctionKnowledge::parameterDoesNotEscape(findFnByName("strlen", C2), 0, C2));
}

// Every table row must agree with LLVM's own libcall inference. The table is
// the production artifact; LLVM is the oracle.
TEST_F(LibraryFunctionKnowledgeTest, TableMatchesLLVMInference) {
  llvm::LLVMContext LLVMCtx;
  llvm::Module M("oracle", LLVMCtx);
  llvm::TargetLibraryInfoImpl TLIImpl(Ctx.getTargetInfo().getTriple());
  llvm::TargetLibraryInfo TLI(TLIImpl);
  auto *Ptr = llvm::PointerType::get(LLVMCtx, 0);
  auto *I32 = llvm::Type::getInt32Ty(LLVMCtx);
  auto *I64 = llvm::Type::getInt64Ty(LLVMCtx);
  auto *Void = llvm::Type::getVoidTy(LLVMCtx);
  // LLVM prototypes for each table entry, in table order.
  std::map<std::string, llvm::FunctionType *> Protos = {
      {"strlen", llvm::FunctionType::get(I64, {Ptr}, false)},
      {"strnlen", llvm::FunctionType::get(I64, {Ptr, I64}, false)},
      {"wcslen", llvm::FunctionType::get(I64, {Ptr}, false)},
      {"memcpy", llvm::FunctionType::get(Ptr, {Ptr, Ptr, I64}, false)},
      {"memmove", llvm::FunctionType::get(Ptr, {Ptr, Ptr, I64}, false)},
      {"memcmp", llvm::FunctionType::get(I32, {Ptr, Ptr, I64}, false)},
      {"bcmp", llvm::FunctionType::get(I32, {Ptr, Ptr, I64}, false)},
      {"strcmp", llvm::FunctionType::get(I32, {Ptr, Ptr}, false)},
      {"strncmp", llvm::FunctionType::get(I32, {Ptr, Ptr, I64}, false)},
      {"strcpy", llvm::FunctionType::get(Ptr, {Ptr, Ptr}, false)},
      {"strncpy", llvm::FunctionType::get(Ptr, {Ptr, Ptr, I64}, false)},
      {"stpcpy", llvm::FunctionType::get(Ptr, {Ptr, Ptr}, false)},
      {"strcat", llvm::FunctionType::get(Ptr, {Ptr, Ptr}, false)},
      {"strncat", llvm::FunctionType::get(Ptr, {Ptr, Ptr, I64}, false)},
      {"puts", llvm::FunctionType::get(I32, {Ptr}, false)},
      {"fputs", llvm::FunctionType::get(I32, {Ptr, Ptr}, false)},
      {"fwrite", llvm::FunctionType::get(I64, {Ptr, I64, I64, Ptr}, false)},
      {"printf", llvm::FunctionType::get(I32, {Ptr}, true)},
      {"fprintf", llvm::FunctionType::get(I32, {Ptr, Ptr}, true)},
      {"atoi", llvm::FunctionType::get(I32, {Ptr}, false)},
      {"strtol", llvm::FunctionType::get(I64, {Ptr, Ptr, I32}, false)},
      {"free", llvm::FunctionType::get(Void, {Ptr}, false)},
      {"realloc", llvm::FunctionType::get(Ptr, {Ptr, I64}, false)},
      {"reallocf", llvm::FunctionType::get(Ptr, {Ptr, I64}, false)},
  };
  for (const LibraryFunctionFact &Fact : libraryFunctionFacts()) {
    auto It = Protos.find(Fact.Name.str());
    ASSERT_NE(It, Protos.end()) << "no oracle prototype for " << Fact.Name.str();
    llvm::Function *F = llvm::Function::Create(It->second, llvm::GlobalValue::ExternalLinkage,
                                               Fact.Name, M);
    llvm::LibFunc LF = TLI.getLibFunc(*F);
    ASSERT_NE(LF, llvm::NotLibFunc) << Fact.Name.str();
    llvm::inferNonMandatoryLibFuncAttrs(*F, TLI);
    bool IsDealloc = llvm::isLibFreeFunction(F, LF) || llvm::isReallocLikeFn(F);
    EXPECT_EQ(Fact.IsDeallocator, IsDealloc) << Fact.Name.str();
    EXPECT_EQ(Fact.Arity, F->arg_size()) << Fact.Name.str();
    for (unsigned I = 0; I < F->arg_size(); ++I) {
      bool LLVMSays = F->hasParamAttribute(I, llvm::Attribute::Captures) &&
                      llvm::capturesNothing(F->getParamAttribute(I, llvm::Attribute::Captures)
                                                .getCaptureInfo()
                                                .getOtherComponents());
      bool TableSays = (Fact.NonCapturingParams >> I) & 1u;
      EXPECT_EQ(TableSays, LLVMSays) << Fact.Name.str() << " param " << I;
    }
  }
}

} // namespace
```

Add `Analyses/ParameterEscape/LibraryFunctionKnowledgeTest.cpp` to the unittest CMake source list and `Analysis Core TransformUtils` to that target's `LLVM_COMPONENTS` (the production library gains nothing).

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build ClangScalableAnalysisTests 2>&1 | tail -3`
Expected: header not found.

- [ ] **Step 3: Implement**

`clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h`:

```c++
//===- LibraryFunctionKnowledge.h -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Which parameters of well-known C library functions never capture their
// argument. The table mirrors LLVM's libcall attribute inference
// (llvm/lib/Transforms/Utils/BuildLibCalls.cpp) and is verified against it by
// LibraryFunctionKnowledgeTest, so the analysis assumes exactly what the
// optimizer assumes — without linking LLVM IR into the frontend.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_LIBRARYFUNCTIONKNOWLEDGE_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_LIBRARYFUNCTIONKNOWLEDGE_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clang::ssaf {

struct LibraryFunctionFact {
  llvm::StringLiteral Name;
  unsigned Arity;               // fixed parameters (variadic tail excluded)
  unsigned NonCapturingParams;  // bit i set ⇒ parameter i is captures(none)
  bool IsDeallocator;
};

llvm::ArrayRef<LibraryFunctionFact> libraryFunctionFacts();

class LibraryFunctionKnowledge {
public:
  static bool parameterDoesNotEscape(const FunctionDecl *FD, unsigned ParamIndex,
                                     ASTContext &Ctx);
  static bool isDeallocationFunction(const FunctionDecl *FD, ASTContext &Ctx);
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_LIBRARYFUNCTIONKNOWLEDGE_H
```

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.cpp`:

```c++
//===- LibraryFunctionKnowledge.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h"
#include "clang/AST/Attr.h"
#include "clang/Basic/Builtins.h"

using namespace clang;
using namespace clang::ssaf;

// Bit i of NonCapturingParams ⇔ BuildLibCalls.cpp calls setDoesNotCapture(F, i)
// for that LibFunc. Deallocators are listed so callers can refuse them even
// though LLVM marks their pointer captures(none).
static constexpr LibraryFunctionFact Facts[] = {
    {"strlen", 1, 0b1, false},     {"strnlen", 2, 0b1, false},  {"wcslen", 1, 0b1, false},
    {"memcpy", 3, 0b10, false},    {"memmove", 3, 0b10, false}, {"memcmp", 3, 0b11, false},
    {"bcmp", 3, 0b11, false},      {"strcmp", 2, 0b11, false},  {"strncmp", 3, 0b11, false},
    {"strcpy", 2, 0b10, false},    {"strncpy", 3, 0b10, false}, {"stpcpy", 2, 0b10, false},
    {"strcat", 2, 0b10, false},    {"strncat", 3, 0b10, false}, {"puts", 1, 0b1, false},
    {"fputs", 2, 0b11, false},     {"fwrite", 4, 0b1001, false}, {"printf", 1, 0b1, false},
    {"fprintf", 2, 0b11, false},   {"atoi", 1, 0b1, false},     {"strtol", 3, 0b10, false},
    {"free", 1, 0b1, true},        {"realloc", 2, 0b1, true},   {"reallocf", 2, 0b1, true},
};

llvm::ArrayRef<LibraryFunctionFact> clang::ssaf::libraryFunctionFacts() { return Facts; }

static const LibraryFunctionFact *lookup(const FunctionDecl *FD, ASTContext &Ctx) {
  if (!FD || Ctx.getLangOpts().NoBuiltin || FD->hasAttr<NoBuiltinAttr>())
    return nullptr;
  if (FD->isDefined() || FD->isCXXClassMember())
    return nullptr; // definitions are analyzed, never trusted by name
  StringRef Name;
  if (unsigned ID = FD->getBuiltinID()) {
    Name = Ctx.BuiltinInfo.getName(ID);
    Name.consume_front("__builtin_");
  } else {
    if (Ctx.getLangOpts().CPlusPlus && !FD->isExternC())
      return nullptr;
    const IdentifierInfo *II = FD->getIdentifier();
    if (!II)
      return nullptr;
    Name = II->getName();
  }
  for (const LibraryFunctionFact &F : Facts)
    if (F.Name == Name && F.Arity == FD->getNumParams())
      return &F;
  return nullptr;
}

bool LibraryFunctionKnowledge::isDeallocationFunction(const FunctionDecl *FD, ASTContext &Ctx) {
  const LibraryFunctionFact *F = lookup(FD, Ctx);
  return F && F->IsDeallocator;
}

bool LibraryFunctionKnowledge::parameterDoesNotEscape(const FunctionDecl *FD, unsigned ParamIndex,
                                                      ASTContext &Ctx) {
  const LibraryFunctionFact *F = lookup(FD, Ctx);
  if (!F || F->IsDeallocator || ParamIndex >= F->Arity)
    return false;
  return (F->NonCapturingParams >> ParamIndex) & 1u;
}
```

Add `ParameterEscape/LibraryFunctionKnowledge.cpp` to the Analyses CMake list.

- [ ] **Step 4: Run the tests**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='LibraryFunctionKnowledge*'`
Expected: 7 PASS. If `TableMatchesLLVMInference` reports a mismatch, LLVM is right: fix the table row (or delete it) and the affected expectation.

- [ ] **Step 5: Commit**

```bash
git add clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.cpp clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt clang/unittests/ScalableStaticAnalysis/CMakeLists.txt clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledgeTest.cpp
git commit -m "[SSAF][ParameterEscape] Add LLVM-verified library function capture table

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Extractor skeleton — candidacy, candidate types, sound stub facts

**Files:**
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.h`
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.cpp` (stub in this task; full classifier in Task 6)
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractor.cpp`
- Modify: `clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt` (add both `.cpp`; `LINK_LIBS` += `clangAnalysisLifetimeSafety`), `BuiltinAnchorSources.def` (`ANCHOR(ParameterEscapeExtractorAnchorSource)`)
- Test: `clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractorTest.cpp` (harness + candidacy tests; later tasks append to it)
- Modify: `clang/unittests/ScalableStaticAnalysis/CMakeLists.txt`

**Interfaces:**
- Consumes: `ParameterEscapeSummary` (Task 3), `LibraryFunctionKnowledge` (Task 4), `extractAndAddSummaries`/`findContributors` from `SSAFAnalysesCommon.h`, `lifetimes::isGslPointerType` from `clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h` (library `clangAnalysisLifetimeSafety`).
- Produces (private header `EscapeClassifier.h`, `namespace clang::ssaf`):
  ```c++
  bool isViewLikeRecordType(QualType T);       // gsl::Pointer or swift_attr("~Escapable"), any triviality
  bool isTrackedViewType(QualType T);          // view-like and trivially copyable + destructible
  bool isPointerCarryingType(QualType T);      // reference, any non-function pointer, block pointer, tracked view
  bool isCandidateParameterType(QualType T);   // reference, object pointer, tracked view
  bool isCandidateDefinition(const FunctionDecl *Def, ASTContext &Ctx);
  struct FunctionEscapeFacts { std::map<unsigned, EscapeFact> Params; std::optional<EscapeFact> This; };
  FunctionEscapeFacts classifyFunctionEscapes(const FunctionDecl *Def, ASTContext &Ctx,
                                              TUSummaryExtractor &Extractor);
  ```
- Extractor registered under the name `"ParameterEscape"`.

- [ ] **Step 1: Write the failing tests (harness + candidacy)**

`clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractorTest.cpp`:

```c++
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "../../FindDecl.h"
#include "../../TestFixture.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/PCHContainerOperations.h"
#include "clang/Frontend/SSAFOptions.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/ExtractorRegistry.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummary.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryBuilder.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ssaf;

namespace {

// Minimal stand-ins so tests need no libc++: a tracked view and an owner.
constexpr const char *Prelude = R"cpp(
struct [[gsl::Pointer(int)]] View { int *d; unsigned n;
  View(int *p, unsigned n) : d(p), n(n) {}
  int *data() const { return d; }
  unsigned size() const { return n; }
  int &operator[](unsigned i) const { return d[i]; }
  void leak();
  View sub(unsigned i) const { return View(d + i, n - i); } };
struct [[gsl::Owner(int)]] Owner { int *buf; Owner(); ~Owner(); void push(int *p); };
struct Holder { int *p; Holder(int *q) : p(q) {} };
struct __attribute__((swift_attr("~Escapable"))) SwiftView { int *d; };
struct [[gsl::Pointer(int)]] NonTrivialView { int *d; NonTrivialView(int *p) : d(p) {} ~NonTrivialView(); };
int *g_ptr; static int *s_ptr;
void unknown(int *); void noesc(__attribute__((noescape)) int *);
typedef __SIZE_TYPE__ size_t; extern "C" size_t strlen(const char *);
extern "C" void free(void *); extern "C" void *memcpy(void *, const void *, size_t);
)cpp";

class ParameterEscapeExtractorTest : public TestFixture {
protected:
  SSAFOptions Opts;
  TUSummary TUSum{llvm::Triple("arm64-apple-macosx"),
                  BuildNamespace(BuildNamespaceKind::CompilationUnit, "Mock.cpp")};
  TUSummaryBuilder Builder{TUSum, Opts};
  std::unique_ptr<TUSummaryExtractor> Extractor;
  std::unique_ptr<ASTUnit> AST;

  bool setUp(StringRef Body, std::vector<std::string> Args = {"-std=c++20"},
             bool WithPrelude = true, tooling::FileContentMappings Files = {}) {
    std::string Code = WithPrelude ? (Prelude + Body.str()) : Body.str();
    AST = tooling::buildASTFromCodeWithArgs(
        Code, Args, "input.cc", "clang-tool", std::make_shared<PCHContainerOperations>(),
        tooling::getClangStripDependencyFileAdjuster(), Files);
    if (!AST || AST->getDiagnostics().hasErrorOccurred()) {
      ADD_FAILURE() << "test code has errors";
      return false;
    }
    if (!isTUSummaryExtractorRegistered(ParameterEscapeSummary::Name)) {
      ADD_FAILURE() << "ParameterEscape extractor not registered";
      return false;
    }
    Extractor = makeTUSummaryExtractor(ParameterEscapeSummary::Name, Builder);
    Extractor->HandleTranslationUnit(AST->getASTContext());
    return true;
  }

  const ParameterEscapeSummary *summaryOfDecl(const FunctionDecl *FD) {
    if (!FD) return nullptr;
    std::optional<EntityId> Id = Extractor->addEntity(FD);
    if (!Id) { ADD_FAILURE() << "no entity for " << FD->getNameAsString(); return nullptr; }
    auto &Data = getData(TUSum);
    auto It = Data.find(ParameterEscapeSummary::summaryName());
    if (It == Data.end()) return nullptr;
    auto E = It->second.find(*Id);
    if (E == It->second.end()) return nullptr;
    return static_cast<const ParameterEscapeSummary *>(E->second.get());
  }

  const ParameterEscapeSummary *summaryOf(StringRef Fn) {
    const FunctionDecl *FD = findFnByName(Fn, AST->getASTContext());
    if (!FD) { ADD_FAILURE() << "no function " << Fn; return nullptr; }
    return summaryOfDecl(FD);
  }

  // The user-declared constructor of `Class` with `NumParams` parameters.
  const ParameterEscapeSummary *ctorSummaryOf(StringRef Class, unsigned NumParams) {
    const auto *RD = findDeclByName<CXXRecordDecl>(Class, AST->getASTContext());
    if (!RD) { ADD_FAILURE() << "no class " << Class; return nullptr; }
    for (const CXXConstructorDecl *CD : RD->getDefinition()->ctors())
      if (!CD->isImplicit() && CD->getNumParams() == NumParams)
        return summaryOfDecl(CD);
    ADD_FAILURE() << "no ctor " << Class << "/" << NumParams;
    return nullptr;
  }

  const EscapeFact *factOf(StringRef Fn, unsigned Index) {
    const ParameterEscapeSummary *S = summaryOf(Fn);
    if (!S) return nullptr;
    auto It = S->Params.find(Index);
    return It == S->Params.end() ? nullptr : &It->second;
  }

  const EscapeFact *thisFactOf(StringRef Fn) {
    const ParameterEscapeSummary *S = summaryOf(Fn);
    return S && S->This ? &*S->This : nullptr;
  }

  // Sink reason, or nullopt when there is none. Fails the test if no fact.
  std::optional<EscapeReason> sinkOf(StringRef Fn, unsigned Index) {
    const EscapeFact *F = factOf(Fn, Index);
    if (!F) { ADD_FAILURE() << "no fact for " << Fn << "#" << Index; return std::nullopt; }
    return F->OtherSink ? std::optional(F->OtherSink->Reason) : std::nullopt;
  }

  bool flowsTo(StringRef Fn, unsigned Index, StringRef Callee, int CalleeIndex) {
    const EscapeFact *F = factOf(Fn, Index);
    const FunctionDecl *CD = findFnByName(Callee, AST->getASTContext());
    if (!F || !CD) return false;
    std::optional<EntityId> Id = Extractor->addEntity(CD);
    return Id && F->FlowsTo.count(FlowTarget{*Id, CalleeIndex});
  }

  // A parameter is "clean" when it neither sinks, nor returns, nor flows.
  bool clean(StringRef Fn, unsigned Index) {
    const EscapeFact *F = factOf(Fn, Index);
    if (!F) { ADD_FAILURE() << "no fact for " << Fn << "#" << Index; return false; }
    return !F->OtherSink && !F->returnsSelf() && F->FlowsTo.empty();
  }
};

//===--- Candidacy ---------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, PlainDefinitionIsCandidate) {
  ASSERT_TRUE(setUp("void f(int *p, int n, View v, int &r) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_TRUE(S->IsCandidate);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{0, 2, 3}));
}

TEST_F(ParameterEscapeExtractorTest, VirtualIsNotCandidate) {
  ASSERT_TRUE(setUp("struct B { virtual void f(int *p) { } };"));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, TemplateInstantiationIsAnalyzedButNotCandidate) {
  ASSERT_TRUE(setUp("template <class T> void tf(int *p) { } void use(int *q) { tf<int>(q); }"));
  const ParameterEscapeSummary *S = summaryOf("tf");
  ASSERT_NE(S, nullptr) << "instantiations must still be analyzed";
  EXPECT_FALSE(S->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, ClassTemplateMemberIsNotCandidate) {
  ASSERT_TRUE(setUp("template <class T> struct C { void m(int *p) { } }; void use(C<int> c, int *q) { c.m(q); }"));
  ASSERT_NE(summaryOf("m"), nullptr);
  EXPECT_FALSE(summaryOf("m")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, ExplicitSpecializationIsNotCandidate) {
  ASSERT_TRUE(setUp("template <class T> void ts(int *p); template <> void ts<int>(int *p) { }"));
  ASSERT_NE(summaryOf("ts"), nullptr);
  EXPECT_FALSE(summaryOf("ts")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, MacroRedeclIsNotCandidate) {
  ASSERT_TRUE(setUp("#define DECL(n) void n(int *p)\nDECL(f);\nvoid f(int *p) { }"));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, MacroTypedNonCandidateParameterDoesNotVeto) {
  ASSERT_TRUE(setUp("void f(int *p, __SIZE_TYPE__ n) { }"));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_TRUE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, SystemHeaderRedeclIsNotCandidate) {
  tooling::FileContentMappings Files = {{"/sys/s.h", "#pragma clang system_header\nvoid f(int *p);\n"}};
  ASSERT_TRUE(setUp("#include <s.h>\nvoid f(int *p) { }", {"-std=c++20", "-isystem/sys"}, true, Files));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, KAndRDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp("void f(p) int *p; { }", {"-x", "c", "-std=c17", "-Wno-deprecated-non-prototype"},
                    /*WithPrelude=*/false));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, CoroutineIsNotCandidate) {
  tooling::FileContentMappings Files = {{"/inc/std-coroutine.h", CoroutineHeader}};
  ASSERT_TRUE(setUp("#include <std-coroutine.h>\n"
                    "struct task { struct promise_type { task get_return_object(); "
                    "std::suspend_never initial_suspend(); std::suspend_never final_suspend() noexcept; "
                    "void return_void(); void unhandled_exception(); }; };\n"
                    "task co(int *p) { co_return; }",
                    {"-std=c++20", "-I/inc"}, true, Files));
  ASSERT_NE(summaryOf("co"), nullptr);
  EXPECT_FALSE(summaryOf("co")->IsCandidate);
  EXPECT_EQ(sinkOf("co", 0), EscapeReason::Coroutine);
}

TEST_F(ParameterEscapeExtractorTest, BodilessDeclarationContributesNothing) {
  ASSERT_TRUE(setUp("void decl_only(int *p);"));
  EXPECT_EQ(summaryOf("decl_only"), nullptr);
}

TEST_F(ParameterEscapeExtractorTest, CandidateTypesExcludeCallablesAndOwners) {
  ASSERT_TRUE(setUp("void f(void (*fp)(int), Owner o, SwiftView sv, NonTrivialView nv, int *const *pp) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{2, 4}));
}

} // namespace
```

`CoroutineHeader` is a `constexpr const char *` defined next to `Prelude` holding the contents of `clang/test/SemaCXX/Inputs/std-coroutine.h` (copy that file's text verbatim into the string literal; it defines `std::coroutine_traits`, `std::coroutine_handle`, `std::suspend_never`, `std::suspend_always`).

Add `Analyses/ParameterEscape/ParameterEscapeExtractorTest.cpp` to the unittest CMake list.

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='ParameterEscapeExtractor*'`
Expected: every test fails with `ParameterEscape extractor not registered` (the harness checks registration first, so the assertion inside `makeTUSummaryExtractor` is never reached).

- [ ] **Step 3: Implement the skeleton**

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.h`:

```c++
//===- EscapeClassifier.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Classifies every use of every parameter alias in one function definition.
// Private to the ParameterEscape extractor.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_ESCAPECLASSIFIER_H
#define LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_ESCAPECLASSIFIER_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryExtractor.h"
#include <map>
#include <optional>

namespace clang::ssaf {

bool isViewLikeRecordType(QualType T);
bool isTrackedViewType(QualType T);
bool isPointerCarryingType(QualType T);
bool isCandidateParameterType(QualType T);
bool isCandidateDefinition(const FunctionDecl *Def, ASTContext &Ctx);

struct FunctionEscapeFacts {
  std::map<unsigned, EscapeFact> Params;
  std::optional<EscapeFact> This;
};

FunctionEscapeFacts classifyFunctionEscapes(const FunctionDecl *Def,
                                            ASTContext &Ctx,
                                            TUSummaryExtractor &Extractor);

} // namespace clang::ssaf

#endif
```

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.cpp` — stub version (Task 6 replaces `classifyFunctionEscapes`):

```c++
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
#include "clang/Basic/SourceManager.h"

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
  if (!RD || !RD->hasDefinition())
    return false;
  return lifetimes::isGslPointerType(T) || hasSwiftNonEscapableAttr(RD->getDefinition());
}

bool clang::ssaf::isTrackedViewType(QualType T) {
  if (!isViewLikeRecordType(T))
    return false;
  const CXXRecordDecl *RD = T.getNonReferenceType()->getAsCXXRecordDecl()->getDefinition();
  return RD->isTriviallyCopyable() && RD->hasTrivialDestructor();
}

bool clang::ssaf::isPointerCarryingType(QualType T) {
  T = T.getCanonicalType();
  if (T->isReferenceType())
    return true;
  if (T->isAnyPointerType() && !T->isFunctionPointerType())
    return true;
  if (T->isBlockPointerType())
    return true;
  return isTrackedViewType(T);
}

bool clang::ssaf::isCandidateParameterType(QualType T) {
  T = T.getCanonicalType();
  if (T->isReferenceType())
    return true;
  if (T->isPointerType())
    return !T->isFunctionPointerType() && !T->getPointeeType()->isFunctionType();
  return isTrackedViewType(T);
}

bool clang::ssaf::isCandidateDefinition(const FunctionDecl *Def, ASTContext &Ctx) {
  if (!Def->doesThisDeclarationHaveABody() || Def->isMain() ||
      isa<CXXDeductionGuideDecl>(Def))
    return false;
  if (const auto *MD = dyn_cast<CXXMethodDecl>(Def); MD && MD->isVirtual())
    return false;
  if (Def->isTemplated() || Def->getDescribedFunctionTemplate() ||
      Def->getTemplateSpecializationKind() != TSK_Undeclared ||
      Def->isTemplateInstantiation() || Def->getInstantiatedFromMemberFunction() ||
      Def->getTemplateInstantiationPattern())
    return false;
  if (isa_and_nonnull<CoroutineBodyStmt>(Def->getBody()))
    return false;
  const SourceManager &SM = Ctx.getSourceManager();
  for (const FunctionDecl *RD : Def->redecls()) {
    if (!RD->hasWrittenPrototype() || RD->isTemplated() ||
        RD->getDeclContext()->isDependentContext())
      return false;
    SourceLocation Loc = RD->getLocation();
    if (Loc.isInvalid() || Loc.isMacroID() || SM.isInSystemHeader(Loc))
      return false;
    // Only parameters we may annotate need a rewritable begin location.
    for (const ParmVarDecl *P : RD->parameters())
      if (isCandidateParameterType(P->getType()) &&
          (P->getBeginLoc().isInvalid() || P->getBeginLoc().isMacroID()))
        return false;
  }
  return true;
}

static SourceLocationRecord recordFor(SourceLocation Loc, const SourceManager &SM) {
  PresumedLoc P = SM.getPresumedLoc(SM.getExpansionLoc(Loc));
  if (P.isInvalid())
    return SourceLocationRecord{"<unknown>", 0, 0};
  return SourceLocationRecord{P.getFilename(), P.getLine(), P.getColumn()};
}

// Sound placeholder until Task 6: every pointer-carrying parameter escapes.
FunctionEscapeFacts clang::ssaf::classifyFunctionEscapes(const FunctionDecl *Def,
                                                         ASTContext &Ctx,
                                                         TUSummaryExtractor &) {
  FunctionEscapeFacts Facts;
  const SourceManager &SM = Ctx.getSourceManager();
  bool IsCoroutine = isa_and_nonnull<CoroutineBodyStmt>(Def->getBody());
  EscapeReason R = IsCoroutine ? EscapeReason::Coroutine : EscapeReason::UnrecognizedUse;
  for (const ParmVarDecl *P : Def->parameters()) {
    if (!isPointerCarryingType(P->getType()))
      continue;
    EscapeFact F;
    F.OtherSink = Sink{R, recordFor(Def->getLocation(), SM), "classifier stub"};
    Facts.Params[P->getFunctionScopeIndex()] = std::move(F);
  }
  if (const auto *MD = dyn_cast<CXXMethodDecl>(Def); MD && MD->isInstance()) {
    EscapeFact F;
    F.OtherSink = Sink{R, recordFor(Def->getLocation(), SM), "classifier stub"};
    Facts.This = std::move(F);
  }
  return Facts;
}
```

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractor.cpp`:

```c++
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
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/ExtractorRegistry.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryBuilder.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryExtractor.h"
#include <memory>

using namespace clang;
using namespace clang::ssaf;

namespace {

class ParameterEscapeTUSummaryExtractor : public TUSummaryExtractor {
public:
  using TUSummaryExtractor::TUSummaryExtractor;

  std::unique_ptr<ParameterEscapeSummary>
  extractEntitySummary(const std::vector<const NamedDecl *> &Decls, ASTContext &Ctx) {
    auto S = std::make_unique<ParameterEscapeSummary>();
    const FunctionDecl *Def = nullptr;
    for (const NamedDecl *D : Decls)
      if (const auto *FD = dyn_cast<FunctionDecl>(D);
          FD && FD->doesThisDeclarationHaveABody())
        Def = FD;
    if (!Def)
      return S; // bodiless declarations and non-functions contribute nothing
    S->IsCandidate = isCandidateDefinition(Def, Ctx);
    for (const ParmVarDecl *P : Def->parameters())
      if (isCandidateParameterType(P->getType()))
        S->CandidateParams.insert(P->getFunctionScopeIndex());
    FunctionEscapeFacts Facts = classifyFunctionEscapes(Def, Ctx, *this);
    S->Params = std::move(Facts.Params);
    S->This = std::move(Facts.This);
    return S;
  }

  void HandleTranslationUnit(ASTContext &Ctx) override {
    extractAndAddSummaries(
        *this, SummaryBuilder, Ctx,
        [&](const std::vector<const NamedDecl *> &Decls) {
          return extractEntitySummary(Decls, Ctx);
        },
        "ParameterEscape");
  }
};

} // namespace

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int ParameterEscapeExtractorAnchorSource = 0;
} // namespace clang::ssaf

static TUSummaryExtractorRegistry::Add<ParameterEscapeTUSummaryExtractor>
    RegisterExtractor(ParameterEscapeSummary::Name,
                      "Extract per-parameter escape facts for noescape inference");
```

Wiring: add `ParameterEscape/EscapeClassifier.cpp` and `ParameterEscape/ParameterEscapeExtractor.cpp` to the Analyses CMake source list; add `clangAnalysisLifetimeSafety` to its `LINK_LIBS`; add `ANCHOR(ParameterEscapeExtractorAnchorSource)` to `BuiltinAnchorSources.def` (alphabetical, right before `ParameterEscapeJSONFormatAnchorSource`).

The stub is *sound* (every parameter escapes), so the tree is never in a state where the pipeline could emit a wrong annotation.

- [ ] **Step 4: Run the tests**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='ParameterEscapeExtractor*'`
Expected: all 13 PASS.

- [ ] **Step 5: Commit**

```bash
git add clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt clang/include/clang/ScalableStaticAnalysis/BuiltinAnchorSources.def clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractorTest.cpp clang/unittests/ScalableStaticAnalysis/CMakeLists.txt
git commit -m "[SSAF][ParameterEscape] Add extractor skeleton with candidacy and sound stub facts

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: The escape classifier — alias derivation and use classification

**Files:**
- Modify: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.cpp` (replace the stub `classifyFunctionEscapes`; keep the predicates and `recordFor`)
- Test: append to `ParameterEscapeExtractorTest.cpp`

**Interfaces:**
- Consumes: `resolveCallSite` (Task 1), `LibraryFunctionKnowledge` (Task 4), predicates from Task 5.
- Produces: the real `classifyFunctionEscapes`, implementing spec §5.2 for one source at a time (each pointer-carrying parameter, then `this`).

Design notes the code below follows (they are the fixes from the v1 review):
- `DynamicRecursiveASTVisitor` has no `VisitExpr`; use `VisitStmt` and `dyn_cast<Expr>`.
- A use is classified at every alias-kinded expression **unless** its parent is a *pass-through* consumer of exactly that operand position (`isCoveredByParent`). Call arguments, implicit objects, and assignment right-hand sides are never pass-through, so `int *q = id(p)` records `FlowsTo(id, 0)` and `*pp = pp` records a store.
- `ParentMapContext` does not model `CXXCtorInitializer`; the parent of a member-initializer expression is the `CXXConstructorDecl`, handled by `classifyCtorInitUse` via `CD->inits()`.
- Reference-typed variables are never local storage for stores (`isLocalPointerStorage` excludes them); they join the alias set only through their own initializer (`isLocalReference`).
- `BindingDecl` references resolve through `getBinding()`; `StmtExpr` propagates its last expression; `__block` and `cleanup` variables are never alias storage.
- glvalue call results are `VarLValue` when the referent is pointer-carrying (e.g. `int *&get()`), else `Place` (e.g. `v[i]`).

- [ ] **Step 1: Write the failing tests (core rows)**

Append inside the anonymous namespace of the test file:

```c++
//===--- Benign uses -------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ReadsComparesAndBoolAreBenign) {
  ASSERT_TRUE(setUp("int f(int *p, int *q, int &r) { int x = *p + p[1] + r; if (p && p == q) x++; return x + (int)sizeof(*p); }"));
  EXPECT_TRUE(clean("f", 0));
  EXPECT_TRUE(clean("f", 1));
  EXPECT_TRUE(clean("f", 2));
}

TEST_F(ParameterEscapeExtractorTest, WritesThroughAliasAreBenign) {
  ASSERT_TRUE(setUp("struct S { int m; int *n; }; void f(S *s, int *p, int &r) { *p = 1; p[2] = 3; s->m = 4; r = 5; s->n = g_ptr; }"));
  EXPECT_TRUE(clean("f", 0));
  EXPECT_TRUE(clean("f", 1));
  EXPECT_TRUE(clean("f", 2));
}

TEST_F(ParameterEscapeExtractorTest, LoadsThroughNonViewAliasAreFresh) {
  ASSERT_TRUE(setUp("struct N { N *next; int *v; }; void f(N *n) { g_ptr = n->v; N *m = n->next; g_ptr = m->v; }"));
  EXPECT_TRUE(clean("f", 0));
}

TEST_F(ParameterEscapeExtractorTest, DeclaredNoescapeCalleeIsBenign) {
  ASSERT_TRUE(setUp("void f(int *p) { noesc(p); }"));
  EXPECT_TRUE(clean("f", 0));
}

TEST_F(ParameterEscapeExtractorTest, LibraryKnowledgeIsBenign) {
  ASSERT_TRUE(setUp("unsigned f(const char *s, char *d) { memcpy(d, s, 1); return strlen(s); }"));
  EXPECT_TRUE(clean("f", 0));
  // memcpy's destination is only 'returned' in LLVM's model: not trusted, so
  // it is an ordinary flow (the fixpoint rejects it as an unanalyzed callee).
  EXPECT_TRUE(flowsTo("f", 1, "memcpy", 0));
  EXPECT_EQ(sinkOf("f", 1), std::nullopt);
}

TEST_F(ParameterEscapeExtractorTest, LocalScalarCopiesJoinAliasSet) {
  ASSERT_TRUE(setUp("void f(int *p) { int *q = p; int *r; r = q + 1; int &x = *r; g_ptr = &x; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, DiscardedValuesAreBenign) {
  ASSERT_TRUE(setUp("void f(int *p) { p; (void)p; for (int *q = p; q; ++q) {} }"));
  EXPECT_TRUE(clean("f", 0));
}

//===--- Flows -------------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ArgumentsFlowToCalleeParameters) {
  ASSERT_TRUE(setUp("void callee(int *a, int *b); void f(int *p, int &r) { callee(p, &r); }"));
  EXPECT_TRUE(flowsTo("f", 0, "callee", 0));
  EXPECT_TRUE(flowsTo("f", 1, "callee", 1));
  EXPECT_EQ(sinkOf("f", 0), std::nullopt);
}

TEST_F(ParameterEscapeExtractorTest, ImplicitObjectFlowsToThis) {
  ASSERT_TRUE(setUp("void f(Owner *o, int *p) { o->push(p); }"));
  EXPECT_TRUE(flowsTo("f", 0, "push", ThisParamIndex));
  EXPECT_TRUE(flowsTo("f", 1, "push", 0));
}

TEST_F(ParameterEscapeExtractorTest, CallResultsAliasArgumentsAndArgumentsStillFlow) {
  ASSERT_TRUE(setUp("int *id(int *x); void f(int *p) { int *q = id(p); g_ptr = q; } void g(int *p) { (void)id(p); }"));
  EXPECT_TRUE(flowsTo("f", 0, "id", 0));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(flowsTo("g", 0, "id", 0));
  EXPECT_EQ(sinkOf("g", 0), std::nullopt);
}

TEST_F(ParameterEscapeExtractorTest, NonPointerCallResultsAreFresh) {
  ASSERT_TRUE(setUp("int len(int *x); void f(int *p) { int n = len(p); (void)n; }"));
  EXPECT_TRUE(flowsTo("f", 0, "len", 0));
  EXPECT_EQ(sinkOf("f", 0), std::nullopt);
}

TEST_F(ParameterEscapeExtractorTest, FlowRecordsTheCallSite) {
  ASSERT_TRUE(setUp("void callee(int *a);\nvoid f(int *p) {\n  callee(p);\n}"));
  const EscapeFact *F = factOf("f", 0);
  ASSERT_TRUE(F && F->FlowsTo.size() == 1);
  EXPECT_GT(F->FlowsTo.begin()->second.Line, 0u);
}

//===--- Return ------------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ReturnIsRecorded) {
  ASSERT_TRUE(setUp("int *f(int *p) { return p; } int &g(int *p) { return *p; } int h(int *p) { return *p; }"));
  EXPECT_TRUE(factOf("f", 0)->returnsSelf());
  EXPECT_TRUE(factOf("g", 0)->returnsSelf());
  EXPECT_FALSE(factOf("h", 0)->returnsSelf());
}

//===--- Sinks -------------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, StoreSinks) {
  ASSERT_TRUE(setUp(R"cpp(
    struct S { int *m; };
    void to_global(int *p) { g_ptr = p; }
    void to_static(int *p) { s_ptr = p; }
    void to_static_local(int *p) { static int *l; l = p; }
    void to_thread_local(int *p) { static thread_local int *t; t = p; }
    void to_field(S *s, int *p) { s->m = p; }
    void to_local_aggregate(int *p) { S s; s.m = p; }
    void to_init_list(int *p) { S s{p}; (void)s; }
    void through_pointer(int **pp, int *p) { *pp = p; }
    void through_subscript(int **arr, int *p) { arr[0] = p; }
    void through_ref_param(int *&out, int *p) { out = p; }
    void through_local_ref(int *p) { int *&r = g_ptr; r = p; }
    void self_store(void **pp) { *pp = pp; }
  )cpp"));
  EXPECT_EQ(sinkOf("to_global", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("to_static", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("to_static_local", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("to_thread_local", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("to_field", 1), EscapeReason::StoreToField);
  EXPECT_TRUE(clean("to_field", 0));
  EXPECT_EQ(sinkOf("to_local_aggregate", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("to_init_list", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("through_pointer", 1), EscapeReason::StoreThroughPointer);
  EXPECT_TRUE(clean("through_pointer", 0));
  EXPECT_EQ(sinkOf("through_subscript", 1), EscapeReason::StoreThroughPointer);
  EXPECT_EQ(sinkOf("through_ref_param", 1), EscapeReason::StoreThroughPointer);
  EXPECT_EQ(sinkOf("through_local_ref", 0), EscapeReason::StoreThroughPointer);
  EXPECT_EQ(sinkOf("self_store", 0), EscapeReason::StoreThroughPointer);
}

TEST_F(ParameterEscapeExtractorTest, AddressAndReferenceToTheVariable) {
  ASSERT_TRUE(setUp("void take(int **); void f(int *p) { take(&p); } void g(int *p) { int *&r = p; (void)r; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::AddressTaken);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::AddressTaken);
}

TEST_F(ParameterEscapeExtractorTest, ArrayMemberDecayIsAnInteriorPointer) {
  ASSERT_TRUE(setUp("struct A { int a[4]; }; void f(A *s) { g_ptr = s->a; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, StatementExpressionPropagates) {
  ASSERT_TRUE(setUp("void f(int *p) { g_ptr = ({ p; }); }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, StructuredBindingsResolveThroughTheirBinding) {
  ASSERT_TRUE(setUp("struct P { int x, y; }; void f(P *ps) { auto &[a, b] = *ps; g_ptr = &a; } void g(P *ps) { auto &[a, b] = *ps; int s = a + b; (void)s; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("g", 0));
}

TEST_F(ParameterEscapeExtractorTest, CleanupVariablesAreNotAliasStorage) {
  ASSERT_TRUE(setUp("void keep(int **); void f(int *p) { int *q __attribute__((cleanup(keep))) = p; (void)q; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::AddressTaken);
}

TEST_F(ParameterEscapeExtractorTest, CallSinks) {
  ASSERT_TRUE(setUp(R"cpp(
    void v(int, ...);
    void indirect(void (*fp)(int *), int *p) { fp(p); }
    void variadic(int *p) { v(1, p); }
    void dealloc(void *p) { free(p); }
    void del(int *p) { delete p; }
    void heap(int *p) { new Holder(p); }
    struct B { virtual void m(int *); };
    void virt(B *b, int *p) { b->m(p); }
  )cpp"));
  EXPECT_EQ(sinkOf("indirect", 1), EscapeReason::IndirectCall);
  EXPECT_EQ(sinkOf("variadic", 0), EscapeReason::UnmatchedArgument);
  EXPECT_EQ(sinkOf("dealloc", 0), EscapeReason::Deallocation);
  EXPECT_EQ(sinkOf("del", 0), EscapeReason::Deallocation);
  EXPECT_EQ(sinkOf("heap", 0), EscapeReason::HeapAllocation);
  EXPECT_EQ(sinkOf("virt", 0), EscapeReason::VirtualCall);
  EXPECT_EQ(sinkOf("virt", 1), EscapeReason::VirtualCall);
}

TEST_F(ParameterEscapeExtractorTest, ConversionThrowAsmAndCaptureSinks) {
  ASSERT_TRUE(setUp(R"cpp(
    void cast(int *p) { __UINTPTR_TYPE__ u = (__UINTPTR_TYPE__)p; (void)u; }
    void thr(int *p) { throw p; }
    void asm_(int *p) { __asm__("" : : "r"(p)); }
    void lam(int *p) { auto l = [p] { return *p; }; (void)l; }
    void lam_ref(int *p) { auto l = [&] { return *p; }; (void)l; }
  )cpp"));
  EXPECT_EQ(sinkOf("cast", 0), EscapeReason::CastToNonPointer);
  EXPECT_EQ(sinkOf("thr", 0), EscapeReason::Throw);
  EXPECT_EQ(sinkOf("asm_", 0), EscapeReason::Asm);
  EXPECT_EQ(sinkOf("lam", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("lam_ref", 0), EscapeReason::Capture);
}

TEST_F(ParameterEscapeExtractorTest, UnrecognizedUseIsTheDefault) {
  // An atomic builtin operating on the alias value is deliberately unmodeled.
  ASSERT_TRUE(setUp("void f(int *p) { __atomic_store_n(&g_ptr, p, __ATOMIC_SEQ_CST); }"));
  std::optional<EscapeReason> R = sinkOf("f", 0);
  ASSERT_TRUE(R.has_value()) << "must be some sink";
  EXPECT_TRUE(*R == EscapeReason::UnrecognizedUse || *R == EscapeReason::UnnamedCallee);
}

TEST_F(ParameterEscapeExtractorTest, FirstSinkOnlyIsRecordedWithLocation) {
  ASSERT_TRUE(setUp("void f(int *p) {\n g_ptr = p;\n s_ptr = p; }"));
  const EscapeFact *F = factOf("f", 0);
  ASSERT_TRUE(F && F->OtherSink);
  EXPECT_EQ(F->OtherSink->Reason, EscapeReason::StoreToGlobal);
  EXPECT_GT(F->OtherSink->Location.Line, 0u);
}
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='ParameterEscapeExtractor*'`
Expected: the Task 5 tests pass; every new test fails (the stub reports `UnrecognizedUse` everywhere).

- [ ] **Step 3: Implement the classifier**

Replace the stub `classifyFunctionEscapes` in `EscapeClassifier.cpp` with the following (keep the predicates and `recordFor`). Add includes: `clang/AST/DynamicRecursiveASTVisitor.h`, `clang/AST/ExprCXX.h`, `clang/AST/ExprObjC.h`, `clang/AST/ParentMapContext.h`, `clang/AST/StmtObjC.h`, `clang/AST/OperationKinds.h`, `clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h`, `clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h`, `llvm/ADT/DenseMap.h`, `llvm/ADT/SmallPtrSet.h`, `llvm/ADT/STLExtras.h`.

```c++
namespace {

/// What an expression denotes with respect to the current source parameter.
enum class AliasKind : uint8_t {
  None,      ///< unrelated to the source
  Value,     ///< a pointer/reference/view value carrying the source's provenance
  Place,     ///< a glvalue denoting the pointee object or one of its subobjects
  VarLValue, ///< the lvalue of a variable (or referent) that holds an alias value
  ViewField, ///< a pointer-carrying field of a tracked-view alias
};

struct Source {
  const ParmVarDecl *Param = nullptr; // null ⇒ the implicit object parameter
  bool isThis() const { return Param == nullptr; }
};

class Classifier {
public:
  Classifier(const FunctionDecl *Def, ASTContext &Ctx, TUSummaryExtractor &Extractor)
      : Def(Def), Ctx(Ctx), SM(Ctx.getSourceManager()), Extractor(Extractor) {
    const auto *MD = dyn_cast<CXXMethodDecl>(Def);
    Class = MD ? MD->getParent() : nullptr;
    ThisIsTrackedView = Class && isTrackedViewType(Ctx.getCanonicalTagType(Class));
    DefIsTrackedViewCtor = ThisIsTrackedView && isa<CXXConstructorDecl>(Def);
  }

  EscapeFact analyze(Source S) {
    Src = S;
    Fact = EscapeFact();
    AliasVars.clear();
    if (Src.Param)
      AliasVars.insert(Src.Param);
    growAliasSet();
    Memo.clear();
    UseVisitor(*this).run();
    return Fact;
  }

private:
  const FunctionDecl *Def;
  ASTContext &Ctx;
  const SourceManager &SM;
  TUSummaryExtractor &Extractor;
  const CXXRecordDecl *Class = nullptr;
  bool ThisIsTrackedView = false;
  bool DefIsTrackedViewCtor = false;
  std::optional<EntityId> SelfId;

  Source Src;
  llvm::SmallPtrSet<const VarDecl *, 8> AliasVars;
  llvm::DenseMap<const Expr *, AliasKind> Memo;
  EscapeFact Fact;

  //===--- recording -------------------------------------------------------===//

  void sink(EscapeReason R, SourceLocation Loc, llvm::StringRef Detail = "") {
    if (!Fact.OtherSink)
      Fact.OtherSink = Sink{R, recordFor(Loc, SM), Detail.str()};
  }
  void returnsSelf(SourceLocation Loc) {
    if (!Fact.ReturnsSelfAt)
      Fact.ReturnsSelfAt = recordFor(Loc, SM);
  }
  void flowsTo(const FunctionDecl *Callee, int Index, SourceLocation Loc) {
    std::optional<EntityId> Id = Extractor.addEntity(Callee);
    if (!Id)
      return sink(EscapeReason::UnnamedCallee, Loc, Callee->getNameAsString());
    Fact.FlowsTo.try_emplace(FlowTarget{*Id, Index}, recordFor(Loc, SM));
  }
  void flowsToSelfThis(SourceLocation Loc) {
    if (!SelfId)
      SelfId = Extractor.addEntity(Def);
    if (!SelfId)
      return sink(EscapeReason::UnnamedCallee, Loc, Def->getNameAsString());
    Fact.FlowsTo.try_emplace(FlowTarget{*SelfId, ThisParamIndex}, recordFor(Loc, SM));
  }

  //===--- helpers ---------------------------------------------------------===//

  /// A local automatic, non-reference variable that can hold an alias value.
  static bool isLocalPointerStorage(const VarDecl *VD) {
    return VD->hasLocalStorage() && !VD->getType()->isReferenceType() &&
           !VD->hasAttr<BlocksAttr>() && !VD->hasAttr<CleanupAttr>() &&
           !isa<ImplicitParamDecl>(VD) && isPointerCarryingType(VD->getType());
  }
  /// A local reference; it can alias only through its own initializer.
  static bool isLocalReference(const VarDecl *VD) {
    return VD->hasLocalStorage() && VD->getType()->isReferenceType() &&
           !VD->hasAttr<CleanupAttr>() && !isa<ImplicitParamDecl>(VD);
  }

  static bool paramIsDeclaredNoescape(const FunctionDecl *FD, unsigned I) {
    for (const FunctionDecl *R : FD->redecls())
      if (I < R->getNumParams() && R->getParamDecl(I)->hasAttr<NoEscapeAttr>())
        return true;
    if (const auto *FPT = FD->getType()->getAs<FunctionProtoType>())
      return I < FPT->getNumParams() && FPT->getExtParameterInfo(I).isNoEscape();
    return false;
  }

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

  static bool sameExpr(const Expr *A, const Expr *B) {
    return A == B || A->IgnoreParenImpCasts() == B->IgnoreParenImpCasts();
  }

  //===--- alias kinds (spec §5.2 derivations) -----------------------------===//

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
    if (T->isReferenceType())
      return isPointerCarryingType(T.getNonReferenceType()) ? AliasKind::VarLValue
                                                             : AliasKind::Place;
    return AliasKind::VarLValue;
  }

  AliasKind kindOfCast(const CastExpr *C) {
    AliasKind S = kind(C->getSubExpr());
    switch (C->getCastKind()) {
    case CK_LValueToRValue:
      return (S == AliasKind::VarLValue || S == AliasKind::ViewField || S == AliasKind::Value)
                 ? AliasKind::Value
                 : AliasKind::None;
    case CK_ArrayToPointerDecay:
      return S == AliasKind::Place ? AliasKind::Value : S;
    case CK_PointerToBoolean:
    case CK_PointerToIntegral:
    case CK_ToVoid:
    case CK_IntegralToPointer:
    case CK_NullToPointer:
    case CK_FunctionToPointerDecay:
    case CK_BuiltinFnToFnPtr:
      return AliasKind::None;
    default:
      return (isPointerCarryingType(C->getType()) || C->isGLValue()) ? S : AliasKind::None;
    }
  }

  AliasKind kindOfMember(const MemberExpr *ME) {
    if (!isa<FieldDecl>(ME->getMemberDecl()))
      return AliasKind::None; // method reference: handled as a call
    const Expr *Base = ME->getBase();
    AliasKind B = kind(Base);
    bool FieldCarries = isPointerCarryingType(ME->getType());
    if (isa<CXXThisExpr>(Base->IgnoreParenImpCasts()) && Src.isThis() && ThisIsTrackedView)
      return FieldCarries ? AliasKind::ViewField : AliasKind::None;
    switch (B) {
    case AliasKind::None:
      return AliasKind::None;
    case AliasKind::Value:
      if (ME->isArrow())
        return AliasKind::Place;
      return (isTrackedViewType(Base->getType()) && FieldCarries) ? AliasKind::Value
                                                                  : AliasKind::Place;
    case AliasKind::Place:
      return AliasKind::Place;
    case AliasKind::VarLValue:
    case AliasKind::ViewField:
      return (isTrackedViewType(Base->getType()) && FieldCarries) ? AliasKind::ViewField
                                                                  : AliasKind::None;
    }
    llvm_unreachable("covered");
  }

  AliasKind kindOfCall(const Expr *E) {
    std::optional<CallSite> CS = resolveCallSite(E);
    if (!CS)
      return AliasKind::None;
    bool Carries = isPointerCarryingType(E->getType());
    if (!Carries && !E->isGLValue())
      return AliasKind::None;
    bool AnyAlias = CS->ImplicitObjectArg && kind(CS->ImplicitObjectArg) != AliasKind::None;
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
      return GSE->isResultDependent() ? AliasKind::None : kind(GSE->getResultExpr());
    if (const auto *CE = dyn_cast<ChooseExpr>(E))
      return kind(CE->getChosenSubExpr());
    if (const auto *SE = dyn_cast<StmtExpr>(E)) {
      const CompoundStmt *Body = SE->getSubStmt();
      if (Body->body_empty())
        return AliasKind::None;
      return kind(dyn_cast<Expr>(Body->body_back()));
    }
    if (const auto *C = dyn_cast<CastExpr>(E))
      return kindOfCast(C);
    if (const auto *ME = dyn_cast<MemberExpr>(E))
      return kindOfMember(ME);
    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
      AliasKind S = kind(UO->getSubExpr());
      switch (UO->getOpcode()) {
      case UO_AddrOf:
        return S == AliasKind::Place ? AliasKind::Value : AliasKind::None;
      case UO_Deref:
        return S == AliasKind::Value ? AliasKind::Place : AliasKind::None;
      case UO_PostInc: case UO_PostDec: case UO_PreInc: case UO_PreDec:
        return S == AliasKind::VarLValue ? AliasKind::Value : AliasKind::None;
      case UO_Plus:
        return S;
      default:
        return AliasKind::None;
      }
    }
    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      switch (BO->getOpcode()) {
      case BO_Add: case BO_Sub:
        if (!isPointerCarryingType(BO->getType()))
          return AliasKind::None;
        return (kind(BO->getLHS()) == AliasKind::Value || kind(BO->getRHS()) == AliasKind::Value)
                   ? AliasKind::Value : AliasKind::None;
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
      return (B == AliasKind::Value || B == AliasKind::Place) ? AliasKind::Place : AliasKind::None;
    }
    if (isa<CallExpr, CXXConstructExpr>(E))
      return kindOfCall(E);
    return AliasKind::None;
  }

  //===--- growth (locals that receive aliases) -----------------------------===//

  bool growByInit(const VarDecl *VD, AliasKind K) {
    if (K == AliasKind::None)
      return false;
    if (isLocalReference(VD)) {
      // Binding a reference to the pointer *variable* (K == VarLValue) is an
      // escape handled by the use rule, not growth.
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

  class GrowthVisitor : public DynamicRecursiveASTVisitor {
    Classifier &C;
  public:
    bool Changed = false;
    explicit GrowthVisitor(Classifier &C) : C(C) { ShouldVisitImplicitCode = true; }
    bool TraverseDecl(Decl *D) override {
      if (D && D != C.Def && isa<FunctionDecl, RecordDecl, BlockDecl, ObjCMethodDecl>(D))
        return true;
      return DynamicRecursiveASTVisitor::TraverseDecl(D);
    }
    bool TraverseLambdaExpr(LambdaExpr *) override { return true; }
    bool TraverseBlockExpr(BlockExpr *) override { return true; }
    bool VisitVarDecl(VarDecl *VD) override {
      if (VD->hasInit())
        Changed |= C.growByInit(VD, C.kind(VD->getInit()));
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
    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *OCE) override {
      // Trivial copy/move assignment of a tracked view: `v2 = v1;`
      const FunctionDecl *FD = OCE->getDirectCallee();
      if (!FD || OCE->getOperator() != OO_Equal || OCE->getNumArgs() != 2 ||
          !isTrivialImplicitCopyOrMove(FD) || !isTrackedViewType(OCE->getArg(0)->getType()))
        return true;
      if (const auto *DRE = dyn_cast<DeclRefExpr>(OCE->getArg(0)->IgnoreParenImpCasts()))
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
          Changed |= C.growByAssign(VD, C.kind(OCE->getArg(1)));
      return true;
    }
  };

  void growAliasSet() {
    for (;;) {
      Memo.clear();
      GrowthVisitor V(*this);
      V.TraverseDecl(const_cast<FunctionDecl *>(Def));
      if (!V.Changed)
        return;
    }
  }

  //===--- use classification (spec §5.2 table) ----------------------------===//

  /// True when PE (an alias-kinded parent) fully accounts for E's use, so E
  /// itself needs no classification. Call arguments, implicit objects, and
  /// assignment right-hand sides are never covered.
  bool isCoveredByParent(const Expr *E, const Expr *PE) {
    if (kind(PE) == AliasKind::None)
      return false;
    if (isa<ParenExpr, FullExpr, MaterializeTemporaryExpr, CXXBindTemporaryExpr, OpaqueValueExpr,
            CXXDefaultArgExpr, CXXDefaultInitExpr, GenericSelectionExpr, ChooseExpr, StmtExpr,
            CastExpr, MemberExpr, ArraySubscriptExpr, AbstractConditionalOperator,
            UnaryOperator>(PE))
      return true;
    if (const auto *BO = dyn_cast<BinaryOperator>(PE)) {
      if (BO->getOpcode() == BO_Assign || isa<CompoundAssignOperator>(BO))
        return BO->getLHS() == E; // the RHS is a store and must be classified
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
    void run() { TraverseDecl(const_cast<FunctionDecl *>(C.Def)); }
    bool TraverseDecl(Decl *D) override {
      if (D && D != C.Def && isa<FunctionDecl, RecordDecl, BlockDecl, ObjCMethodDecl>(D))
        return true;
      return DynamicRecursiveASTVisitor::TraverseDecl(D);
    }
    bool TraverseLambdaExpr(LambdaExpr *LE) override {
      for (const LambdaCapture &Cap : LE->captures()) {
        if (Cap.capturesThis() && C.Src.isThis())
          C.sink(EscapeReason::Capture, LE->getBeginLoc());
        if (Cap.capturesVariable())
          if (const auto *VD = dyn_cast<VarDecl>(Cap.getCapturedVar());
              VD && C.AliasVars.count(VD))
            C.sink(EscapeReason::Capture, Cap.getLocation());
      }
      for (const Expr *Init : LE->capture_inits())
        if (Init && C.kind(Init) != AliasKind::None)
          C.sink(EscapeReason::Capture, Init->getBeginLoc());
      return true; // the body is not traversed: any capture already sank
    }
    bool TraverseBlockExpr(BlockExpr *BE) override {
      for (const BlockDecl::Capture &Cap : BE->getBlockDecl()->captures())
        if (C.AliasVars.count(Cap.getVariable()))
          C.sink(EscapeReason::Capture, BE->getBeginLoc());
      if (BE->getBlockDecl()->capturesCXXThis() && C.Src.isThis())
        C.sink(EscapeReason::Capture, BE->getBeginLoc());
      return true;
    }
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

  void classifyStoreInto(const Expr *Target, SourceLocation Loc) {
    const Expr *T = Target->IgnoreParenImpCasts();
    if (const auto *DRE = dyn_cast<DeclRefExpr>(T)) {
      if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (isLocalPointerStorage(VD))
          return; // growth already recorded it
        if (VD->getType()->isReferenceType())
          return sink(EscapeReason::StoreThroughPointer, Loc, VD->getNameAsString());
        if (VD->hasAttr<BlocksAttr>())
          return sink(EscapeReason::Capture, Loc);
        if (VD->hasAttr<CleanupAttr>())
          return sink(EscapeReason::AddressTaken, Loc);
        return sink(EscapeReason::StoreToGlobal, Loc, VD->getNameAsString());
      }
      return sink(EscapeReason::StoreThroughPointer, Loc);
    }
    if (const auto *ME = dyn_cast<MemberExpr>(T)) {
      if (DefIsTrackedViewCtor && isa<CXXThisExpr>(ME->getBase()->IgnoreParenImpCasts()))
        return flowsToSelfThis(Loc);
      return sink(EscapeReason::StoreToField, Loc, ME->getMemberDecl()->getNameAsString());
    }
    return sink(EscapeReason::StoreThroughPointer, Loc);
  }

  void classifyCallUse(const Expr *M, AliasKind K, const Expr *Call) {
    SourceLocation Loc = M->getBeginLoc();
    std::optional<CallSite> CS = resolveCallSite(Call);
    if (!CS || !CS->Callee)
      return sink(EscapeReason::IndirectCall, Loc);
    const FunctionDecl *Callee = CS->Callee;
    if (llvm::any_of(CS->UnmatchedArgs, [&](const Expr *A) { return sameExpr(A, M); }))
      return sink(EscapeReason::UnmatchedArgument, Loc);
    if (isDeallocationOperator(Callee) || LibraryFunctionKnowledge::isDeallocationFunction(Callee, Ctx))
      return sink(EscapeReason::Deallocation, Loc);
    const auto *MD = dyn_cast<CXXMethodDecl>(Callee);
    if (CS->ImplicitObjectArg && sameExpr(CS->ImplicitObjectArg, M)) {
      if (MD && MD->isVirtual())
        return sink(EscapeReason::VirtualCall, Loc);
      if (isTrivialImplicitCopyOrMove(Callee)) {
        if (isTrackedViewType(M->getType()))
          return classifyStoreInto(M, Loc); // `v2 = v1` stores into v2
        return; // trivially copying *from* the object is a load
      }
      return flowsTo(Callee, ThisParamIndex, Loc);
    }
    for (auto [Arg, Idx] : CS->Arguments) {
      if (!sameExpr(Arg, M))
        continue;
      if (paramIsDeclaredNoescape(Callee, Idx))
        return;
      if (LibraryFunctionKnowledge::parameterDoesNotEscape(Callee, Idx, Ctx))
        return;
      if (isTrivialImplicitCopyOrMove(Callee)) {
        if (K == AliasKind::Place)
          return; // copying the pointee by value is a load
        if (isa<CXXConstructorDecl>(Callee) && isTrackedViewType(Call->getType()))
          return; // the copy is an alias; its own uses are classified
        return classifyStoreInto(CS->ImplicitObjectArg ? CS->ImplicitObjectArg : Call, Loc);
      }
      if (MD && MD->isVirtual())
        return sink(EscapeReason::VirtualCall, Loc);
      if (isa<CXXConstructorDecl>(Callee) && isViewLikeRecordType(Call->getType()) &&
          !isTrackedViewType(Call->getType()))
        return sink(EscapeReason::NonTrivialView, Loc);
      return flowsTo(Callee, Idx, Loc);
    }
    // M is nested inside an argument without being the argument: the argument
    // expression's own kind covers it.
  }

  void classifyCtorInitUse(const Expr *M, const CXXConstructorDecl *CD) {
    SourceLocation Loc = M->getBeginLoc();
    for (const CXXCtorInitializer *CI : CD->inits()) {
      if (CI->getInit() != M)
        continue;
      if (CI->isDelegatingInitializer())
        return; // the CXXConstructExpr's arguments are classified as a call
      if (DefIsTrackedViewCtor)
        return flowsToSelfThis(Loc); // member or base subobject of the view
      return sink(EscapeReason::StoreToField, Loc);
    }
    sink(EscapeReason::UnrecognizedUse, Loc, "constructor initializer");
  }

  void classifyUse(const Expr *M, AliasKind K, const DynTypedNode &P) {
    SourceLocation Loc = M->getBeginLoc();

    if (const auto *RS = P.get<ReturnStmt>())
      return returnsSelf(RS->getBeginLoc());
    if (const auto *CD = P.get<CXXConstructorDecl>())
      return classifyCtorInitUse(M, CD);
    if (const auto *VD = P.get<VarDecl>()) {
      if (isLocalReference(VD))
        return K == AliasKind::VarLValue ? sink(EscapeReason::AddressTaken, Loc) : void();
      if (isLocalPointerStorage(VD))
        return; // growth
      if (VD->hasAttr<BlocksAttr>())
        return sink(EscapeReason::Capture, Loc);
      if (VD->hasAttr<CleanupAttr>())
        return sink(EscapeReason::AddressTaken, Loc);
      if (VD->hasGlobalStorage())
        return sink(EscapeReason::StoreToGlobal, Loc, VD->getNameAsString());
      return sink(EscapeReason::UnrecognizedUse, Loc, "variable initializer");
    }
    if (P.get<GCCAsmStmt>() || P.get<MSAsmStmt>())
      return sink(EscapeReason::Asm, Loc);
    if (P.get<ObjCAtThrowStmt>())
      return sink(EscapeReason::Throw, Loc);
    if (P.get<Stmt>() && !P.get<Expr>())
      return; // expression statement, condition, loop clause: discarded/bool

    const Expr *PE = P.get<Expr>();
    if (!PE)
      return sink(EscapeReason::UnrecognizedUse, Loc, "no parent");

    if (isa<CXXThrowExpr>(PE))
      return sink(EscapeReason::Throw, Loc);
    if (isa<CXXNewExpr>(PE))
      return sink(EscapeReason::HeapAllocation, Loc);
    if (isa<CXXDeleteExpr>(PE))
      return sink(EscapeReason::Deallocation, Loc);
    if (isa<InitListExpr, DesignatedInitExpr, CXXStdInitializerListExpr, CXXParenListInitExpr>(PE))
      return sink(EscapeReason::StoreToField, Loc);
    if (isa<LambdaExpr>(PE))
      return sink(EscapeReason::Capture, Loc);
    if (isa<ObjCMessageExpr, PseudoObjectExpr, ObjCBoxedExpr, ObjCArrayLiteral, ObjCDictionaryLiteral>(PE))
      return sink(EscapeReason::ObjCMessage, Loc);
    if (isa<VAArgExpr>(PE))
      return sink(EscapeReason::VarArgs, Loc);
    if (isa<UnaryExprOrTypeTraitExpr, CXXTypeidExpr, CXXNoexceptExpr, SizeOfPackExpr>(PE))
      return; // unevaluated
    if (const auto *C = dyn_cast<CastExpr>(PE)) {
      switch (C->getCastKind()) {
      case CK_LValueToRValue: case CK_PointerToBoolean: case CK_ToVoid:
        return;
      case CK_PointerToIntegral:
        return sink(EscapeReason::CastToNonPointer, Loc);
      default:
        return sink(EscapeReason::CastToNonPointer, Loc, C->getCastKindName());
      }
    }
    if (const auto *ME = dyn_cast<MemberExpr>(PE)) {
      if (isa<CXXMethodDecl>(ME->getMemberDecl())) {
        DynTypedNode GP = parentOf(ME);
        if (const auto *MCE = GP.get<CXXMemberCallExpr>())
          return classifyCallUse(M, K, MCE);
        return sink(EscapeReason::CallableUse, Loc); // pointer-to-member formation
      }
      return; // non-pointer-carrying field read of an alias object
    }
    if (const auto *UO = dyn_cast<UnaryOperator>(PE)) {
      if (UO->getOpcode() == UO_AddrOf)
        return sink(EscapeReason::AddressTaken, Loc);
      return; // !p, -x, etc. are value reads
    }
    if (const auto *BO = dyn_cast<BinaryOperator>(PE)) {
      if (BO->getOpcode() == BO_Assign) {
        if (BO->getRHS() == M)
          return classifyStoreInto(BO->getLHS(), Loc);
        return; // writing into the alias's location
      }
      if (BO->isComparisonOp() || BO->isLogicalOp() || BO->getOpcode() == BO_Comma ||
          (BO->getOpcode() == BO_Sub && !isPointerCarryingType(BO->getType())) ||
          isa<CompoundAssignOperator>(BO))
        return;
      return sink(EscapeReason::UnrecognizedUse, Loc, BO->getOpcodeStr().str());
    }
    if (const auto *CO = dyn_cast<AbstractConditionalOperator>(PE)) {
      if (CO->getCond() == M)
        return;
      return sink(EscapeReason::UnrecognizedUse, Loc, "conditional");
    }
    if (isa<ArraySubscriptExpr>(PE))
      return; // index operand
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
    if (isa<StmtExpr>(PE))
      return; // covered by the statement expression's own classification
    return sink(EscapeReason::UnrecognizedUse, Loc, PE->getStmtClassName());
  }
};

} // namespace

FunctionEscapeFacts clang::ssaf::classifyFunctionEscapes(const FunctionDecl *Def,
                                                         ASTContext &Ctx,
                                                         TUSummaryExtractor &Extractor) {
  FunctionEscapeFacts Facts;
  const SourceManager &SM = Ctx.getSourceManager();
  const auto *MD = dyn_cast<CXXMethodDecl>(Def);
  bool HasThis = MD && MD->isInstance();

  if (isa_and_nonnull<CoroutineBodyStmt>(Def->getBody())) {
    Sink S{EscapeReason::Coroutine, recordFor(Def->getBody()->getBeginLoc(), SM), ""};
    for (const ParmVarDecl *P : Def->parameters())
      if (isPointerCarryingType(P->getType()))
        Facts.Params[P->getFunctionScopeIndex()].OtherSink = S;
    if (HasThis)
      Facts.This = EscapeFact{{}, std::nullopt, S};
    return Facts;
  }

  Classifier C(Def, Ctx, Extractor);
  for (const ParmVarDecl *P : Def->parameters())
    if (isPointerCarryingType(P->getType()))
      Facts.Params[P->getFunctionScopeIndex()] = C.analyze(Source{P});
  if (HasThis)
    Facts.This = C.analyze(Source{nullptr});
  return Facts;
}
```

- [ ] **Step 4: Run the tests**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='ParameterEscapeExtractor*'`
Expected: all PASS. If a row test fails, fix the classifier rule for that row — never weaken a test to a less specific sink, and never make a use benign without a soundness argument written into the spec.

- [ ] **Step 5: Commit**

```bash
git add clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.cpp clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractorTest.cpp
git commit -m "[SSAF][ParameterEscape] Implement alias derivation and use classification

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Tracked views, `this`, and constructor initializers

**Files:**
- Modify (only if a test fails): `EscapeClassifier.cpp`
- Test: append to `ParameterEscapeExtractorTest.cpp`

- [ ] **Step 1: Write the tests**

```c++
//===--- Tracked views -----------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ViewConstructionFlowsIntoTheViewObject) {
  ASSERT_TRUE(setUp("void f(int *p) { View v(p, 1); (void)v.size(); }"));
  EXPECT_TRUE(flowsTo("f", 0, "View", 0));
  EXPECT_FALSE(sinkOf("f", 0).has_value());
  const ParameterEscapeSummary *Ctor = ctorSummaryOf("View", 2);
  ASSERT_NE(Ctor, nullptr);
  ASSERT_EQ(Ctor->Params.count(0), 1u);
  EXPECT_FALSE(Ctor->Params.at(0).OtherSink.has_value());
  EXPECT_EQ(Ctor->Params.at(0).FlowsTo.size(), 1u) << "p flows to View::View's this";
  ASSERT_TRUE(Ctor->This.has_value());
  EXPECT_FALSE(Ctor->This->OtherSink.has_value());
}

TEST_F(ParameterEscapeExtractorTest, ViewAccessorResultIsAnAlias) {
  ASSERT_TRUE(setUp("void f(int *p) { View v(p, 1); g_ptr = v.data(); }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, ViewFieldOfAliasIsAnAlias) {
  ASSERT_TRUE(setUp("void f(View v) { g_ptr = v.d; } void g(const View &v) { g_ptr = v.d; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, ViewMemberThisFieldLoadsAreAliases) {
  ASSERT_TRUE(setUp("void View::leak() { g_ptr = d; }"));
  const EscapeFact *T = thisFactOf("leak");
  ASSERT_NE(T, nullptr);
  ASSERT_TRUE(T->OtherSink.has_value());
  EXPECT_EQ(T->OtherSink->Reason, EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, ViewAccessorsReturnThis) {
  ASSERT_TRUE(setUp(""));
  ASSERT_NE(thisFactOf("data"), nullptr);
  EXPECT_TRUE(thisFactOf("data")->returnsSelf());
  ASSERT_NE(thisFactOf("size"), nullptr);
  EXPECT_TRUE(!thisFactOf("size")->returnsSelf() && !thisFactOf("size")->OtherSink);
  ASSERT_NE(thisFactOf("sub"), nullptr);
  EXPECT_TRUE(thisFactOf("sub")->returnsSelf());
}

TEST_F(ParameterEscapeExtractorTest, ViewCopiesAndReturnsAreTracked) {
  ASSERT_TRUE(setUp("int *p0(); View f(View v) { View w = v; View x(p0(), 0); x = w; return x; }"));
  EXPECT_TRUE(factOf("f", 0)->returnsSelf());
  EXPECT_FALSE(sinkOf("f", 0).has_value());
}

TEST_F(ParameterEscapeExtractorTest, ViewStoredIntoNonLocalIsASink) {
  ASSERT_TRUE(setUp("View g_view(nullptr, 0); void f(View v) { g_view = v; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, NonTrivialViewConstructionIsASink) {
  ASSERT_TRUE(setUp("void f(int *p) { NonTrivialView v(p); (void)v; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::NonTrivialView);
}

TEST_F(ParameterEscapeExtractorTest, SwiftNonEscapableCountsAsView) {
  ASSERT_TRUE(setUp("void f(SwiftView v) { g_ptr = v.d; } void g(SwiftView v) { (void)v.d[0]; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("g", 0));
}

TEST_F(ParameterEscapeExtractorTest, NonViewConstructorStoringParameterIsASink) {
  ASSERT_TRUE(setUp("void f(int *p) { Holder h(p); (void)h; }"));
  EXPECT_TRUE(flowsTo("f", 0, "Holder", 0));
  const ParameterEscapeSummary *Ctor = ctorSummaryOf("Holder", 1);
  ASSERT_NE(Ctor, nullptr);
  ASSERT_TRUE(Ctor->Params.at(0).OtherSink.has_value());
  EXPECT_EQ(Ctor->Params.at(0).OtherSink->Reason, EscapeReason::StoreToField);
}

TEST_F(ParameterEscapeExtractorTest, ConstructorInitializerFlowsAreVisited) {
  ASSERT_TRUE(setUp("struct H2 { View v; H2(int *p) : v(p, 1) {} };"));
  const ParameterEscapeSummary *Ctor = ctorSummaryOf("H2", 1);
  ASSERT_NE(Ctor, nullptr);
  ASSERT_TRUE(Ctor->Params.at(0).OtherSink.has_value());
  EXPECT_EQ(Ctor->Params.at(0).OtherSink->Reason, EscapeReason::StoreToField);
}

TEST_F(ParameterEscapeExtractorTest, BaseInitializerIsACall) {
  ASSERT_TRUE(setUp("struct D : Holder { D(int *p) : Holder(p) {} };"));
  const ParameterEscapeSummary *Ctor = ctorSummaryOf("D", 1);
  ASSERT_NE(Ctor, nullptr);
  EXPECT_TRUE(Ctor->Params.at(0).FlowsTo.size() == 1u) << "p flows to Holder::Holder(0)";
  EXPECT_FALSE(Ctor->Params.at(0).OtherSink.has_value());
}

TEST_F(ParameterEscapeExtractorTest, ThisAsStoreBaseIsBenignForThis) {
  ASSERT_TRUE(setUp("struct T { int *m; int n; void set(int *p) { m = p; n = 1; } };"));
  const EscapeFact *T = thisFactOf("set");
  ASSERT_NE(T, nullptr);
  EXPECT_FALSE(T->OtherSink.has_value());
  EXPECT_EQ(sinkOf("set", 0), EscapeReason::StoreToField);
}
```

- [ ] **Step 2: Run the tests**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='ParameterEscapeExtractor*'`
Expected: PASS. If `ConstructorInitializerFlowsAreVisited` fails, confirm that `parentOf(<init expr>)` yields the `CXXConstructorDecl` (it does not yield a `CXXCtorInitializer`; `ParentMapContext` never records those) and that `classifyCtorInitUse` finds the initializer by pointer identity with `CI->getInit()`.

- [ ] **Step 3: Commit**

```bash
git add clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.cpp clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractorTest.cpp
git commit -m "[SSAF][ParameterEscape] Cover tracked views, this, and constructor initializers

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Objective-C++ constructs, blocks, glvalue edge cases

**Files:**
- Modify (only if a test fails): `EscapeClassifier.cpp`
- Test: append to `ParameterEscapeExtractorTest.cpp`

- [ ] **Step 1: Write the tests**

```c++
//===--- ObjC++ and blocks -------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ObjCConstructsAreSinks) {
  ASSERT_TRUE(setUp(R"objc(
    @interface Obj
    - (void)take:(int *)p;
    @property int *prop;
    @end
    void msg(Obj *o, int *p) { [o take:p]; }
    void prop(Obj *o, int *p) { o.prop = p; }
    void blk(int *p) { void (^b)(void) = ^{ (void)*p; }; (void)b; }
    void blkvar(int *p) { __block int *q = p; (void)q; }
    void othrow(int *p) { @throw (id)p; }
  )objc", {"-x", "objective-c++", "-fblocks", "-fobjc-arc"}));
  EXPECT_EQ(sinkOf("msg", 1), EscapeReason::ObjCMessage);
  EXPECT_TRUE(sinkOf("prop", 1).has_value()) << "property store must sink (ObjCMessage or StoreThroughPointer)";
  EXPECT_EQ(sinkOf("blk", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("blkvar", 0), EscapeReason::Capture);
  EXPECT_TRUE(sinkOf("othrow", 0).has_value());
}

//===--- glvalue edge cases ------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ReferenceBoundToPointeeGrows) {
  ASSERT_TRUE(setUp("void f(int *p) { int &r = *p; g_ptr = &r; } void g(int *p) { int &r = *p; r = 1; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("g", 0));
}

TEST_F(ParameterEscapeExtractorTest, ReferenceParameterSubobjectAddress) {
  ASSERT_TRUE(setUp("struct S { int m; }; void f(S &s) { g_ptr = &s.m; } void g(S &s) { int x = s.m; (void)x; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("g", 0));
}

TEST_F(ParameterEscapeExtractorTest, ReferenceToPointerCallResultKeepsAlias) {
  ASSERT_TRUE(setUp("int *&slot(int *); void f(int *p) { g_ptr = slot(p); } void g(int *p) { slot(p) = p; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::StoreThroughPointer);
}

TEST_F(ParameterEscapeExtractorTest, ConditionalAndCommaPropagate) {
  ASSERT_TRUE(setUp("void f(int *p, int *q, bool c) { g_ptr = c ? p : q; } void g(int *p) { g_ptr = (0, p); }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("f", 1), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, RangeForOverPointeeIsBenign) {
  ASSERT_TRUE(setUp("struct R { int *begin(); int *end(); }; void f(R *r) { for (int x : *r) (void)x; }"));
  EXPECT_TRUE(flowsTo("f", 0, "begin", ThisParamIndex));
  EXPECT_FALSE(sinkOf("f", 0).has_value());
}

TEST_F(ParameterEscapeExtractorTest, StdMoveLikeReferenceCastKeepsAlias) {
  ASSERT_TRUE(setUp("template <class T> T &&mv(T &t) { return static_cast<T &&>(t); } void f(int *p) { g_ptr = mv(p); }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, PassingAliasToUnknownCalleeRecordsFlowOnly) {
  ASSERT_TRUE(setUp("void f(int *p) { unknown(p); }"));
  EXPECT_TRUE(flowsTo("f", 0, "unknown", 0));
  EXPECT_FALSE(sinkOf("f", 0).has_value());
}

TEST_F(ParameterEscapeExtractorTest, DefaultArgumentsAndNestedLambdasAreSeen) {
  ASSERT_TRUE(setUp("void take(int *a, int *b = g_ptr); void f(int *p) { take(p); } void g(int *p) { auto o = [] { return [p = g_ptr] { return p; }; }; (void)o; auto i = [p] { auto j = [p] { return p; }; return j; }; (void)i; }"));
  EXPECT_TRUE(flowsTo("f", 0, "take", 0));
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::Capture);
}
```

- [ ] **Step 2: Run the tests**

Run: same command as Task 7.
Expected: PASS. For `ReferenceToPointerCallResultKeepsAlias::g`, `slot(p) = p`: the assignment's kind is `kind(LHS) == VarLValue`; the RHS `p` is not covered (assignment RHS is never covered) and `classifyStoreInto(slot(p))` falls to `StoreThroughPointer`.

- [ ] **Step 3: Commit**

```bash
git add clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.cpp clang/unittests/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeExtractorTest.cpp
git commit -m "[SSAF][ParameterEscape] Cover ObjC++ constructs, blocks, and glvalue edge cases

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Whole-program analyses and result serialization

**Files:**
- Create: `clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h`
- Create: `clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.cpp`
- Modify: Analyses `CMakeLists.txt`, `BuiltinAnchorSources.def` (`ANCHOR(ParameterEscapeAnalysisAnchorSource)`)
- Test: `clang/unittests/ScalableStaticAnalysis/WholeProgramAnalysis/NonEscapingParametersAnalysisTest.cpp`
- Modify: unittest `CMakeLists.txt`

**Interfaces:**
- Consumes: `ParameterEscapeSummary`, `EscapeFact`, `FlowTarget`, `ThisParamIndex` (Task 3); JSON helpers from `ParameterEscapeJSON.h` (Task 3).
- Produces (`namespace clang::ssaf`):
  ```c++
  struct Node { EntityId Function; int ParamIndex; };  // operator<, ==
  struct ParameterEscapeResult final : AnalysisResult {   // analysisName() == "ParameterEscapeResult"
    std::map<Node, EscapeFact> Facts; std::set<Node> Candidates; };
  struct NonEscapingParametersResult final : AnalysisResult { // "NonEscapingParametersResult"
    std::map<EntityId, std::set<unsigned>> NonEscaping;
    struct Rejection { EscapeReason Reason; SourceLocationRecord Location; std::string Detail; std::optional<Node> Blame; };
    std::map<Node, Rejection> Rejected; };
  ```
  Analyses registered as `ParameterEscapeResult` and `NonEscapingParametersResult` (the `-a` names for `clang-ssaf-analyzer`).

- [ ] **Step 1: Write the failing tests**

`clang/unittests/ScalableStaticAnalysis/WholeProgramAnalysis/NonEscapingParametersAnalysisTest.cpp`:

```c++
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"
#include "../TestFixture.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/EntityLinker/LUSummary.h"
#include "clang/ScalableStaticAnalysis/Core/Model/BuildNamespace.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisDriver.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ssaf;

namespace {

const SourceLocationRecord Site{"a.cpp", 7, 3};

class NonEscapingParametersAnalysisTest : public TestFixture {
protected:
  NestedBuildNamespace NS{{BuildNamespace(BuildNamespaceKind::LinkUnit, "LU")}};
  std::unique_ptr<LUSummary> LU =
      std::make_unique<LUSummary>(llvm::Triple("arm64-apple-macosx"), NS);
  std::map<std::string, EntityId> Ids;

  EntityId fn(StringRef Name) {
    auto It = Ids.find(Name.str());
    if (It != Ids.end())
      return It->second;
    EntityId Id = getIdTable(*LU).getId(EntityName(("c:@F@" + Name).str(), "", NS));
    getLinkageTable(*LU).insert({Id, EntityLinkage(EntityLinkageType::External)});
    Ids[Name.str()] = Id;
    return Id;
  }

  ParameterEscapeSummary &summary(StringRef Name, bool Candidate = true) {
    auto &Slot = getData(*LU)[ParameterEscapeSummary::summaryName()][fn(Name)];
    if (!Slot) {
      auto S = std::make_unique<ParameterEscapeSummary>();
      S->IsCandidate = Candidate;
      Slot = std::move(S);
    }
    return static_cast<ParameterEscapeSummary &>(*Slot);
  }

  EscapeFact &param(StringRef Fn, unsigned I, bool CandidateType = true) {
    ParameterEscapeSummary &S = summary(Fn);
    if (CandidateType)
      S.CandidateParams.insert(I);
    return S.Params[I];
  }

  static EscapeFact flows(std::initializer_list<FlowTarget> Ts) {
    EscapeFact F;
    for (const FlowTarget &T : Ts)
      F.FlowsTo[T] = Site;
    return F;
  }
  static EscapeFact sinks(EscapeReason R) {
    EscapeFact F;
    F.OtherSink = Sink{R, SourceLocationRecord{"a.cpp", 1, 1}, ""};
    return F;
  }

  NonEscapingParametersResult run() {
    AnalysisDriver Driver(std::move(LU));
    llvm::Expected<WPASuite> Suite = Driver.run<NonEscapingParametersResult>();
    EXPECT_THAT_EXPECTED(Suite, llvm::Succeeded());
    auto R = Suite->get<NonEscapingParametersResult>();
    EXPECT_THAT_EXPECTED(R, llvm::Succeeded());
    return *R;
  }

  static bool annotated(const NonEscapingParametersResult &R, EntityId F, unsigned I) {
    auto It = R.NonEscaping.find(F);
    return It != R.NonEscaping.end() && It->second.count(I);
  }
};

TEST_F(NonEscapingParametersAnalysisTest, CleanParameterIsAnnotated) {
  param("f", 0) = EscapeFact();
  auto R = run();
  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_TRUE(R.Rejected.empty());
}

TEST_F(NonEscapingParametersAnalysisTest, SinkRejectsWithReason) {
  param("f", 0) = sinks(EscapeReason::StoreToGlobal);
  auto R = run();
  EXPECT_FALSE(annotated(R, fn("f"), 0));
  ASSERT_EQ(R.Rejected.count(Node{fn("f"), 0}), 1u);
  EXPECT_EQ(R.Rejected.at(Node{fn("f"), 0}).Reason, EscapeReason::StoreToGlobal);
}

TEST_F(NonEscapingParametersAnalysisTest, ChainPropagatesWithBlameAndCallSite) {
  param("f", 0) = flows({FlowTarget{fn("g"), 0}});
  param("g", 0) = flows({FlowTarget{fn("h"), 0}});
  param("h", 0) = sinks(EscapeReason::StoreToField);
  auto R = run();
  EXPECT_FALSE(annotated(R, fn("f"), 0));
  const auto &Rej = R.Rejected.at(Node{fn("f"), 0});
  EXPECT_EQ(Rej.Reason, EscapeReason::EscapesViaCallee);
  ASSERT_TRUE(Rej.Blame.has_value());
  EXPECT_EQ(*Rej.Blame, (Node{fn("g"), 0}));
  EXPECT_EQ(Rej.Location, Site) << "the call site of the blamed flow";
}

TEST_F(NonEscapingParametersAnalysisTest, CycleWithoutSinkIsNonEscaping) {
  param("f", 0) = flows({FlowTarget{fn("g"), 0}});
  param("g", 0) = flows({FlowTarget{fn("f"), 0}});
  auto R = run();
  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_TRUE(annotated(R, fn("g"), 0));
}

TEST_F(NonEscapingParametersAnalysisTest, CycleWithSinkEscapes) {
  param("f", 0) = flows({FlowTarget{fn("g"), 0}});
  param("g", 0) = flows({FlowTarget{fn("f"), 0}});
  param("g", 0).OtherSink = Sink{EscapeReason::Throw, SourceLocationRecord{"a.cpp", 2, 2}, ""};
  auto R = run();
  EXPECT_FALSE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("g"), 0));
}

TEST_F(NonEscapingParametersAnalysisTest, MissingNodeEscapes) {
  param("f", 0) = flows({FlowTarget{fn("external"), 0}});
  auto R = run();
  EXPECT_FALSE(annotated(R, fn("f"), 0));
  EXPECT_EQ(R.Rejected.at(Node{fn("f"), 0}).Reason, EscapeReason::UnanalyzedCallee);
  EXPECT_EQ(R.Rejected.at(Node{fn("f"), 0}).Location, Site);
}

TEST_F(NonEscapingParametersAnalysisTest, ReturnRejectsSelfButNotCallers) {
  param("f", 0) = flows({FlowTarget{fn("id"), 0}});
  param("id", 0).ReturnsSelfAt = SourceLocationRecord{"a.cpp", 3, 3};
  auto R = run();
  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("id"), 0));
  EXPECT_EQ(R.Rejected.at(Node{fn("id"), 0}).Reason, EscapeReason::Return);
}

TEST_F(NonEscapingParametersAnalysisTest, ThisNodesParticipateButAreNeverAnnotated) {
  param("f", 0) = flows({FlowTarget{fn("m"), ThisParamIndex}});
  summary("m").This = sinks(EscapeReason::StoreToGlobal);
  auto R = run();
  EXPECT_FALSE(annotated(R, fn("f"), 0));
  EXPECT_EQ(R.Rejected.count(Node{fn("m"), ThisParamIndex}), 0u);
}

TEST_F(NonEscapingParametersAnalysisTest, OnlyCandidatesAreReported) {
  param("f", 0) = EscapeFact();
  summary("f").IsCandidate = false;
  param("g", 0, /*CandidateType=*/false) = EscapeFact();
  auto R = run();
  EXPECT_TRUE(R.NonEscaping.empty());
  EXPECT_TRUE(R.Rejected.empty());
}

TEST_F(NonEscapingParametersAnalysisTest, DeterministicAcrossRuns) {
  param("a", 0) = flows({FlowTarget{fn("b"), 0}, FlowTarget{fn("c"), 0}});
  param("b", 0) = sinks(EscapeReason::Throw);
  param("c", 0) = sinks(EscapeReason::Asm);
  auto R1 = run();
  // Rebuild the same LU and run again; blame must be the smallest node.
  LU = std::make_unique<LUSummary>(llvm::Triple("arm64-apple-macosx"), NS);
  Ids.clear();
  param("a", 0) = flows({FlowTarget{fn("c"), 0}, FlowTarget{fn("b"), 0}});
  param("b", 0) = sinks(EscapeReason::Throw);
  param("c", 0) = sinks(EscapeReason::Asm);
  auto R2 = run();
  EXPECT_EQ(R1.Rejected.at(Node{fn("a"), 0}).Blame->ParamIndex,
            R2.Rejected.at(Node{fn("a"), 0}).Blame->ParamIndex);
}

} // namespace
```

Add `WholeProgramAnalysis/NonEscapingParametersAnalysisTest.cpp` to the unittest CMake list.

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build ClangScalableAnalysisTests 2>&1 | tail -3`
Expected: header not found.

- [ ] **Step 3: Implement**

`clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h`:

```c++
//===- ParameterEscapeAnalysis.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Whole-program results for noescape inference:
//  - ParameterEscapeResult aggregates per-parameter escape facts by node.
//  - NonEscapingParametersResult is the fixpoint's verdict: parameters to
//    annotate, and rejected candidates with a reason and one hop of blame.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEANALYSIS_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEANALYSIS_H

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisName.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisResult.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace clang::ssaf {

constexpr llvm::StringLiteral ParameterEscapeResultName = "ParameterEscapeResult";
constexpr llvm::StringLiteral NonEscapingParametersResultName = "NonEscapingParametersResult";

struct Node {
  EntityId Function;
  int ParamIndex;
  bool operator<(const Node &O) const {
    return std::tie(Function, ParamIndex) < std::tie(O.Function, O.ParamIndex);
  }
  bool operator==(const Node &O) const {
    return std::tie(Function, ParamIndex) == std::tie(O.Function, O.ParamIndex);
  }
};

struct ParameterEscapeResult final : AnalysisResult {
  static AnalysisName analysisName() { return AnalysisName(ParameterEscapeResultName.str()); }
  std::map<Node, EscapeFact> Facts;
  std::set<Node> Candidates;
};

struct NonEscapingParametersResult final : AnalysisResult {
  static AnalysisName analysisName() {
    return AnalysisName(NonEscapingParametersResultName.str());
  }
  struct Rejection {
    EscapeReason Reason;
    SourceLocationRecord Location;
    std::string Detail;
    std::optional<Node> Blame;
  };
  std::map<EntityId, std::set<unsigned>> NonEscaping;
  std::map<Node, Rejection> Rejected;
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEANALYSIS_H
```

`clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.cpp`:

```c++
//===- ParameterEscapeAnalysis.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"
#include "ParameterEscapeJSON.h"
#include "SSAFAnalysesCommon.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisRegistry.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/DerivedAnalysis.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/SummaryAnalysis.h"
#include "llvm/Support/JSON.h"
#include <vector>

using namespace clang::ssaf;
using namespace llvm;

namespace {

//===--- JSON --------------------------------------------------------------===//

json::Object nodeToJSON(const Node &N, JSONFormat::EntityIdToJSONFn IdToJSON) {
  return json::Object{{"function", IdToJSON(N.Function)},
                      {"param", static_cast<int64_t>(N.ParamIndex)}};
}

Expected<Node> nodeFromJSON(const json::Object &O, JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  const json::Object *F = O.getObject("function");
  auto P = O.getInteger("param");
  if (!F || !P)
    return makeSawButExpectedError(O, "a node with function and param");
  Expected<EntityId> Id = IdFromJSON(*F);
  if (!Id)
    return Id.takeError();
  return Node{*Id, static_cast<int>(*P)};
}

json::Object serializeParameterEscapeResult(const ParameterEscapeResult &R,
                                            JSONFormat::EntityIdToJSONFn IdToJSON) {
  json::Array Facts, Candidates;
  for (const auto &[N, F] : R.Facts) {
    json::Object E = escapeFactToJSON(F, IdToJSON);
    E["node"] = nodeToJSON(N, IdToJSON);
    Facts.push_back(std::move(E));
  }
  for (const Node &N : R.Candidates)
    Candidates.push_back(nodeToJSON(N, IdToJSON));
  return json::Object{{ParameterEscapeResultName,
                       json::Object{{"facts", std::move(Facts)},
                                    {"candidates", std::move(Candidates)}}}};
}

Expected<std::unique_ptr<AnalysisResult>>
deserializeParameterEscapeResult(const json::Object &Obj, JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  const json::Object *Content = Obj.getObject(ParameterEscapeResultName);
  const json::Array *Facts = Content ? Content->getArray("facts") : nullptr;
  const json::Array *Candidates = Content ? Content->getArray("candidates") : nullptr;
  if (!Facts || !Candidates)
    return makeSawButExpectedError(Obj, "an object with a key %s", ParameterEscapeResultName.data());
  auto R = std::make_unique<ParameterEscapeResult>();
  for (const auto &V : *Facts) {
    const json::Object *E = V.getAsObject();
    const json::Object *NodeObj = E ? E->getObject("node") : nullptr;
    if (!NodeObj)
      return makeSawButExpectedError(V, "a fact with a node");
    Expected<Node> N = nodeFromJSON(*NodeObj, IdFromJSON);
    if (!N)
      return N.takeError();
    Expected<EscapeFact> F = escapeFactFromJSON(*E, IdFromJSON);
    if (!F)
      return F.takeError();
    R->Facts[*N] = std::move(*F);
  }
  for (const auto &V : *Candidates) {
    const json::Object *NodeObj = V.getAsObject();
    if (!NodeObj)
      return makeSawButExpectedError(V, "a node");
    Expected<Node> N = nodeFromJSON(*NodeObj, IdFromJSON);
    if (!N)
      return N.takeError();
    R->Candidates.insert(*N);
  }
  return std::move(R);
}

JSONFormat::AnalysisResultRegistry::Add<ParameterEscapeResult>
    RegisterParameterEscapeResultForJSON(serializeParameterEscapeResult,
                                         deserializeParameterEscapeResult);

json::Object serializeNonEscapingParametersResult(const NonEscapingParametersResult &R,
                                                  JSONFormat::EntityIdToJSONFn IdToJSON) {
  json::Array NonEscaping, Rejected;
  for (const auto &[F, Params] : R.NonEscaping) {
    json::Array Idx;
    for (unsigned I : Params)
      Idx.push_back(static_cast<int64_t>(I));
    NonEscaping.push_back(json::Object{{"function", IdToJSON(F)}, {"params", std::move(Idx)}});
  }
  for (const auto &[N, Rej] : R.Rejected) {
    json::Object O{{"node", nodeToJSON(N, IdToJSON)},
                   {"reason", escapeReasonName(Rej.Reason).str()},
                   {"location", sourceLocationRecordToJSON(Rej.Location)},
                   {"detail", Rej.Detail}};
    if (Rej.Blame)
      O["blame"] = nodeToJSON(*Rej.Blame, IdToJSON);
    Rejected.push_back(std::move(O));
  }
  return json::Object{{NonEscapingParametersResultName,
                       json::Object{{"non_escaping", std::move(NonEscaping)},
                                    {"rejected", std::move(Rejected)}}}};
}

Expected<std::unique_ptr<AnalysisResult>>
deserializeNonEscapingParametersResult(const json::Object &Obj,
                                       JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  const json::Object *Content = Obj.getObject(NonEscapingParametersResultName);
  const json::Array *NonEscaping = Content ? Content->getArray("non_escaping") : nullptr;
  const json::Array *Rejected = Content ? Content->getArray("rejected") : nullptr;
  if (!NonEscaping || !Rejected)
    return makeSawButExpectedError(Obj, "an object with a key %s",
                                   NonEscapingParametersResultName.data());
  auto R = std::make_unique<NonEscapingParametersResult>();
  for (const auto &V : *NonEscaping) {
    const json::Object *E = V.getAsObject();
    const json::Object *F = E ? E->getObject("function") : nullptr;
    const json::Array *Params = E ? E->getArray("params") : nullptr;
    if (!F || !Params)
      return makeSawButExpectedError(V, "a function with params");
    Expected<EntityId> Id = IdFromJSON(*F);
    if (!Id)
      return Id.takeError();
    for (const auto &P : *Params) {
      auto I = P.getAsInteger();
      if (!I)
        return makeSawButExpectedError(P, "a parameter index");
      R->NonEscaping[*Id].insert(static_cast<unsigned>(*I));
    }
  }
  for (const auto &V : *Rejected) {
    const json::Object *E = V.getAsObject();
    const json::Object *NodeObj = E ? E->getObject("node") : nullptr;
    auto Reason = E ? E->getString("reason") : std::nullopt;
    const json::Object *Loc = E ? E->getObject("location") : nullptr;
    if (!NodeObj || !Reason || !Loc)
      return makeSawButExpectedError(V, "a rejection with node, reason, location");
    Expected<Node> N = nodeFromJSON(*NodeObj, IdFromJSON);
    if (!N)
      return N.takeError();
    auto Rsn = parseEscapeReason(*Reason);
    if (!Rsn)
      return makeSawButExpectedError(V, "a known escape reason");
    Expected<SourceLocationRecord> L = sourceLocationRecordFromJSON(*Loc);
    if (!L)
      return L.takeError();
    NonEscapingParametersResult::Rejection Rej{*Rsn, *L, E->getString("detail").value_or("").str(), std::nullopt};
    if (const json::Object *B = E->getObject("blame")) {
      Expected<Node> BN = nodeFromJSON(*B, IdFromJSON);
      if (!BN)
        return BN.takeError();
      Rej.Blame = *BN;
    }
    R->Rejected[*N] = std::move(Rej);
  }
  return std::move(R);
}

JSONFormat::AnalysisResultRegistry::Add<NonEscapingParametersResult>
    RegisterNonEscapingParametersResultForJSON(serializeNonEscapingParametersResult,
                                               deserializeNonEscapingParametersResult);

//===--- Summary aggregation -----------------------------------------------===//

class ParameterEscapeAnalysis final
    : public SummaryAnalysis<ParameterEscapeResult, ParameterEscapeSummary> {
public:
  llvm::Error add(EntityId Id, const ParameterEscapeSummary &S) override {
    for (const auto &[Index, Fact] : S.Params) {
      Node N{Id, static_cast<int>(Index)};
      getResult().Facts[N] = Fact;
      if (S.IsCandidate && S.CandidateParams.count(Index))
        getResult().Candidates.insert(N);
    }
    if (S.This)
      getResult().Facts[Node{Id, ThisParamIndex}] = *S.This;
    return llvm::Error::success();
  }
};

AnalysisRegistry::Add<ParameterEscapeAnalysis>
    RegisterParameterEscapeAnalysis("Aggregates per-parameter escape facts by node");

//===--- Fixpoint ----------------------------------------------------------===//

class NonEscapingParametersAnalysis final
    : public DerivedAnalysis<NonEscapingParametersResult, ParameterEscapeResult> {
  const ParameterEscapeResult *In = nullptr;
  std::set<Node> EscapesForCaller;
  std::map<Node, std::vector<Node>> Reverse; // target → sources that flow into it

  void markEscapes(Node N, std::vector<Node> &Worklist) {
    if (EscapesForCaller.insert(N).second)
      Worklist.push_back(N);
  }

public:
  llvm::Error initialize(const ParameterEscapeResult &R) override {
    In = &R;
    std::vector<Node> Worklist;
    for (const auto &[N, F] : R.Facts) {
      if (F.OtherSink)
        markEscapes(N, Worklist);
      for (const auto &[T, Site] : F.FlowsTo) {
        Node Q{T.Callee, T.ParamIndex};
        Reverse[Q].push_back(N);
        if (!R.Facts.count(Q))
          markEscapes(Q, Worklist); // unanalyzed ⇒ escapes
      }
    }
    while (!Worklist.empty()) {
      Node Q = Worklist.back();
      Worklist.pop_back();
      auto It = Reverse.find(Q);
      if (It == Reverse.end())
        continue;
      for (const Node &P : It->second)
        markEscapes(P, Worklist);
    }
    return llvm::Error::success();
  }

  llvm::Expected<bool> step() override { return false; }

  llvm::Error finalize() override {
    for (const Node &X : In->Candidates) {
      const EscapeFact &F = In->Facts.at(X);
      NonEscapingParametersResult::Rejection Rej;
      if (F.OtherSink) {
        Rej = {F.OtherSink->Reason, F.OtherSink->Location, F.OtherSink->Detail, std::nullopt};
      } else if (F.returnsSelf()) {
        Rej = {EscapeReason::Return, *F.ReturnsSelfAt, "", std::nullopt};
      } else {
        // FlowsTo is an ordered map, so the first escaping target is the
        // smallest node: deterministic blame.
        const std::pair<const FlowTarget, SourceLocationRecord> *Culprit = nullptr;
        for (const auto &Entry : F.FlowsTo)
          if (EscapesForCaller.count(Node{Entry.first.Callee, Entry.first.ParamIndex})) {
            Culprit = &Entry;
            break;
          }
        if (!Culprit) {
          getResult().NonEscaping[X.Function].insert(static_cast<unsigned>(X.ParamIndex));
          continue;
        }
        Node Q{Culprit->first.Callee, Culprit->first.ParamIndex};
        bool Unanalyzed = !In->Facts.count(Q);
        Rej = {Unanalyzed ? EscapeReason::UnanalyzedCallee : EscapeReason::EscapesViaCallee,
               Culprit->second, "", Q};
      }
      getResult().Rejected[X] = std::move(Rej);
    }
    return llvm::Error::success();
  }
};

AnalysisRegistry::Add<NonEscapingParametersAnalysis>
    RegisterNonEscapingParametersAnalysis(
        "Whole-program escape fixpoint: parameters that provably do not escape");

} // namespace

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int ParameterEscapeAnalysisAnchorSource = 0;
} // namespace clang::ssaf
```

Wiring: add `ParameterEscape/ParameterEscapeAnalysis.cpp` to the Analyses CMake list; add `ANCHOR(ParameterEscapeAnalysisAnchorSource)` to `BuiltinAnchorSources.def` (alphabetical: before `ParameterEscapeExtractorAnchorSource`).

- [ ] **Step 4: Run the tests**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='NonEscapingParametersAnalysis*'`
Expected: 10 PASS.

- [ ] **Step 5: Commit**

```bash
git add clang/include/clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h clang/lib/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.cpp clang/lib/ScalableStaticAnalysis/Analyses/CMakeLists.txt clang/include/clang/ScalableStaticAnalysis/BuiltinAnchorSources.def clang/unittests/ScalableStaticAnalysis/WholeProgramAnalysis/NonEscapingParametersAnalysisTest.cpp clang/unittests/ScalableStaticAnalysis/CMakeLists.txt
git commit -m "[SSAF][ParameterEscape] Add whole-program escape fixpoint and result serialization

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: `--ssaf-noescape-spelling` and the `infer-noescape` transformation

**Files:**
- Modify: `clang/include/clang/Frontend/SSAFOptions.h` (add `std::string NoescapeSpelling;`)
- Modify: `clang/include/clang/Options/Options.td` (new option after `_ssaf_transformation_report_file`)
- Modify: `clang/lib/Driver/ToolChains/Clang.cpp` (forward the option next to the other `OPT__ssaf_*` `AddLastArg` lines)
- Create: `clang/include/clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h`
- Create: `clang/lib/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.cpp`
- Modify: `clang/lib/ScalableStaticAnalysis/SourceTransformation/CMakeLists.txt`, `BuiltinAnchorSources.def` (`ANCHOR(InferNoescapeAnchorSource)`)
- Test: `clang/unittests/ScalableStaticAnalysis/SourceTransformation/InferNoescapeTest.cpp`
- Modify: unittest `CMakeLists.txt`

**Interfaces:**
- Consumes: `NonEscapingParametersResult`, `Node`, `EscapeReason` (Task 9), `Transformation`, `SourceEditEmitter`, `TransformationReportEmitter`, `getQualifiedEntityName`, `clang::index::generateUSRForDecl` from `clang/UnifiedSymbolResolution/USRGeneration.h` (library `clangUnifiedSymbolResolution`, already a transitive dependency through `clangScalableStaticAnalysisCore`).
- Produces: transformation registered as `infer-noescape`; SARIF rule ids `noescape-inserted`, `noescape-skipped`, `noescape-rejected`; `SSAFOptions::NoescapeSpelling`.

- [ ] **Step 1: Write the failing tests**

`clang/unittests/ScalableStaticAnalysis/SourceTransformation/InferNoescapeTest.cpp`:

```c++
#include "clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h"
#include "FindDecl.h"
#include "TestFixture.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/Sarif.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/PCHContainerOperations.h"
#include "clang/Frontend/SSAFOptions.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"
#include "clang/ScalableStaticAnalysis/Core/ASTEntityMapping.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/WPASuite.h"
#include "clang/ScalableStaticAnalysis/SourceTransformation/SourceEditEmitter.h"
#include "clang/ScalableStaticAnalysis/SourceTransformation/TransformationReportEmitter.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "gtest/gtest.h"

using namespace clang;
using namespace clang::ssaf;

namespace {

class RecordingEditEmitter : public SourceEditEmitter {
public:
  std::vector<tooling::Replacement> Replacements;
  void addReplacement(tooling::Replacement R) override { Replacements.push_back(std::move(R)); }
};

class RecordingReportEmitter : public TransformationReportEmitter {
public:
  struct Entry { std::string RuleId; SarifResultLevel Level; std::string Message; };
  std::vector<Entry> Results;
  void addResult(StringRef RuleId, SarifResultLevel Level, CharSourceRange, StringRef Message) override {
    Results.push_back({RuleId.str(), Level, Message.str()});
  }
};

struct Captured {
  std::string Rewritten;
  std::vector<RecordingReportEmitter::Entry> Reports;
  size_t count(StringRef Rule) const {
    return llvm::count_if(Reports, [&](const auto &E) { return E.RuleId == Rule; });
  }
};

class InferNoescapeTest : public TestFixture {
protected:
  NestedBuildNamespace TUNs = NestedBuildNamespace::makeCompilationUnit("cu");
  NestedBuildNamespace LUNs = NestedBuildNamespace::makeLinkUnit("lu");

  Captured run(StringRef Code, StringRef Fn, std::vector<unsigned> Indices,
               std::vector<std::string> Args = {"-std=c++20"}, StringRef Spelling = "",
               tooling::FileContentMappings Files = {}) {
    std::unique_ptr<ASTUnit> AST = tooling::buildASTFromCodeWithArgs(
        Code, Args, "input.cc", "clang-tool", std::make_shared<PCHContainerOperations>(),
        tooling::getClangStripDependencyFileAdjuster(), Files);
    ASTContext &Ctx = AST->getASTContext();
    WPASuite Suite = makeWPASuite();
    auto Result = std::make_unique<NonEscapingParametersResult>();
    if (const FunctionDecl *FD = findFnByName(Fn, Ctx)) {
      auto Name = getQualifiedEntityName(FD, TUNs, LUNs);
      EXPECT_TRUE(Name.has_value());
      EntityId Id = getIdTable(Suite).getId(*Name);
      for (unsigned I : Indices)
        Result->NonEscaping[Id].insert(I);
    }
    getData(Suite)[NonEscapingParametersResult::analysisName()] = std::move(Result);

    RecordingEditEmitter Edits;
    RecordingReportEmitter Report;
    SSAFOptions Opts;
    Opts.CompilationUnitId = "cu";
    Opts.LinkUnitId = "lu";
    Opts.NoescapeSpelling = Spelling.str();
    InferNoescape(Suite, Opts, Edits, Report).HandleTranslationUnit(Ctx);

    tooling::Replacements Rs;
    for (const tooling::Replacement &R : Edits.Replacements)
      if (R.getFilePath() == "input.cc")
        cantFail(Rs.add(R));
    return {cantFail(tooling::applyAllReplacements(Code, Rs)), std::move(Report.Results)};
  }
};

TEST_F(InferNoescapeTest, PrefixInsertionOnNamedParameter) {
  Captured C = run("void f(int *p, int n) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p, int n) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 0u);
}

TEST_F(InferNoescapeTest, EveryRedeclarationIsEdited) {
  Captured C = run("void f(int *p);\nvoid f(int *q);\nvoid f(int *r) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p);\n"
                         "void f(__attribute__((noescape)) int *q);\n"
                         "void f(__attribute__((noescape)) int *r) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 3u);
}

TEST_F(InferNoescapeTest, UnnamedFunctionPointerArrayAndReferenceParameters) {
  Captured C = run("void f(int *, void (*cb)(int), int &r, int a[]) { }", "f", {0, 1, 2, 3});
  EXPECT_EQ(C.Rewritten,
            "void f(__attribute__((noescape)) int *, __attribute__((noescape)) void (*cb)(int), "
            "__attribute__((noescape)) int &r, __attribute__((noescape)) int a[]) { }");
}

TEST_F(InferNoescapeTest, SpellingOverride) {
  Captured C = run("void f(int *p) { }", "f", {0}, {"-std=c++20"}, "[[clang::noescape]]");
  EXPECT_EQ(C.Rewritten, "void f([[clang::noescape]] int *p) { }");
}

TEST_F(InferNoescapeTest, DefaultSpellingIsGNUInC) {
  Captured C = run("void f(int *p) { }", "f", {0}, {"-x", "c", "-std=c17"});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p) { }");
}

TEST_F(InferNoescapeTest, AlreadyAnnotatedIsSilentlySkipped) {
  Captured C = run("void f(__attribute__((noescape)) int *p) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 0u);
}

TEST_F(InferNoescapeTest, MacroParameterIsSkippedAndReported) {
  Captured C = run("#define P int *p\nvoid f(P) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "#define P int *p\nvoid f(P) { }");
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_EQ(C.Reports.front().Level, SarifResultLevel::Warning);
}

TEST_F(InferNoescapeTest, SystemHeaderRedeclIsSkippedAndReported) {
  tooling::FileContentMappings Files = {{"/sys/s.h", "#pragma clang system_header\nvoid f(int *p);\n"}};
  Captured C = run("#include <s.h>\nvoid f(int *p) { }", "f", {0},
                   {"-std=c++20", "-isystem/sys"}, "", Files);
  EXPECT_EQ(C.Rewritten, "#include <s.h>\nvoid f(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
}

TEST_F(InferNoescapeTest, RejectedCandidatesAreReportedAtTheDefinition) {
  std::unique_ptr<ASTUnit> AST = tooling::buildASTFromCodeWithArgs("void f(int *p);\nvoid f(int *p) { }", {"-std=c++20"});
  ASTContext &Ctx = AST->getASTContext();
  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  const FunctionDecl *FD = findFnByName("f", Ctx);
  EntityId Id = getIdTable(Suite).getId(*getQualifiedEntityName(FD, TUNs, LUNs));
  Result->Rejected[Node{Id, 0}] = {EscapeReason::StoreToGlobal, SourceLocationRecord{"input.cc", 2, 20}, "g", std::nullopt};
  getData(Suite)[NonEscapingParametersResult::analysisName()] = std::move(Result);
  RecordingEditEmitter Edits;
  RecordingReportEmitter Report;
  SSAFOptions Opts;
  Opts.CompilationUnitId = "cu";
  Opts.LinkUnitId = "lu";
  InferNoescape(Suite, Opts, Edits, Report).HandleTranslationUnit(Ctx);
  EXPECT_TRUE(Edits.Replacements.empty());
  ASSERT_EQ(Report.Results.size(), 1u) << "once, at the definition only";
  EXPECT_EQ(Report.Results[0].RuleId, NoescapeRejectedRuleId);
  EXPECT_NE(Report.Results[0].Message.find("StoreToGlobal"), std::string::npos);
}

} // namespace
```

Add `SourceTransformation/InferNoescapeTest.cpp` to the unittest CMake list.

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build ClangScalableAnalysisTests 2>&1 | tail -3`
Expected: `InferNoescape.h` not found.

- [ ] **Step 3: Add the option**

`SSAFOptions.h` — after `TransformationReportFile`:

```c++
  /// Attribute spelling inserted by the 'infer-noescape' source
  /// transformation. Empty means `__attribute__((noescape))`. Must be the
  /// same for every translation unit of a link unit.
  /// Controlled by: --ssaf-noescape-spelling
  std::string NoescapeSpelling;
```

`Options.td` — after `_ssaf_transformation_report_file`:

```
def _ssaf_noescape_spelling :
  Joined<["--"], "ssaf-noescape-spelling=">,
  MetaVarName<"<text>">,
  Group<SSAF_Group>,
  Visibility<[ClangOption, CC1Option]>,
  HelpText<
    "Attribute spelling inserted by the 'infer-noescape' SSAF source "
    "transformation. Defaults to '__attribute__((noescape))'. Pass the same "
    "value for every translation unit of a link unit.">,
  MarshallingInfoString<SSAFOpts<"NoescapeSpelling">>;
```

`clang/lib/Driver/ToolChains/Clang.cpp` — next to the existing `Args.AddLastArg(CmdArgs, options::OPT__ssaf_transformation_report_file);` line (search for `OPT__ssaf_` in that file), add:

```c++
  Args.AddLastArg(CmdArgs, options::OPT__ssaf_noescape_spelling);
```

- [ ] **Step 4: Implement the transformation**

`clang/include/clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h`:

```c++
//===- InferNoescape.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The infer-noescape transformation inserts the noescape attribute on every
// redeclaration of each parameter the NonEscapingParameters analysis proved
// non-escaping, and reports skipped sites and rejected candidates as SARIF.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_SOURCETRANSFORMATION_TRANSFORMATIONS_INFERNOESCAPE_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_SOURCETRANSFORMATION_TRANSFORMATIONS_INFERNOESCAPE_H

#include "clang/ScalableStaticAnalysis/SourceTransformation/Transformation.h"
#include "llvm/ADT/StringRef.h"

namespace clang::ssaf {

constexpr llvm::StringLiteral NoescapeInsertedRuleId = "noescape-inserted";
constexpr llvm::StringLiteral NoescapeSkippedRuleId = "noescape-skipped";
constexpr llvm::StringLiteral NoescapeRejectedRuleId = "noescape-rejected";
constexpr llvm::StringLiteral DefaultNoescapeSpelling = "__attribute__((noescape))";

class InferNoescape final : public Transformation {
public:
  using Transformation::Transformation;
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_SOURCETRANSFORMATION_TRANSFORMATIONS_INFERNOESCAPE_H
```

`clang/lib/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.cpp`:

```c++
//===- InferNoescape.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/SSAFOptions.h"
#include "clang/Lex/Lexer.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"
#include "clang/ScalableStaticAnalysis/Core/ASTEntityMapping.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityIdTable.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/SourceTransformation/TransformationRegistry.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "llvm/Support/FormatVariadic.h"
#include <map>
#include <vector>

using namespace clang;
using namespace clang::ssaf;

namespace {

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
                   const NestedBuildNamespace &TUNs, const NestedBuildNamespace &LUNs,
                   llvm::StringRef Spelling, SourceEditEmitter &Edits,
                   TransformationReportEmitter &Report)
      : Ctx(Ctx), SM(Ctx.getSourceManager()), Result(Result), NameToId(NameToId),
        RejectedByFunction(RejectedByFunction), TUNs(TUNs), LUNs(LUNs), Spelling(Spelling),
        Edits(Edits), Report(Report) {}

  bool VisitFunctionDecl(FunctionDecl *FD) override {
    if (FD->isTemplated())
      return true;
    std::optional<EntityName> Name = getQualifiedEntityName(FD, TUNs, LUNs);
    if (!Name)
      return true;
    auto IdIt = NameToId.find(*Name);
    if (IdIt == NameToId.end())
      return true;
    EntityId Id = IdIt->second;
    std::string USR = usrOf(FD);

    if (auto It = Result.NonEscaping.find(Id); It != Result.NonEscaping.end())
      for (unsigned Index : It->second)
        annotate(FD, Index, USR);

    if (FD->doesThisDeclarationHaveABody())
      if (auto It = RejectedByFunction.find(Id); It != RejectedByFunction.end())
        for (const auto &[N, Rej] : It->second)
          reportRejected(FD, N, *Rej, USR);
    return true;
  }

private:
  void annotate(FunctionDecl *FD, unsigned Index, llvm::StringRef USR) {
    if (Index >= FD->getNumParams())
      return;
    ParmVarDecl *P = FD->getParamDecl(Index);
    if (P->hasAttr<NoEscapeAttr>())
      return; // idempotent
    SourceLocation Loc = P->getBeginLoc();
    std::string Site = llvm::formatv("entity={0} param={1}", USR, Index);
    if (!FD->hasWrittenPrototype() || Loc.isInvalid() || Loc.isMacroID() ||
        SM.isInSystemHeader(Loc)) {
      Report.addResult(NoescapeSkippedRuleId, SarifResultLevel::Warning, rangeOf(P),
                       "cannot insert noescape here; a redeclaration elsewhere may "
                       "now mismatch (" + Site + ")");
      return;
    }
    tooling::Replacement R(SM, Loc, 0, (Spelling + " ").str());
    if (!R.isApplicable()) {
      Report.addResult(NoescapeSkippedRuleId, SarifResultLevel::Warning, rangeOf(P),
                       "replacement not applicable (" + Site + ")");
      return;
    }
    Edits.addReplacement(std::move(R));
    Report.addResult(NoescapeInsertedRuleId, SarifResultLevel::Note, rangeOf(P),
                     "inserted noescape (" + Site + ")");
  }

  void reportRejected(FunctionDecl *FD, const Node &N,
                      const NonEscapingParametersResult::Rejection &Rej, llvm::StringRef USR) {
    if (N.ParamIndex < 0 || static_cast<unsigned>(N.ParamIndex) >= FD->getNumParams())
      return;
    std::string DetailStr = Rej.Detail.empty() ? std::string() : " (" + Rej.Detail + ")";
    std::string BlameStr =
        Rej.Blame ? llvm::formatv(" via callee parameter {0}", Rej.Blame->ParamIndex).str()
                  : std::string();
    std::string Msg = llvm::formatv(
        "parameter {0} of {1} not annotated: {2} at {3}:{4}:{5}{6}{7}", N.ParamIndex, USR,
        escapeReasonName(Rej.Reason), Rej.Location.FilePath, Rej.Location.Line,
        Rej.Location.Column, DetailStr, BlameStr);
    Report.addResult(NoescapeRejectedRuleId, SarifResultLevel::Note,
                     rangeOf(FD->getParamDecl(N.ParamIndex)), Msg);
  }

  CharSourceRange rangeOf(const ParmVarDecl *P) {
    return Lexer::getAsCharRange(P->getSourceRange(), SM, Ctx.getLangOpts());
  }

  ASTContext &Ctx;
  const SourceManager &SM;
  const NonEscapingParametersResult &Result;
  const std::map<EntityName, EntityId> &NameToId;
  const std::map<EntityId, RejectionList> &RejectedByFunction;
  NestedBuildNamespace TUNs, LUNs;
  llvm::StringRef Spelling;
  SourceEditEmitter &Edits;
  TransformationReportEmitter &Report;
};

} // namespace

void InferNoescape::HandleTranslationUnit(ASTContext &Ctx) {
  auto Result = Suite.get<NonEscapingParametersResult>();
  if (!Result) {
    llvm::consumeError(Result.takeError());
    return;
  }
  std::map<EntityName, EntityId> NameToId;
  Suite.getIdTable().forEach([&](const EntityName &Name, EntityId Id) { NameToId.emplace(Name, Id); });
  std::map<EntityId, RejectionList> RejectedByFunction;
  for (const auto &[N, Rej] : Result->Rejected)
    RejectedByFunction[N.Function].push_back({N, &Rej});
  llvm::StringRef Spelling = Opts.NoescapeSpelling.empty() ? DefaultNoescapeSpelling
                                                           : llvm::StringRef(Opts.NoescapeSpelling);
  NoescapeRewriter(Ctx, *Result, NameToId, RejectedByFunction,
                   NestedBuildNamespace::makeCompilationUnit(Opts.CompilationUnitId),
                   NestedBuildNamespace::makeLinkUnit(Opts.LinkUnitId), Spelling, Edits, Report)
      .TraverseDecl(Ctx.getTranslationUnitDecl());
}

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int InferNoescapeAnchorSource = 0;
} // namespace clang::ssaf

static clang::ssaf::TransformationRegistry::Add<InferNoescape>
    RegisterInferNoescape("infer-noescape",
                          "Inserts noescape on parameters proven non-escaping");
```

Wiring: add `Transformations/InferNoescape.cpp` to the SourceTransformation CMake list; add `ANCHOR(InferNoescapeAnchorSource)` to `BuiltinAnchorSources.def` (alphabetical, after `EntitySourceLocationExtractorAnchorSource`). No new link libraries: `clangUnifiedSymbolResolution` already comes through `clangScalableStaticAnalysisCore`.

- [ ] **Step 5: Run the tests and the driver check**

Run: `ninja -C build ClangScalableAnalysisTests clang && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests --gtest_filter='InferNoescape*' && build/bin/clang --ssaf-noescape-spelling=__noescape -fsyntax-only -### -x c /dev/null 2>&1 | grep -c 'ssaf-noescape-spelling=__noescape'`
Expected: 9 PASS; the grep prints `1`.

- [ ] **Step 6: Commit**

```bash
git add clang/include/clang/Frontend/SSAFOptions.h clang/include/clang/Options/Options.td clang/lib/Driver/ToolChains/Clang.cpp clang/include/clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h clang/lib/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.cpp clang/lib/ScalableStaticAnalysis/SourceTransformation/CMakeLists.txt clang/include/clang/ScalableStaticAnalysis/BuiltinAnchorSources.def clang/unittests/ScalableStaticAnalysis/SourceTransformation/InferNoescapeTest.cpp clang/unittests/ScalableStaticAnalysis/CMakeLists.txt
git commit -m "[SSAF] Add the infer-noescape source transformation and --ssaf-noescape-spelling

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 11: Lit tests — extraction, whole-program, negative corpus, libc++, API Notes, end-to-end oracle

**Files:**
- Create: `clang/test/Analysis/Scalable/ParameterEscape/lit.local.cfg`
- Create: `clang/test/Analysis/Scalable/ParameterEscape/extraction.cpp`
- Create: `clang/test/Analysis/Scalable/ParameterEscape/determinism.cpp`
- Create: `clang/test/Analysis/Scalable/ParameterEscape/cross-tu.test`
- Create: `clang/test/Analysis/Scalable/ParameterEscape/libcxx-views.cpp`
- Create: `clang/test/Analysis/Scalable/ParameterEscape/apinotes-noescape.cpp` + `Inputs/apinotes/{module.modulemap,NoescapeLib.h,NoescapeLib.apinotes}`
- Create: `clang/test/Analysis/Scalable/ParameterEscape/negative/*.cpp|.mm` and `positive/*.cpp`
- Create: `clang/test/Analysis/Scalable/source-edit-generation/infer-noescape-end-to-end.test`
- Create: `clang/test/Analysis/Scalable/source-edit-generation/infer-noescape-mixed-language.test`

**Interfaces:** consumes the `ParameterEscape` extractor name, the `NonEscapingParametersResult` analysis name, the `infer-noescape` transformation, and the SARIF rule ids.

- [ ] **Step 1: Directory config**

`clang/test/Analysis/Scalable/ParameterEscape/lit.local.cfg`:

```python
config.suffixes = ['.c', '.cpp', '.mm', '.test']
```

- [ ] **Step 2: Extraction snapshot and determinism**

`extraction.cpp`:

```c++
// Per-TU ParameterEscape facts serialize with the keys this feature defines.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_cc1 -fsyntax-only -std=c++20 %s \
// RUN:   --ssaf-extract-summaries=ParameterEscape \
// RUN:   --ssaf-tu-summary-file=%t/tu.json \
// RUN:   --ssaf-compilation-unit-id=cu
// RUN: FileCheck %s --input-file=%t/tu.json

int *g;
void callee(int *a);
void flows(int *p) { callee(p); }
void sinks(int *p) { g = p; }
int *returns(int *p) { return p; }

// CHECK: ParameterEscape
// CHECK-DAG: "is_candidate": true
// CHECK-DAG: "flows_to"
// CHECK-DAG: "callee"
// CHECK-DAG: "reason": "StoreToGlobal"
// CHECK-DAG: "returns_self_at"
```

`determinism.cpp`:

```c++
// Two extractions of the same TU are byte-identical.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_cc1 -fsyntax-only -std=c++20 %s --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=%t/a.json --ssaf-compilation-unit-id=cu
// RUN: %clang_cc1 -fsyntax-only -std=c++20 %s --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=%t/b.json --ssaf-compilation-unit-id=cu
// RUN: diff %t/a.json %t/b.json
// RUN: clang-ssaf-linker %t/a.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/w1.json -a NonEscapingParametersResult
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/w2.json -a NonEscapingParametersResult
// RUN: diff %t/w1.json %t/w2.json

int *g;
void x(int *p); void y(int *p);
void a(int *p) { x(p); y(p); }
void x(int *p) { g = p; }
void y(int *p) { g = p; }
```

- [ ] **Step 3: Cross-TU whole-program test**

`cross-tu.test`:

```
// The callee's definition in TU B decides the annotation in TU A.
// RUN: rm -rf %t && mkdir -p %t
// RUN: split-file %s %t
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -I%t %t/a.cpp --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=%t/a.json --ssaf-compilation-unit-id=a
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -I%t %t/b.cpp --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=%t/b.json --ssaf-compilation-unit-id=b
// RUN: clang-ssaf-linker %t/a.json %t/b.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/wpa.json -a NonEscapingParametersResult
// RUN: FileCheck %s --input-file=%t/wpa.json

//--- shared.h
void keeps(int *p);
void reads(int *p);
void external(int *p);
void annotated_ext(__attribute__((noescape)) int *p);

//--- a.cpp
#include "shared.h"
void via_keeps(int *p) { keeps(p); }
void via_reads(int *p) { reads(p); }
void via_external(int *p) { external(p); }
void via_annotated(int *p) { annotated_ext(p); }
void self_recursive(int *p) { if (p) self_recursive(p + 1); }

//--- b.cpp
#include "shared.h"
int *g;
void keeps(int *p) { g = p; }
void reads(int *p) { *p = 1; }

// Bind each function's id through the id table (USR), then check membership.
// CHECK-DAG: "id": [[VIA_READS:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@via_reads#*I#"
// CHECK-DAG: "id": [[VIA_ANN:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@via_annotated#*I#"
// CHECK-DAG: "id": [[SELF:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@self_recursive#*I#"
// CHECK-DAG: "id": [[VIA_KEEPS:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@via_keeps#*I#"
// CHECK-DAG: "id": [[VIA_EXT:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@via_external#*I#"
// CHECK: "analysis_name": "NonEscapingParametersResult"
// CHECK: "non_escaping"
// CHECK-DAG: "@": [[VIA_READS]]
// CHECK-DAG: "@": [[VIA_ANN]]
// CHECK-DAG: "@": [[SELF]]
// CHECK: "rejected"
// CHECK-DAG: "@": [[VIA_KEEPS]]{{[^}]*}}}{{[^}]*}}"param": 0{{[^}]*}}}{{[^"]*}}"reason": "EscapesViaCallee"
// CHECK-DAG: "@": [[VIA_EXT]]{{[^}]*}}}{{[^}]*}}"param": 0{{[^}]*}}}{{[^"]*}}"reason": "UnanalyzedCallee"
```

The `"@": N` form is how `JSONFormat` prints an `EntityId` reference (see `clang/test/Analysis/Scalable/PointerFlow/lref-to-rref-cast.test`). If the emitted `rejected` entry lays out `node`/`reason` in a different key order than the two last regexes assume, run the pipeline once, look at `%t/wpa.json`, and adjust *only those two regexes*; the membership assertions must stay.

- [ ] **Step 4: Negative and positive corpus**

Every file in `negative/` shares this preamble (C++ files):

```c++
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -fblocks %s --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=%t/tu.json --ssaf-compilation-unit-id=cu
// RUN: clang-ssaf-linker %t/tu.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/wpa.json -a NonEscapingParametersResult
// RUN: FileCheck %s --input-file=%t/wpa.json
// CHECK: "non_escaping": []
// CHECK: "reason": "<REASON>"
```

Create these files (body → `<REASON>`); each body is the *entire* content after the preamble:

| file | body | `<REASON>` |
|---|---|---|
| `return.cpp` | `int *f(int *p) { return p; }` | `Return` |
| `store-to-field.cpp` | `struct S { int *m; }; void f(S *s, int *p) { s->m = p; }` — add `// CHECK-NOT: "@":` is **not** needed; `s` is clean but `p` is not, so `non_escaping` is non-empty here: use `// CHECK: "reason": "StoreToField"` only (drop the empty `non_escaping` line for this file) | `StoreToField` |
| `store-to-global.cpp` | `int *g; void f(int *p) { g = p; }` | `StoreToGlobal` |
| `store-through-pointer.cpp` | `void f(int **pp, int *p) { *pp = p; }` (as above: `pp` is clean; keep only the reason check) | `StoreThroughPointer` |
| `store-through-reference.cpp` | `void f(int *&out, int *p) { out = p; }` (reason check only) | `StoreThroughPointer` |
| `address-taken.cpp` | `void take(int **); void f(int *p) { take(&p); }` | `AddressTaken` |
| `indirect-call.cpp` | `void f(void (*fp)(int *), int *p) { fp(p); }` | `IndirectCall` |
| `unmatched-argument.cpp` | `void v(int, ...); void f(int *p) { v(0, p); }` | `UnmatchedArgument` |
| `cast-to-non-pointer.cpp` | `void f(int *p) { __UINTPTR_TYPE__ u = (__UINTPTR_TYPE__)p; (void)u; }` | `CastToNonPointer` |
| `throw.cpp` | `void f(int *p) { throw p; }` | `Throw` |
| `coroutine.cpp` | preamble RUN line adds `-I%S/../../../../SemaCXX/Inputs`; body: `#include "std-coroutine.h"` then `struct task { struct promise_type { task get_return_object(); std::suspend_never initial_suspend(); std::suspend_never final_suspend() noexcept; void return_void(); void unhandled_exception(); }; }; task co(int *p) { co_return; }` | `Coroutine` |
| `deallocation.cpp` | `extern "C" void free(void *); void f(int *p) { free(p); }` | `Deallocation` |
| `heap-allocation.cpp` | `struct H { int *q; H(int *p) : q(p) {} }; void f(int *p) { new H(p); }` | `HeapAllocation` |
| `non-trivial-view.cpp` | `struct [[gsl::Pointer(int)]] NT { int *d; NT(int *p) : d(p) {} ~NT(); }; void f(int *p) { NT v(p); (void)v; }` | `NonTrivialView` |
| `asm.cpp` | `void f(int *p) { __asm__("" : : "r"(p)); }` | `Asm` |
| `capture.cpp` | `void f(int *p) { auto l = [p] { return *p; }; (void)l; }` | `Capture` |
| `block-capture.cpp` | `void f(int *p) { void (^b)(void) = ^{ (void)*p; }; (void)b; }` | `Capture` |
| `virtual-call.cpp` | `struct B { virtual void m(int *); }; void f(B *b, int *p) { b->m(p); }` | `VirtualCall` |
| `unanalyzed-callee.cpp` | `void ext(int *); void f(int *p) { ext(p); }` | `UnanalyzedCallee` |
| `escapes-via-callee.cpp` | `int *g; void k(int *q) { g = q; } void f(int *p) { k(p); }` | `EscapesViaCallee` |
| `array-member-decay.cpp` | `struct A { int a[4]; }; int *g; void f(A *s) { g = s->a; }` | `StoreToGlobal` |
| `statement-expression.cpp` | `int *g; void f(int *p) { g = ({ p; }); }` | `StoreToGlobal` |
| `structured-binding.cpp` | `struct P { int x, y; }; int *g; void f(P *ps) { auto &[a, b] = *ps; g = &a; }` | `StoreToGlobal` |
| `cleanup-attribute.cpp` | `void keep(int **); void f(int *p) { int *q __attribute__((cleanup(keep))) = p; (void)q; }` | `AddressTaken` |
| `view-ctor-init.cpp` | `struct [[gsl::Pointer(int)]] V { int *d; V(int *p) : d(p) {} }; struct H2 { V v; H2(int *p) : v(p) {} };` | `StoreToField` |
| `objc-message.mm` | RUN line uses `-x objective-c++ -fobjc-arc -fblocks`; body: `@interface O - (void)take:(int *)p; @end void f(O *o, int *p) { [o take:p]; }` (reason check only; `o` is a non-candidate ObjC pointer) | `ObjCMessage` |
| `template-instantiation.cpp` | `struct Keep { static int *g; static void keep(int *p) { g = p; } }; struct Drop { static void keep(int *) {} }; template <class T> void store(int *p) { T::keep(p); } void a(int *p) { store<Drop>(p); } void b(int *p) { store<Keep>(p); }` with checks `// CHECK: "non_escaping"` / `// CHECK-NOT: store<` / `// CHECK: "rejected"` — i.e. no instantiation of `store` is ever listed as annotatable (both are non-candidates), while `a` is annotated and `b` rejected | (see checks) |

`positive/reads.cpp` (`int f(int *p) { return *p; }`) and `positive/view-accessor.cpp` (the `View` prelude from Task 5 followed by `int g(int *p) { View v(p, 1); return *v.data(); }`) use the same preamble with `// CHECK: "non_escaping": [ {{[[:space:]]*}}{` — proving the harness is not vacuous.

- [ ] **Step 5: libc++ views and API Notes**

`libcxx-views.cpp` (needs libc++ headers from the SDK; `REQUIRES: system-darwin`):

```c++
// REQUIRES: system-darwin
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -fsyntax-only -std=c++20 %s --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=%t/tu.json --ssaf-compilation-unit-id=cu
// RUN: clang-ssaf-linker %t/tu.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/wpa.json -a NonEscapingParametersResult
// RUN: FileCheck %s --input-file=%t/wpa.json
#include <span>
#include <string_view>
#include <vector>
int *g;
std::string_view gsv;
unsigned span_size(int *p, unsigned n) { return std::span<int>(p, n).size(); }
int first_via_data(int *p, unsigned n) { return *std::span<int>(p, n).data(); }
void push(std::vector<int *> &v, int *p) { v.push_back(p); }
void keep_view(const char *s) { gsv = std::string_view(s); }
// CHECK-DAG: "id": [[SPAN_SIZE:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@span_size#*I#i#"
// CHECK-DAG: "id": [[FIRST:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@first_via_data#*I#i#"
// CHECK-DAG: "id": [[PUSH:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@push#
// CHECK-DAG: "id": [[KEEP:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@keep_view#*1C#"
// CHECK: "non_escaping"
// CHECK-DAG: "@": [[SPAN_SIZE]]
// CHECK-DAG: "@": [[FIRST]]
// CHECK: "rejected"
// CHECK-DAG: "@": [[PUSH]]
// CHECK-DAG: "@": [[KEEP]]
```

If `span_size`/`first_via_data` land in `rejected`, print their rejection reason: the expected precision depends on libc++'s `span(It, size_type)` constructor storing through `std::to_address` (a call whose result is an alias) and its `size()` reading a non-pointer member. A rejection with reason `UnrecognizedUse` names the construct to model; do not weaken the test.

API Notes: `Inputs/apinotes/module.modulemap`:

```
module NoescapeLib {
  header "NoescapeLib.h"
  export *
}
```

`Inputs/apinotes/NoescapeLib.h`:

```c
void lib_take(int *p);
void lib_keep(int *p);
```

`Inputs/apinotes/NoescapeLib.apinotes`:

```yaml
Name: NoescapeLib
Functions:
  - Name: lib_take
    Parameters:
      - Position: 0
        NoEscape: true
```

`apinotes-noescape.cpp`:

```c++
// API Notes attach noescape to a bodiless SDK declaration; the analysis trusts it.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_cc1 -fsyntax-only -std=c++20 -fmodules -fimplicit-module-maps -fmodules-cache-path=%t/mcp -fapinotes-modules -I %S/Inputs/apinotes %s --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=%t/tu.json --ssaf-compilation-unit-id=cu
// RUN: clang-ssaf-linker %t/tu.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/wpa.json -a NonEscapingParametersResult
// RUN: FileCheck %s --input-file=%t/wpa.json
#include "NoescapeLib.h"
void via_take(int *p) { lib_take(p); }
void via_keep(int *p) { lib_keep(p); }
// CHECK-DAG: "id": [[TAKE:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@via_take#*I#"
// CHECK-DAG: "id": [[KEEP:[0-9]+]],{{([^]]|[[:space:]])+}}"usr": "c:@F@via_keep#*I#"
// CHECK: "non_escaping"
// CHECK-DAG: "@": [[TAKE]]
// CHECK: "rejected"
// CHECK-DAG: "@": [[KEEP]]
```

- [ ] **Step 6: End-to-end oracle**

`clang/test/Analysis/Scalable/source-edit-generation/infer-noescape-end-to-end.test`:

```
// Full pipeline over two TUs sharing a header: extract, link, analyze,
// transform each TU, merge, apply. Then verify (1) every inserted site
// (distinct, after merge) is present in the tree, (2) the rewritten tree
// compiles, (3) -Wlifetime-safety-noescape is silent, (4) a second full run
// produces no edits.
// REQUIRES: clang-apply-replacements
// RUN: rm -rf %t && mkdir -p %t/edits %t/merged %t/edits2 %t/merged2
// RUN: split-file %s %t

// DEFINE: %{tu} = unset
// DEFINE: %{dir} = unset
// DEFINE: %{extract} = %clang -fsyntax-only -std=c++20 -I%t %t/%{tu}.cpp --ssaf-extract-summaries=ParameterEscape --ssaf-compilation-unit-id=%{tu} --ssaf-tu-summary-file=%t/%{tu}.tu.json
// DEFINE: %{transform} = %clang -fsyntax-only -std=c++20 -I%t %t/%{tu}.cpp --ssaf-source-transformation=infer-noescape --ssaf-global-scope-analysis-result=%t/wpa.json --ssaf-src-edit-file=%{dir}/%{tu}.edits.yaml --ssaf-transformation-report-file=%{dir}/%{tu}.sarif --ssaf-compilation-unit-id=%{tu} --ssaf-link-unit-id=lu

// REDEFINE: %{tu} = a
// RUN: %{extract}
// REDEFINE: %{tu} = b
// RUN: %{extract}
// RUN: clang-ssaf-linker %t/a.tu.json %t/b.tu.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/wpa.json -a NonEscapingParametersResult
// REDEFINE: %{dir} = %t/edits
// REDEFINE: %{tu} = a
// RUN: %{transform}
// REDEFINE: %{tu} = b
// RUN: %{transform}
// RUN: clang-ssaf-src-edit-merge %t/edits/a.edits.yaml %t/edits/b.edits.yaml -o %t/merged/merged.yaml
// RUN: clang-apply-replacements %t/merged

// (1) Distinct merged replacements == attributes now present in the tree.
// RUN: grep -c 'Offset:' %t/merged/merged.yaml > %t/merged.txt
// RUN: cat %t/shared.h %t/a.cpp %t/b.cpp | grep -c '__attribute__((noescape))' > %t/present.txt
// RUN: diff %t/merged.txt %t/present.txt

// (2)+(3) The rewritten tree compiles and the checker is silent.
// RUN: %clang -fsyntax-only -std=c++20 -I%t -Werror -Wlifetime-safety-noescape %t/a.cpp
// RUN: %clang -fsyntax-only -std=c++20 -I%t -Werror -Wlifetime-safety-noescape %t/b.cpp
// RUN: FileCheck --check-prefix=SHARED --input-file=%t/shared.h %s
// SHARED: int sum(__attribute__((noescape)) const int *values, unsigned n);
// SHARED-NEXT: void keep(int *p);

// (4) Second full run produces no edits.
// REDEFINE: %{tu} = a
// RUN: %{extract}
// REDEFINE: %{tu} = b
// RUN: %{extract}
// RUN: clang-ssaf-linker %t/a.tu.json %t/b.tu.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/wpa.json -a NonEscapingParametersResult
// REDEFINE: %{dir} = %t/edits2
// REDEFINE: %{tu} = a
// RUN: %{transform}
// REDEFINE: %{tu} = b
// RUN: %{transform}
// RUN: not grep -q 'Offset:' %t/edits2/a.edits.yaml
// RUN: not grep -q 'Offset:' %t/edits2/b.edits.yaml

//--- shared.h
int sum(const int *values, unsigned n);
void keep(int *p);

//--- a.cpp
#include "shared.h"
int sum(const int *values, unsigned n) { int s = 0; for (unsigned i = 0; i < n; ++i) s += values[i]; return s; }

//--- b.cpp
#include "shared.h"
int *g;
void keep(int *p) { g = p; }
int use(int *p) { keep(p); return sum(p, 1); }
```

Notes: the `SHARED:` directives live above the first `//---` marker, so they are not part of `shared.h` and cannot match themselves. `clang-apply-replacements` is pointed at `%t/merged`, which contains only the merged YAML. `clang-ssaf-src-edit-merge -o` is its required output flag.

- [ ] **Step 7: Mixed-language header**

`infer-noescape-mixed-language.test`:

```
// A header shared by a C TU and a C++ TU receives one deduplicated insertion.
// REQUIRES: clang-apply-replacements
// RUN: rm -rf %t && mkdir -p %t/edits %t/merged
// RUN: split-file %s %t
// RUN: %clang -fsyntax-only -x c -std=c17 -I%t %t/a.c --ssaf-extract-summaries=ParameterEscape --ssaf-compilation-unit-id=a --ssaf-tu-summary-file=%t/a.tu.json
// RUN: %clang -fsyntax-only -std=c++20 -I%t %t/b.cpp --ssaf-extract-summaries=ParameterEscape --ssaf-compilation-unit-id=b --ssaf-tu-summary-file=%t/b.tu.json
// RUN: clang-ssaf-linker %t/a.tu.json %t/b.tu.json -o %t/lu.json
// RUN: clang-ssaf-analyzer %t/lu.json -o %t/wpa.json -a NonEscapingParametersResult
// RUN: %clang -fsyntax-only -x c -std=c17 -I%t %t/a.c --ssaf-source-transformation=infer-noescape --ssaf-global-scope-analysis-result=%t/wpa.json --ssaf-src-edit-file=%t/edits/a.edits.yaml --ssaf-transformation-report-file=%t/edits/a.sarif --ssaf-compilation-unit-id=a --ssaf-link-unit-id=lu
// RUN: %clang -fsyntax-only -std=c++20 -I%t %t/b.cpp --ssaf-source-transformation=infer-noescape --ssaf-global-scope-analysis-result=%t/wpa.json --ssaf-src-edit-file=%t/edits/b.edits.yaml --ssaf-transformation-report-file=%t/edits/b.sarif --ssaf-compilation-unit-id=b --ssaf-link-unit-id=lu
// RUN: clang-ssaf-src-edit-merge %t/edits/a.edits.yaml %t/edits/b.edits.yaml -o %t/merged/merged.yaml
// RUN: grep -c 'shared.h' %t/merged/merged.yaml | FileCheck --check-prefix=ONE %s
// ONE: 1
// RUN: clang-apply-replacements %t/merged
// RUN: %clang -fsyntax-only -x c -std=c17 -I%t -Werror %t/a.c
// RUN: %clang -fsyntax-only -std=c++20 -I%t -Werror %t/b.cpp

//--- shared.h
#ifdef __cplusplus
extern "C" {
#endif
int sum(const int *values, unsigned n);
#ifdef __cplusplus
}
#endif

//--- a.c
#include "shared.h"
int sum(const int *values, unsigned n) { int s = 0; for (unsigned i = 0; i < n; ++i) s += values[i]; return s; }

//--- b.cpp
#include "shared.h"
int use(int *p) { return sum(p, 1); }
```

- [ ] **Step 8: Run the lit tests**

Run: `ninja -C build clang clang-ssaf-linker clang-ssaf-analyzer clang-ssaf-src-edit-merge FileCheck split-file not count && build/bin/llvm-lit -sv clang/test/Analysis/Scalable/ParameterEscape clang/test/Analysis/Scalable/source-edit-generation clang/test/Analysis/Scalable/PointerFlow`
Expected: all PASS; the two end-to-end tests are skipped (`REQUIRES: clang-apply-replacements`) unless `clang-tools-extra` is enabled — enable it (`-DLLVM_ENABLE_PROJECTS='clang;clang-tools-extra'`) in a scratch build directory and run them there before declaring the task done.

- [ ] **Step 9: Commit**

```bash
git add clang/test/Analysis/Scalable/ParameterEscape clang/test/Analysis/Scalable/source-edit-generation/infer-noescape-end-to-end.test clang/test/Analysis/Scalable/source-edit-generation/infer-noescape-mixed-language.test
git commit -m "[SSAF][ParameterEscape] Add lit coverage: extraction, cross-TU, corpus, libc++, API Notes, end-to-end oracle

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 12: Corpus validator (with CMake target), exit-report script, user documentation

**Files:**
- Create: `clang/utils/ssaf/validate_escape_corpus.py`
- Create: `clang/utils/ssaf/noescape_exit_report.py`
- Modify: `clang/test/CMakeLists.txt` (custom target `check-clang-ssaf-escape-corpus`)
- Create: `clang/docs/ScalableStaticAnalysis/user-docs/NoescapeInference.md`

- [ ] **Step 1: Corpus validator**

Add `// DRIVER:` lines to `store-to-global.cpp`, `store-to-field.cpp`, `return.cpp`, `escapes-via-callee.cpp`, `structured-binding.cpp`, `statement-expression.cpp`, `array-member-decay.cpp` in the negative corpus. Each DRIVER is a `main` that lets the callee retain a pointer into a dead local and then dereferences it, e.g. for `store-to-global.cpp`:

```c++
// DRIVER: int main() { int *k; { int local = 1; f(&local); k = g; } return *k; }
```

and for `return.cpp`:

```c++
// DRIVER: int main() { int *k; { int local = 1; k = f(&local); } return *k; }
```

`clang/utils/ssaf/validate_escape_corpus.py`:

```python
#!/usr/bin/env python3
"""Compile each negative-corpus file with its DRIVER under ASan and expect a report.

Proves the corpus consists of true escapes: a file whose driver does not trip
AddressSanitizer is not demonstrating an escape and must be fixed.
"""
import pathlib, re, subprocess, sys, tempfile

def main(clang, corpus_dir):
    failures, skipped = [], []
    for src in sorted(pathlib.Path(corpus_dir).glob("*.cpp")):
        text = src.read_text()
        m = re.search(r"// DRIVER: (.*)", text)
        if not m:
            skipped.append(src.name)
            continue
        body = re.sub(r"^// (RUN|CHECK|DRIVER).*$", "", text, flags=re.M)
        with tempfile.TemporaryDirectory() as td:
            prog = pathlib.Path(td) / "prog.cpp"
            prog.write_text(body + "\n" + m.group(1) + "\n")
            exe = pathlib.Path(td) / "prog"
            cc = subprocess.run([clang, "-std=c++20", "-O2", "-g", "-fsanitize=address",
                                 "-fno-omit-frame-pointer", str(prog), "-o", str(exe)],
                                capture_output=True, text=True)
            if cc.returncode != 0:
                failures.append((src.name, "compile failed:\n" + cc.stderr))
                continue
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            if run.returncode == 0 or "AddressSanitizer" not in run.stderr:
                failures.append((src.name, "no ASan report: not a demonstrated escape"))
    for name in skipped:
        print(f"not runtime-validated: {name}")
    for name, why in failures:
        print(f"FAIL {name}: {why}")
    print(f"{len(failures)} failure(s), {len(skipped)} skipped")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
```

`clang/test/CMakeLists.txt` — append:

```cmake
add_custom_target(check-clang-ssaf-escape-corpus
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../utils/ssaf/validate_escape_corpus.py"
          "$<TARGET_FILE:clang>"
          "${CMAKE_CURRENT_SOURCE_DIR}/Analysis/Scalable/ParameterEscape/negative"
  DEPENDS clang
  COMMENT "Validating the ParameterEscape negative corpus under AddressSanitizer"
  USES_TERMINAL)
```

Run: `ninja -C build check-clang-ssaf-escape-corpus`
Expected: `0 failure(s)`; every DRIVER file produces an ASan report.

- [ ] **Step 2: Exit report**

`clang/utils/ssaf/noescape_exit_report.py`:

```python
#!/usr/bin/env python3
"""Summarize infer-noescape SARIF reports: counts per rule and rejection reasons."""
import collections, json, pathlib, re, sys

def main(root):
    rules = collections.Counter()
    reasons = collections.Counter()
    for path in pathlib.Path(root).rglob("*.sarif"):
        doc = json.loads(path.read_text())
        for run in doc.get("runs", []):
            for res in run.get("results", []):
                rule = res.get("ruleId", "")
                rules[rule] += 1
                if rule == "noescape-rejected":
                    text = res.get("message", {}).get("text", "")
                    m = re.search(r"not annotated: (\w+)", text)
                    reasons[m.group(1) if m else "?"] += 1
    for rule in ("noescape-inserted", "noescape-skipped", "noescape-rejected"):
        print(f"{rule}: {rules[rule]}")
    for reason, n in reasons.most_common():
        print(f"  {reason}: {n}")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
```

Run: `python3 clang/utils/ssaf/noescape_exit_report.py <the %t/edits directory of the end-to-end test>`
Expected: three rule lines followed by reason counts.

- [ ] **Step 3: User documentation**

`clang/docs/ScalableStaticAnalysis/user-docs/NoescapeInference.md` with these sections, each a short paragraph plus the exact commands from the end-to-end test where relevant:
1. *What it infers* — M1 scope (pointer/reference/tracked-view parameters of non-virtual, non-template C/C++ definitions; direct calls).
2. *Pipeline* — the six commands (`--ssaf-extract-summaries=ParameterEscape`, `clang-ssaf-linker`, `clang-ssaf-analyzer -a NonEscapingParametersResult`, `--ssaf-source-transformation=infer-noescape` with `--ssaf-noescape-spelling`, `clang-ssaf-src-edit-merge`, `clang-apply-replacements`).
3. *What is trusted* — declared `noescape` (source/API Notes) and the LLVM-verified libcall table; everything else escapes.
4. *Reading the report* — the three rule ids; before applying, compare `noescape-skipped` sites against `noescape-inserted` sites by location (spec §5.5); consequences of a remote mismatch in C (silently merged) vs C++ (`conflicting types`).
5. *Known precision gaps* — `memcpy`/`memset` destinations, non-view aggregates holding pointers, non-constructor `this` stores, virtual/ObjC/callables deferred to later milestones.
6. Link to the spec.

- [ ] **Step 4: Run everything once and commit**

Run: `ninja -C build ClangScalableAnalysisTests && build/tools/clang/unittests/ScalableStaticAnalysis/ClangScalableAnalysisTests && build/bin/llvm-lit -sv clang/test/Analysis/Scalable && ninja -C build check-clang-ssaf-escape-corpus`
Expected: all PASS.

```bash
git add clang/utils/ssaf clang/test/CMakeLists.txt clang/test/Analysis/Scalable/ParameterEscape/negative clang/docs/ScalableStaticAnalysis/user-docs/NoescapeInference.md
git commit -m "[SSAF][ParameterEscape] Add corpus validator target, exit report, and user docs

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Revision notes (v1 → v2, after adversarial review)

- Classifier: replaced "skip when the parent is alias-kinded" with an explicit pass-through whitelist (`isCoveredByParent`); call arguments, implicit objects and assignment right-hand sides are always classified. `VisitExpr` → `VisitStmt` (the visitor has no `VisitExpr`); `Ctx.getRecordType` → `Ctx.getCanonicalTagType`; constructor initializers handled via the `CXXConstructorDecl` parent and `CD->inits()` (the parent map does not model `CXXCtorInitializer`); reference-typed variables are never store targets that count as local; `BindingDecl`, `StmtExpr`, `CleanupAttr` and `__block` handled; glvalue call results typed by referent.
- Library knowledge: static table verified against LLVM in a unit test; no LLVM IR components added to `clangScalableStaticAnalysisAnalyses` (which `clangDriver` and the SSAF tools link).
- Summary: `FlowsTo` carries the call site, so `EscapesViaCallee`/`UnanalyzedCallee` rejections report a real location.
- Tests: harness checks registration before `makeTUSummaryExtractor` (which asserts); C tests omit the C++ prelude; `NonTrivialView` is view-like; `View g_view(nullptr, 0)`; libcall test asserts a flow, not a fixpoint reason; explicit-object resolver test; array-parameter emitter test; system-header candidacy test; determinism tests; API Notes and libc++ lit tests; corrected `template-instantiation` expectations; end-to-end test with `DEFINE` before `REDEFINE`, merged-only apply directory, count oracle on the merged YAML, `SHARED` checks outside the split file; mixed-language test written out; corpus validator wired to a CMake target.
- Spec deltas recorded in the spec: libcall table instead of runtime LLVM query; `FlowsTo` map with sites; reference/binding/cleanup/statement-expression rules.
