//===- ParameterEscapeExtractorTest.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// The candidacy predicates are private to the extractor. Reaching into the
// library directory matches ModelStringConversionsTest.cpp.
#include "../../../../lib/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeClassifier.h"
#include "../../FindDecl.h"
#include "../../TestFixture.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/PCHContainerOperations.h"
#include "clang/Frontend/SSAFOptions.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/ExtractorRegistry.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummary.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryBuilder.h"
#include "clang/Tooling/Tooling.h"
#include "gtest/gtest.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

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
struct [[gsl::Pointer(int)]] CopyCtorView { int *d; CopyCtorView(const CopyCtorView &o); };
int *g_ptr; static int *s_ptr;
void unknown(int *); void noesc(__attribute__((noescape)) int *);
typedef __SIZE_TYPE__ size_t; extern "C" size_t strlen(const char *);
extern "C" void free(void *); extern "C" void *memcpy(void *, const void *, size_t);
)cpp";

// The contents of clang/test/SemaCXX/Inputs/std-coroutine.h.
constexpr const char *CoroutineHeader = R"cpp(
#ifndef STD_COROUTINE_H
#define STD_COROUTINE_H

namespace std {

template<typename T> struct remove_reference       { typedef T type; };
template<typename T> struct remove_reference<T &>  { typedef T type; };
template<typename T> struct remove_reference<T &&> { typedef T type; };

template<typename T>
typename remove_reference<T>::type &&move(T &&t) noexcept;

struct input_iterator_tag {};
struct forward_iterator_tag : public input_iterator_tag {};

template <class Ret, typename... T>
struct coroutine_traits { using promise_type = typename Ret::promise_type; };

template <class Promise = void>
struct coroutine_handle {
  static coroutine_handle from_address(void *) noexcept;
  static coroutine_handle from_promise(Promise &promise);
  constexpr void* address() const noexcept;
};
template <>
struct coroutine_handle<void> {
  template <class PromiseType>
  coroutine_handle(coroutine_handle<PromiseType>) noexcept;
  static coroutine_handle from_address(void *);
  constexpr void* address() const noexcept;
};

struct suspend_always {
  bool await_ready() noexcept { return false; }
  void await_suspend(coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

struct suspend_never {
  bool await_ready() noexcept { return true; }
  void await_suspend(coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

} // namespace std

#endif // STD_COROUTINE_H
)cpp";

class ParameterEscapeExtractorTest : public TestFixture {
protected:
  SSAFOptions Opts;
  /// Rebuilt by every setUp(). The builder keeps the first summary contributed
  /// under an EntityId and drops the rest, so a second setUp() in one test
  /// would read back the first one's facts for every name whose USR it reuses
  /// -- silently, and for exactly the cases a loop over compiler flags is
  /// written to distinguish.
  std::optional<TUSummary> TUSum;
  std::optional<TUSummaryBuilder> Builder;
  std::unique_ptr<TUSummaryExtractor> Extractor;
  std::unique_ptr<ASTUnit> AST;

  void resetSummaryState() {
    Extractor.reset();
    Builder.reset(); // holds a reference to TUSum; destroy it first
    TUSum.emplace(llvm::Triple("arm64-apple-macosx"),
                  BuildNamespace(BuildNamespaceKind::CompilationUnit,
                                 "Mock.cpp"));
    Builder.emplace(*TUSum, Opts);
  }

  /// A non-empty \p ExpectedError keeps going on an ill-formed TU, and
  /// requires a diagnostic containing that text. The extractor runs as an
  /// ASTConsumer whatever the diagnostics said, so a few reachable states only
  /// exist on an AST that contains errors -- naming the expected one keeps an
  /// unrelated mistake in the test input from passing for the state under
  /// test.
  bool setUp(StringRef Body, std::vector<std::string> Args = {"-std=c++20"},
             bool WithPrelude = true, tooling::FileContentMappings Files = {},
             StringRef ExpectedError = "") {
    resetSummaryState();
    std::string Code = WithPrelude ? (Prelude + Body.str()) : Body.str();
    // Diagnostics are only stored when capture is asked for, and a test that
    // names an expected error has to be able to read it back.
    AST = tooling::buildASTFromCodeWithArgs(
        Code, Args, "input.cc", "clang-tool",
        std::make_shared<PCHContainerOperations>(),
        tooling::getClangStripDependencyFileAdjuster(), Files,
        /*DiagConsumer=*/nullptr, llvm::vfs::getRealFileSystem(),
        ExpectedError.empty()
            ? CaptureDiagsKind::None
            : CaptureDiagsKind::AllWithoutNonErrorsFromIncludes);
    if (!AST) {
      ADD_FAILURE() << "could not build an AST";
      return false;
    }
    if (ExpectedError.empty()) {
      if (AST->getDiagnostics().hasErrorOccurred()) {
        ADD_FAILURE() << "test code has errors";
        return false;
      }
    } else {
      bool Found = false;
      std::string Seen;
      for (const StoredDiagnostic &D : AST->storedDiagnostics()) {
        if (D.getLevel() < DiagnosticsEngine::Error)
          continue;
        Seen += "\n  " + D.getMessage().str();
        Found |= D.getMessage().contains(ExpectedError);
      }
      if (!Found) {
        ADD_FAILURE() << "expected an error containing '" << ExpectedError
                      << "', saw:" << (Seen.empty() ? " (none)" : Seen);
        return false;
      }
    }
    if (!isTUSummaryExtractorRegistered(ParameterEscapeSummary::Name)) {
      ADD_FAILURE() << "ParameterEscape extractor not registered";
      return false;
    }
    Extractor = makeTUSummaryExtractor(ParameterEscapeSummary::Name, *Builder);
    Extractor->HandleTranslationUnit(AST->getASTContext());
    return true;
  }

  const ParameterEscapeSummary *summaryOfDecl(const FunctionDecl *FD) {
    if (!FD)
      return nullptr;
    std::optional<EntityId> Id = Extractor->addEntity(FD);
    if (!Id) {
      ADD_FAILURE() << "no entity for " << FD->getNameAsString();
      return nullptr;
    }
    auto &Data = getData(*TUSum);
    auto It = Data.find(ParameterEscapeSummary::summaryName());
    if (It == Data.end())
      return nullptr;
    auto E = It->second.find(*Id);
    if (E == It->second.end())
      return nullptr;
    return static_cast<const ParameterEscapeSummary *>(E->second.get());
  }

  const ParameterEscapeSummary *summaryOf(StringRef Fn) {
    const FunctionDecl *FD = findFnByName(Fn, AST->getASTContext());
    if (!FD) {
      ADD_FAILURE() << "no function " << Fn;
      return nullptr;
    }
    return summaryOfDecl(FD);
  }

  // The user-declared constructor of `Class` with `NumParams` parameters.
  const ParameterEscapeSummary *ctorSummaryOf(StringRef Class,
                                              unsigned NumParams) {
    return summaryOfDecl(ctorOf(Class, NumParams));
  }

  // The user-declared constructor of \p Class with \p NumParams parameters.
  const CXXConstructorDecl *ctorOf(StringRef Class, unsigned NumParams) {
    const auto *RD = findDeclByName<CXXRecordDecl>(Class, AST->getASTContext());
    if (!RD) {
      ADD_FAILURE() << "no class " << Class;
      return nullptr;
    }
    for (const CXXConstructorDecl *CD : RD->getDefinition()->ctors())
      if (!CD->isImplicit() && CD->getNumParams() == NumParams)
        return CD;
    ADD_FAILURE() << "no ctor " << Class << "/" << NumParams;
    return nullptr;
  }

  const EscapeFact *factOf(StringRef Fn, unsigned Index) {
    const ParameterEscapeSummary *S = summaryOf(Fn);
    if (!S)
      return nullptr;
    auto It = S->Params.find(Index);
    return It == S->Params.end() ? nullptr : &It->second;
  }

  const EscapeFact *factOfDecl(const FunctionDecl *FD, unsigned Index) {
    const ParameterEscapeSummary *S = summaryOfDecl(FD);
    if (!S)
      return nullptr;
    auto It = S->Params.find(Index);
    return It == S->Params.end() ? nullptr : &It->second;
  }

  std::optional<EscapeReason> sinkOfDecl(const FunctionDecl *FD,
                                         unsigned Index) {
    const EscapeFact *F = factOfDecl(FD, Index);
    if (!F) {
      ADD_FAILURE() << "no fact for parameter " << Index;
      return std::nullopt;
    }
    return F->OtherSink ? std::optional(F->OtherSink->Reason) : std::nullopt;
  }

  const EscapeFact *thisFactOfDecl(const FunctionDecl *FD) {
    const ParameterEscapeSummary *S = summaryOfDecl(FD);
    return S && S->This ? &*S->This : nullptr;
  }

  // The `this` sink reason, or nullopt when there is none. Fails the test if
  // the declaration carries no `this` fact at all.
  std::optional<EscapeReason> sinkOfThisDecl(const FunctionDecl *FD) {
    const EscapeFact *F = thisFactOfDecl(FD);
    if (!F) {
      ADD_FAILURE() << "no `this` fact";
      return std::nullopt;
    }
    return F->OtherSink ? std::optional(F->OtherSink->Reason) : std::nullopt;
  }

  const EscapeFact *thisFactOf(StringRef Fn) {
    const ParameterEscapeSummary *S = summaryOf(Fn);
    return S && S->This ? &*S->This : nullptr;
  }

  // Sink reason, or nullopt when there is none. Fails the test if no fact.
  std::optional<EscapeReason> sinkOf(StringRef Fn, unsigned Index) {
    const EscapeFact *F = factOf(Fn, Index);
    if (!F) {
      ADD_FAILURE() << "no fact for " << Fn << "#" << Index;
      return std::nullopt;
    }
    return F->OtherSink ? std::optional(F->OtherSink->Reason) : std::nullopt;
  }

  bool flowsTo(StringRef Fn, unsigned Index, StringRef Callee,
               int CalleeIndex) {
    const EscapeFact *F = factOf(Fn, Index);
    const FunctionDecl *CD = findFnByName(Callee, AST->getASTContext());
    if (!F || !CD)
      return false;
    std::optional<EntityId> Id = Extractor->addEntity(CD);
    return Id && F->FlowsTo.count(FlowTarget{*Id, CalleeIndex});
  }

  // Like flowsTo(), but naming the callee by declaration -- constructors and
  // other functions findFnByName() cannot pick out unambiguously.
  bool flowsToDecl(const EscapeFact *F, const FunctionDecl *Callee,
                   int CalleeIndex) {
    if (!F || !Callee)
      return false;
    std::optional<EntityId> Id = Extractor->addEntity(Callee);
    return Id && F->FlowsTo.count(FlowTarget{*Id, CalleeIndex});
  }

  // A fact is "clean" when it neither sinks, nor returns, nor flows.
  static bool isClean(const EscapeFact *F) {
    return F && !F->OtherSink && !F->returnsSelf() && F->FlowsTo.empty();
  }

  // A parameter is "clean" when it neither sinks, nor returns, nor flows.
  bool clean(StringRef Fn, unsigned Index) {
    const EscapeFact *F = factOf(Fn, Index);
    if (!F) {
      ADD_FAILURE() << "no fact for " << Fn << "#" << Index;
      return false;
    }
    return !F->OtherSink && !F->returnsSelf() && F->FlowsTo.empty();
  }

  // The set of parameter indices that carry an escape fact.
  std::set<unsigned> analyzedParamsOf(StringRef Fn) {
    std::set<unsigned> Result;
    const ParameterEscapeSummary *S = summaryOf(Fn);
    if (!S) {
      ADD_FAILURE() << "no summary for " << Fn;
      return Result;
    }
    for (const auto &[Index, Fact] : S->Params)
      Result.insert(Index);
    return Result;
  }
};

//===--- Candidacy --------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, PlainDefinitionIsCandidate) {
  ASSERT_TRUE(setUp("void f(int *p, int n, View v, int &r) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_TRUE(S->IsCandidate);
  // `v` was index 2 while view types were tracked; M1 no longer tracks them.
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{0, 3}));
}

TEST_F(ParameterEscapeExtractorTest, VirtualIsNotCandidate) {
  ASSERT_TRUE(setUp("struct B { virtual void f(int *p) { } };"));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, MainIsNotCandidate) {
  ASSERT_TRUE(setUp("int main(int argc, char **argv) { return 0; }"));
  const ParameterEscapeSummary *S = summaryOf("main");
  ASSERT_NE(S, nullptr) << "main must still be analyzed";
  EXPECT_FALSE(S->IsCandidate);
  // `argv` is of candidate *type*; only the definition-level rule rejects it.
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{1}));
}

TEST_F(ParameterEscapeExtractorTest,
       TemplateInstantiationIsAnalyzedButNotCandidate) {
  ASSERT_TRUE(setUp("template <class T> void tf(int *p) { } void use(int *q) { "
                    "tf<int>(q); }"));
  const ParameterEscapeSummary *S = summaryOf("tf");
  ASSERT_NE(S, nullptr) << "instantiations must still be analyzed";
  EXPECT_FALSE(S->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, ClassTemplateMemberIsNotCandidate) {
  ASSERT_TRUE(setUp("template <class T> struct C { void m(int *p) { } }; void "
                    "use(C<int> c, int *q) { c.m(q); }"));
  ASSERT_NE(summaryOf("m"), nullptr);
  EXPECT_FALSE(summaryOf("m")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, ExplicitSpecializationIsNotCandidate) {
  ASSERT_TRUE(setUp("template <class T> void ts(int *p); template <> void "
                    "ts<int>(int *p) { }"));
  ASSERT_NE(summaryOf("ts"), nullptr);
  EXPECT_FALSE(summaryOf("ts")->IsCandidate);
}

// A block-scope `extern` declaration inside a function template joins the
// redeclaration chain of the namespace-scope function, and is templated even
// though the definition is not. This is what makes the per-redeclaration
// "not templated" rule reachable at all.
TEST_F(ParameterEscapeExtractorTest, TemplatedRedeclIsNotCandidate) {
  ASSERT_TRUE(setUp("void fr(int *p);\n"
                    "template <class T> void tf() { extern void fr(int *p); }\n"
                    "void fr(int *p) { }"));
  const ParameterEscapeSummary *S = summaryOf("fr");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
}

// The parameter is inside the macro here, so this case is decided one layer
// later, by the parameter's own begin location.
TEST_F(ParameterEscapeExtractorTest, MacroRedeclIsNotCandidate) {
  ASSERT_TRUE(
      setUp("#define DECL(n) void n(int *p)\nDECL(f);\nvoid f(int *p) { }"));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

// A name-only macro expands the declaration's getLocation() but leaves every
// parameter written in source, so the redeclaration's own location is the only
// thing that is a macro. This is what pins `Loc.isMacroID()`; the test above
// passes with that line deleted.
// The rewritability gate runs over the *candidate* parameters, so narrowing
// candidacy widens it: a macro-spelled pointer-to-view no longer blocks the
// definition it sits in, and its neighbours become annotatable where they were
// not at the merge base. Deliberate -- refusing a definition over a parameter
// the transformation will never edit would lose every other parameter with it.
TEST_F(ParameterEscapeExtractorTest, TheRewritabilityGateFollowsCandidacy) {
  ASSERT_TRUE(setUp("struct [[gsl::Pointer(int)]] V { int *p; };\n"
                    "#define VPARAM V *vp\n"
                    "#define IPARAM int *ip\n"
                    "void with_view(int *q, VPARAM) { }\n"
                    "void with_pointer(int *q, IPARAM) { }\n",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const ParameterEscapeSummary *V = summaryOf("with_view");
  ASSERT_NE(V, nullptr);
  EXPECT_TRUE(V->IsCandidate);
  EXPECT_EQ(V->CandidateParams, (std::set<unsigned>{0}));
  // The control isolates the cause: a macro-spelled parameter that *is* a
  // candidate still refuses the whole definition.
  const ParameterEscapeSummary *P = summaryOf("with_pointer");
  ASSERT_NE(P, nullptr);
  EXPECT_FALSE(P->IsCandidate);
  EXPECT_EQ(P->CandidateParams, (std::set<unsigned>{0, 1}));
}

TEST_F(ParameterEscapeExtractorTest, MacroNamedRedeclIsNotCandidate) {
  ASSERT_TRUE(setUp("#define NAME f\nvoid NAME(int *p);\nvoid f(int *p) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  // The parameter itself is written in source and of candidate type.
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{0}));
}

TEST_F(ParameterEscapeExtractorTest,
       MacroTypedCandidateParameterIsNotCandidate) {
  ASSERT_TRUE(setUp("#define PTR int *\nvoid f(PTR p) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  // It is still of candidate type, and still analyzed -- only unrewritable.
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{0}));
}

TEST_F(ParameterEscapeExtractorTest,
       MacroTypedNonCandidateParameterDoesNotVeto) {
  ASSERT_TRUE(setUp("void f(int *p, __SIZE_TYPE__ n) { }"));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_TRUE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, SystemHeaderRedeclIsNotCandidate) {
  tooling::FileContentMappings Files = {
      {"/sys/s.h", "#pragma clang system_header\nvoid f(int *p);\n"}};
  ASSERT_TRUE(setUp("#include <s.h>\nvoid f(int *p) { }",
                    {"-std=c++20", "-isystem/sys"}, true, Files));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, KAndRDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp("void f(p) int *p; { }",
                    {"-x", "c", "-std=c17", "-Wno-deprecated-non-prototype"},
                    /*WithPrelude=*/false));
  ASSERT_NE(summaryOf("f"), nullptr);
  EXPECT_FALSE(summaryOf("f")->IsCandidate);
}

TEST_F(ParameterEscapeExtractorTest, CoroutineIsNotCandidate) {
  tooling::FileContentMappings Files = {
      {"/inc/std-coroutine.h", CoroutineHeader}};
  ASSERT_TRUE(
      setUp("#include <std-coroutine.h>\n"
            "struct task { struct promise_type { task get_return_object(); "
            "std::suspend_never initial_suspend(); std::suspend_never "
            "final_suspend() noexcept; "
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

//===--- One entity, more than one definition -----------------------------===//

// Multiversioned variants are separate top-level FunctionDecls with no `prev`
// link, so the per-redeclaration loop cannot see the siblings. They collide on
// one EntityId, extractAndAddSummaries keeps whichever summary arrives first,
// and the survivor describes only one of the differing bodies.
TEST_F(ParameterEscapeExtractorTest, MultiVersionDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp("int *gg;\n"
                    "__attribute__((target_version(\"sve\")))"
                    " void mv(int *p) { }\n"
                    "__attribute__((target_version(\"default\")))"
                    " void mv(int *p) { gg = p; }\n"));
  const ParameterEscapeSummary *S = summaryOf("mv");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
}

// The cross-TU case, and the one the collision detection below cannot help
// with: this TU defines only one variant, so there is no collision here at all
// and isMultiVersion() is the sole rejecter. The sibling body lives elsewhere.
TEST_F(ParameterEscapeExtractorTest, MultiVersionSoleDefinitionIsNotCandidate) {
  // Exactly one variant is visible here, so there is no collision in this TU
  // at all and isMultiVersion() is the only thing that can reject it. The
  // sibling bodies live in other TUs, which is the cross-TU case that matters.
  ASSERT_TRUE(setUp("__attribute__((target_version(\"default\")))"
                    " void mv(int *p) { }\n"));
  const ParameterEscapeSummary *S = summaryOf("mv");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  // Not the collision path: the body is classified as usual, and an empty body
  // uses nothing, so the fact is clean rather than degraded.
  EXPECT_TRUE(clean("mv", 0));
}

// A weak definition may be replaced at link time by a strong one from another
// TU. Both are in the link unit and first-contributor-wins picks between them
// arbitrarily, so annotating from this body can contradict the body that runs.
TEST_F(ParameterEscapeExtractorTest, WeakDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp("__attribute__((weak)) void wk(int *p) { }"));
  const ParameterEscapeSummary *S = summaryOf("wk");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
}

// A weakref is an alias: calls run the aliasee's body, never this one. clang
// rejects a weakref carrying a body, so this only exists on an error AST --
// which is exactly where a summary would still be produced for it.
TEST_F(ParameterEscapeExtractorTest, WeakRefDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp(
      "static void wr_target(int *p) { }\n"
      "static void wr(int *p) __attribute__((weakref(\"wr_target\"))) { }",
      {"-x", "c", "-std=c17", "-Wno-gcc-compat"}, /*WithPrelude=*/false,
      /*Files=*/{}, /*ExpectedError=*/"cannot also be an alias"));
  const ParameterEscapeSummary *S = summaryOf("wr");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  // The aliasee is an ordinary definition and stays a candidate, so this is
  // the weakref rule and not something rejecting the whole file.
  const ParameterEscapeSummary *T = summaryOf("wr_target");
  ASSERT_NE(T, nullptr);
  EXPECT_TRUE(T->IsCandidate);
}

// The glibc pattern: this body is never emitted, and the external definition
// that is called is invisible here.
TEST_F(ParameterEscapeExtractorTest, GnuInlineDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp("extern __inline__ __attribute__((gnu_inline))"
                    " void gi(int *p) { }",
                    {"-x", "c", "-std=c17"}, /*WithPrelude=*/false));
  const ParameterEscapeSummary *S = summaryOf("gi");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
}

// C11 6.7.4p7: with no external declaration in this TU it is unspecified
// whether a call uses the inline definition or the external definition.
TEST_F(ParameterEscapeExtractorTest, C99InlineDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp("inline void ci(int *p) { }", {"-x", "c", "-std=c17"},
                    /*WithPrelude=*/false));
  const ParameterEscapeSummary *S = summaryOf("ci");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
}

// A C++ inline function is GVA_DiscardableODR, not GVA_AvailableExternally:
// every definition of it is the same body, so it stays a candidate. Without
// this, widening the linkage rule would go unnoticed.
TEST_F(ParameterEscapeExtractorTest, CxxInlineDefinitionIsStillCandidate) {
  ASSERT_TRUE(setUp("inline void cxxinl(int *p) { }"));
  const ParameterEscapeSummary *S = summaryOf("cxxinl");
  ASSERT_NE(S, nullptr);
  EXPECT_TRUE(S->IsCandidate);
}

// Two `enable_if` overloads with identical parameter types share a USR, so
// they collide on one EntityId with no multiversioning at all. Whichever
// summary is retained must report an escape: the whole-program fixpoint reads
// a callee's facts whether or not the callee is a candidate, so a retained
// clean fact would let a *caller* be annotated.
TEST_F(ParameterEscapeExtractorTest,
       CollidingDefinitionsAreDegradedNotTrusted) {
  ASSERT_TRUE(setUp("int *gg;\n"
                    "__attribute__((enable_if(1, \"a\")))"
                    " void ov(int *p) { }\n"
                    "__attribute__((enable_if(0, \"b\")))"
                    " void ov(int *p) { gg = p; }\n"));
  const ParameterEscapeSummary *S = summaryOf("ov");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  const EscapeFact *F = factOf("ov", 0);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Detail, MultipleDefinitionsDetail.str());
  EXPECT_TRUE(F->FlowsTo.empty());
  EXPECT_FALSE(F->returnsSelf());
}

// A C USR carries no parameter types, so these two definitions share
// `c:@F@cf` while having different signatures: one summary is empty and one is
// not. This is the state in which recording the empty summary's pointer would
// dangle -- extractAndAddSummaries destroys it -- and it is reachable only on
// an AST that contains errors, which this extractor still runs on.
//
// What is asserted here is the functional contract: the surviving summary is
// degraded whichever order the groups arrive in. The dangling write itself is
// undefined behaviour and shows up deterministically only under a sanitizer or
// MallocScribble, so this test exercises the path rather than trapping it.
TEST_F(ParameterEscapeExtractorTest, CollidingDefinitionsOfDifferingEmptiness) {
  ASSERT_TRUE(setUp("void cf(int x) { }\nvoid cf(int *p) { }",
                    {"-x", "c", "-std=c17"}, /*WithPrelude=*/false,
                    /*Files=*/{}, /*ExpectedError=*/"conflicting types"));
  const ParameterEscapeSummary *S = summaryOf("cf");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  // The lexically first definition is `void cf(int x)`, which carries no
  // pointer-carrying parameter, so the rebuilt summary holds no node at all --
  // whichever of the two summaries the builder happened to retain.
  EXPECT_TRUE(S->Params.empty());
  EXPECT_TRUE(S->CandidateParams.empty());
}

// The same collision with the two definitions in the other source order. The
// retained summary is rebuilt from whichever is lexically first, so this one
// holds the pointer parameter's node while the test above holds none --
// neither depends on contributor iteration order.
TEST_F(ParameterEscapeExtractorTest, CollidingDefinitionsPickTheFirstBody) {
  ASSERT_TRUE(setUp("void cf(int *p) { }\nvoid cf(int x) { }",
                    {"-x", "c", "-std=c17"}, /*WithPrelude=*/false,
                    /*Files=*/{}, /*ExpectedError=*/"conflicting types"));
  const ParameterEscapeSummary *S = summaryOf("cf");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  EXPECT_EQ(analyzedParamsOf("cf"), (std::set<unsigned>{0}));
  const EscapeFact *F = factOf("cf", 0);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Detail, MultipleDefinitionsDetail.str());
  EXPECT_EQ(F->OtherSink->Location.Line, 1u);
}

// Two bodies in one redeclaration chain. This is a redefinition error, so it
// is reachable only on an AST that contains errors -- which this extractor
// runs on -- and it is the only thing `NumDefinitions > 1` catches.
TEST_F(ParameterEscapeExtractorTest, TwoBodiesInOneChainAreDegraded) {
  ASSERT_TRUE(setUp("void rd(int *p) { }\nvoid rd(int *p) { }", {"-std=c++20"},
                    /*WithPrelude=*/false,
                    /*Files=*/{}, /*ExpectedError=*/"redefinition"));
  const ParameterEscapeSummary *S = summaryOf("rd");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  const EscapeFact *F = factOf("rd", 0);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Detail, MultipleDefinitionsDetail.str());
  // Both bodies are in one contributor group, whose declaration order is
  // stable, so this pins the choice of the lexically first definition without
  // depending on the order groups arrive in.
  EXPECT_EQ(F->OtherSink->Location.Line, 1u);
}

// The cross-TU shape: this TU declares one `enable_if` overload it does not
// define and defines another that collides with it. Only one contributor group
// has a body, so the entity has to be registered even for the bodiless group
// or the collision is invisible -- and the definition would keep trusted,
// annotatable facts under an EntityId it shares with a function defined
// elsewhere.
TEST_F(ParameterEscapeExtractorTest, CollisionWithABodilessOverloadIsDegraded) {
  ASSERT_TRUE(setUp(
      "void ov(int *p) __attribute__((enable_if(p == nullptr, \"null\")));\n"
      "void ov(int *p) __attribute__((enable_if(p != nullptr, \"nn\"))) { "
      "}\n"));
  const ParameterEscapeSummary *S = summaryOf("ov");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  const EscapeFact *F = factOf("ov", 0);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Detail, MultipleDefinitionsDetail.str());
}

// The degradation must not depend on which colliding contributor group the
// builder happened to keep, and contributor order is not controllable from a
// test. Build the same colliding TU several times in one process instead and
// require every build to agree: a regression to "use whichever definition was
// collected first" only has to lose one of these builds to be caught.
TEST_F(ParameterEscapeExtractorTest, CollisionDegradationIsReproducible) {
  constexpr unsigned Builds = 32;
  std::optional<ParameterEscapeSummary> Expected;
  for (unsigned I = 0; I < Builds; ++I) {
    SSAFOptions O;
    TUSummary TU{
        llvm::Triple("arm64-apple-macosx"),
        BuildNamespace(BuildNamespaceKind::CompilationUnit, "Mock.cpp")};
    TUSummaryBuilder B{TU, O};
    std::unique_ptr<ASTUnit> A = tooling::buildASTFromCodeWithArgs(
        "void cf(int *p) { }\nvoid cf(int x) { }\n", {"-x", "c", "-std=c17"},
        "input.cc", "clang-tool", std::make_shared<PCHContainerOperations>(),
        tooling::getClangStripDependencyFileAdjuster());
    ASSERT_NE(A, nullptr) << "build " << I;
    std::unique_ptr<TUSummaryExtractor> E =
        makeTUSummaryExtractor(ParameterEscapeSummary::Name, B);
    E->HandleTranslationUnit(A->getASTContext());

    auto &Data = getData(TU);
    auto It = Data.find(ParameterEscapeSummary::summaryName());
    ASSERT_NE(It, Data.end()) << "build " << I;
    ASSERT_EQ(It->second.size(), 1u) << "build " << I << ": one colliding id";
    const auto *S = static_cast<const ParameterEscapeSummary *>(
        It->second.begin()->second.get());

    // Rebuilt from `void cf(int *p)`, the lexically first definition.
    EXPECT_FALSE(S->IsCandidate) << "build " << I;
    EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{0})) << "build " << I;
    ASSERT_EQ(S->Params.count(0), 1u) << "build " << I;
    ASSERT_TRUE(S->Params.at(0).OtherSink.has_value()) << "build " << I;
    EXPECT_EQ(S->Params.at(0).OtherSink->Location.Line, 1u) << "build " << I;

    if (!Expected)
      Expected = *S;
    else
      EXPECT_TRUE(*Expected == *S) << "build " << I << " differs from build 0";
  }
}

// A definition whose entity has only one body keeps the ordinary facts, so the
// degradation above is not simply applied to everything.
TEST_F(ParameterEscapeExtractorTest, SingleDefinitionIsNotDegraded) {
  ASSERT_TRUE(setUp("void solo(int *p) { }"));
  ASSERT_NE(factOf("solo", 0), nullptr);
  EXPECT_TRUE(clean("solo", 0));
  EXPECT_TRUE(summaryOf("solo")->IsCandidate);
}

//===--- Rewritability of every redeclaration -----------------------------===//

// A declaration written through a function typedef has no written parameter
// list: clang synthesizes an implicit ParmVarDecl at the function's own name.
// hasWrittenPrototype() is true (the prototype is written, in the typedef) and
// the location is neither invalid nor a macro, so only isImplicit() catches it.
TEST_F(ParameterEscapeExtractorTest, TypedefWrittenRedeclIsNotCandidate) {
  ASSERT_TRUE(setUp("typedef void FT(int *p);\nFT ft;\nvoid ft(int *p) { }"));
  const ParameterEscapeSummary *S = summaryOf("ft");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{0}));
}

TEST_F(ParameterEscapeExtractorTest, TypedefWrittenRedeclIsNotCandidateInC) {
  ASSERT_TRUE(setUp("typedef void FT(int *p);\nFT ft;\nvoid ft(int *p) { }",
                    {"-x", "c", "-std=c17"}, /*WithPrelude=*/false));
  ASSERT_NE(summaryOf("ft"), nullptr);
  EXPECT_FALSE(summaryOf("ft")->IsCandidate);
}

// In a naked function the body is inline asm reaching the incoming registers
// directly, without operands, so the classifier sees no use of a parameter the
// asm nonetheless captures.
TEST_F(ParameterEscapeExtractorTest, NakedDefinitionIsNotCandidate) {
  ASSERT_TRUE(setUp("__attribute__((naked)) void nk(int *p) "
                    "{ __asm__ volatile(\"ret\"); }"));
  const ParameterEscapeSummary *S = summaryOf("nk");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->IsCandidate);
}

//===--- Candidate and analyzed parameter types ---------------------------===//

TEST_F(ParameterEscapeExtractorTest, CandidateTypesExcludeCallablesAndOwners) {
  ASSERT_TRUE(setUp("void f(void (*fp)(int), Owner o, SwiftView sv, "
                    "NonTrivialView nv, int *const *pp) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  // `sv` was index 2 while view types were tracked; only the object pointer
  // is left.
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{4}));
  // None of the excluded types is pointer-carrying either, so none is even
  // analyzed.
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{4}));
}

// The inverse of what this test asserted while view types were tracked: a
// view-typed parameter is now neither analyzed nor a candidate, whether its
// copy constructor is trivial (`View`) or not (`CopyCtorView`). Nothing is
// claimed about either, which is what makes dropping the tracking reject-only.
TEST_F(ParameterEscapeExtractorTest, ViewTypedParametersAreNeitherAnalyzedNorCandidates) {
  ASSERT_TRUE(setUp("void f(CopyCtorView v, View w, int *keep) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{2}));
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{2}));
}

TEST_F(ParameterEscapeExtractorTest, BlockPointerIsAnalyzedButNotCandidate) {
  ASSERT_TRUE(setUp("void f(void (^blk)(void), int *p) { }",
                    {"-std=c++20", "-fblocks"}));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{1}));
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{0, 1}));
}

// M1 excludes Objective-C. An ObjC object pointer is not a PointerType, so
// isCandidateParameterType rejects it while isPointerCarryingType (which uses
// isAnyPointerType) still analyzes it. Without this test, "making the two
// consistent" by switching isCandidateParameterType to isAnyPointerType leaves
// the whole suite green while making every `id`/`NSFoo *` annotatable.
TEST_F(ParameterEscapeExtractorTest, ObjCPointerIsAnalyzedButNotCandidate) {
  ASSERT_TRUE(setUp("@interface NSString @end\n"
                    "void f(NSString *s, id o, int *p) { }",
                    {"-x", "objective-c++", "-std=c++20"},
                    /*WithPrelude=*/false));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{2}));
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{0, 1, 2}));
}

TEST_F(ParameterEscapeExtractorTest,
       FunctionReferenceIsAnalyzedButNotCandidate) {
  ASSERT_TRUE(setUp("void f(void (&fr)(int), void (*fp)(int), int *p) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{2}));
  // A reference is pointer-carrying whatever it refers to; a function pointer
  // is not pointer-carrying at all.
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{0, 2}));
}

TEST_F(ParameterEscapeExtractorTest,
       NonPointerParametersAreNeitherAnalyzedNorCandidates) {
  ASSERT_TRUE(setUp("struct Plain { int a; }; void f(int n, double d, Plain s, "
                    "void (Holder::*mp)(), int *p) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{4}));
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{4}));
}

//===--- View types are ordinary records ----------------------------------===//

// M1 tracked `[[gsl::Pointer]]` / `swift_attr("~Escapable")` records
// field-sensitively and no longer does. Every by-value shape below was a
// reported missed sink under that tracking -- a handle in a base, in a nested
// aggregate, in a reference member under either spelling, behind `_Atomic`,
// or reached through `*this`, `this[0]`, a laundered `void *` or a callee
// taking `V *`. None of them can claim anything now.
//
// A reference to a view is refused with it -- `V &`, `const V &` and `V &&`
// alike -- so that dropping the tracking stays reject-only. Asserted on
// CandidateParams as well as on the facts, because that is where the
// difference would be an emitted annotation rather than an internal detail.
TEST_F(ParameterEscapeExtractorTest, ViewTypedParametersDropOutByValueAndByReference) {
  ASSERT_TRUE(setUp(R"cpp(
    struct PBase { int *d; };
    struct [[gsl::Pointer(int)]] Derived : PBase { };
    struct Inner { int n; int *q; int &r; };
    struct [[gsl::Pointer(int)]] NestedV { Inner in; };
    struct [[gsl::Pointer(int)]] RefV { int &r; };
    struct __attribute__((swift_attr("~Escapable"))) SwiftRefV { int &r; };
    struct [[gsl::Pointer(int)]] AtomV { _Atomic(int *) a; };
    struct [[gsl::Pointer(int)]] Flat { int *p; };
    int *g_int;
    void base(Derived v, int *keep)     { g_int = v.d; (void)keep; }
    void nested(NestedV v, int *keep)   { g_int = v.in.q; (void)keep; }
    void ref(RefV v, int *keep)         { g_int = &v.r; (void)keep; }
    void swift_ref(SwiftRefV v, int *keep) { g_int = &v.r; (void)keep; }
    void atom(AtomV v, int *keep)       { g_int = v.a; (void)keep; }
    void flat(Flat v, int *keep)        { g_int = v.p; (void)keep; }
    void flat_ref(Flat &v, int *keep)   { g_int = v.p; (void)keep; }
    void flat_cref(const Flat &v, int *keep) { g_int = v.p; (void)keep; }
    void flat_rref(Flat &&v, int *keep) { g_int = v.p; (void)keep; }
    void plain_ref(int &n, int *keep)   { g_int = &n; (void)keep; }
    void swift_ref_param(SwiftRefV &v, int *keep) { g_int = &v.r; (void)keep; }
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  for (const char *Fn : {"base", "nested", "ref", "swift_ref", "atom", "flat"}) {
    const ParameterEscapeSummary *S = summaryOf(Fn);
    ASSERT_NE(S, nullptr) << Fn;
    EXPECT_EQ(analyzedParamsOf(Fn), (std::set<unsigned>{1})) << Fn;
    EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{1})) << Fn;
  }
  // All three reference spellings are refused with the view: no fact, and no
  // annotation emitted. Reading a field through one is a load that design
  // section 1.1 makes fresh, so tracking or not decides what is *claimed*
  // here, and M1 claims nothing while the byte-versus-pointer question is open.
  // Both spellings of "view", by reference as well as by value.
  for (const char *Fn :
       {"flat_ref", "flat_cref", "flat_rref", "swift_ref_param"}) {
    const ParameterEscapeSummary *R = summaryOf(Fn);
    ASSERT_NE(R, nullptr) << Fn;
    EXPECT_EQ(analyzedParamsOf(Fn), (std::set<unsigned>{1})) << Fn;
    EXPECT_EQ(R->CandidateParams, (std::set<unsigned>{1})) << Fn;
  }
  // A reference to anything else is untouched, so the refusal cannot be read
  // as "references dropped out".
  const ParameterEscapeSummary *P = summaryOf("plain_ref");
  ASSERT_NE(P, nullptr);
  EXPECT_EQ(analyzedParamsOf("plain_ref"), (std::set<unsigned>{0, 1}));
  EXPECT_EQ(P->CandidateParams, (std::set<unsigned>{0, 1}));
}

// A pointer to a view keeps its fact -- refusing it from the analysis
// population would stop its uses being classified rather than refuse them, and
// callers read those facts -- but it leaves candidacy, so M1 emits no
// annotation for a view shape while the byte-versus-pointer question is open.
TEST_F(ParameterEscapeExtractorTest, PointersToViewsAreAnalyzedButNotCandidates) {
  ASSERT_TRUE(setUp(R"cpp(
    struct [[gsl::Pointer(int)]] Flat { int *p; };
    struct __attribute__((swift_attr("~Escapable"))) SwiftFlat { int *p; };
    struct Plain { int *p; };
    int *g_int;
    void gsl_ptr(Flat *v, int *keep)        { g_int = v->p; (void)keep; }
    void gsl_cptr(const Flat *v, int *keep) { g_int = v->p; (void)keep; }
    void swift_ptr(SwiftFlat *v, int *keep) { g_int = v->p; (void)keep; }
    void plain_ptr(Plain *v, int *keep)     { g_int = v->p; (void)keep; }
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  for (const char *Fn : {"gsl_ptr", "gsl_cptr", "swift_ptr"}) {
    const ParameterEscapeSummary *S = summaryOf(Fn);
    ASSERT_NE(S, nullptr) << Fn;
    // Still analyzed, and still answered: reading a field through it is a load
    // that design section 1.1 makes fresh.
    EXPECT_EQ(analyzedParamsOf(Fn), (std::set<unsigned>{0, 1})) << Fn;
    EXPECT_TRUE(clean(Fn, 0)) << Fn;
    // Not annotated.
    EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{1})) << Fn;
  }
  // A pointer to any other record is untouched, so this cannot be read as
  // "pointers to records dropped out".
  const ParameterEscapeSummary *P = summaryOf("plain_ptr");
  ASSERT_NE(P, nullptr);
  EXPECT_EQ(analyzedParamsOf("plain_ptr"), (std::set<unsigned>{0, 1}));
  EXPECT_EQ(P->CandidateParams, (std::set<unsigned>{0, 1}));
}

// Refusing a type is reject-only only while every use of it still reaches a
// rule. An argument matched to a callee parameter outside the analyzed
// population has no node to flow into, and an edge recorded against one cannot
// be resolved -- the escape the callee records would be lost with it. Measured
// against the same shape on an ordinary record, which keeps its edge.
TEST_F(ParameterEscapeExtractorTest, ArgumentsToUnanalyzedParametersSink) {
  ASSERT_TRUE(setUp("struct [[gsl::Pointer(int)]] V { int *p; };\n"
                    "struct R { int *p; };\n"
                    "V *g_pv; R *g_r;\n"
                    "void keeps_v(V &v) { g_pv = &v; }\n"
                    "void keeps_r(R &r) { g_r = &r; }\n"
                    "void via_view(V *pv) { keeps_v(*pv); }\n"
                    "void via_record(R *pr) { keeps_r(*pr); }\n",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  // The record keeps its edge, and the callee's own fact reports the escape --
  // `g_r = &r` is an ordinary pointer escape, not a byte question.
  EXPECT_TRUE(flowsTo("via_record", 0, "keeps_r", 0));
  EXPECT_EQ(sinkOf("via_record", 0), std::nullopt);
  EXPECT_EQ(sinkOf("keeps_r", 0), EscapeReason::StoreToGlobal);
  // The view reference has no node, so the same use sinks instead of flowing.
  const EscapeFact *F = factOf("via_view", 0);
  ASSERT_TRUE(F && F->OtherSink);
  EXPECT_EQ(F->OtherSink->Reason, EscapeReason::UnrecognizedUse);
  EXPECT_EQ(F->OtherSink->Detail,
            "callee parameter outside the analyzed population");
  EXPECT_TRUE(F->FlowsTo.empty());
}

// The property that makes dropping the tracking safe: an alias that reaches a
// view object still escapes, by the rules that cover every other record.
TEST_F(ParameterEscapeExtractorTest, AliasesReachingAViewStillSink) {
  ASSERT_TRUE(setUp(R"cpp(
    struct [[gsl::Pointer(int)]] Flat { int *h; };
    struct [[gsl::Pointer(int)]] Made { int *h; Made(int *q); };
    Made::Made(int *q) : h(q) { }
    Flat g_flat;
    void store(int *p)  { Flat f; f.h = p; (void)f; }
    void global(int *p) { g_flat.h = p; }
    void init(int *p)   { Flat f = {p}; (void)f; }
    void ctor(int *p)   { Made m(p); (void)m; }
    void capture(int *p) { auto l = [q = Flat{p}] { return q.h; }; (void)l; }
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("store", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("global", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("init", 0), EscapeReason::StoreToField);
  // Building the view is itself a store into its field, which is reached
  // before the capture is -- either way the alias does not get away.
  EXPECT_EQ(sinkOf("capture", 0), EscapeReason::StoreToField);
  // A constructor is an ordinary flow target, and its own fact reports the
  // store -- so the escape is still expressed, one node along.
  const CXXConstructorDecl *CD = ctorOf("Made", 1);
  ASSERT_NE(CD, nullptr);
  EXPECT_TRUE(flowsToDecl(factOf("ctor", 0), CD, 0));
  EXPECT_EQ(sinkOfDecl(CD, 0), EscapeReason::StoreToField);
}

//===--- `this` -----------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ThisFactOnlyForInstanceMethods) {
  ASSERT_TRUE(
      setUp("struct S { void inst(int *p) { } static void stat(int *p) { } };\n"
            "void freefn(int *p) { }"));
  EXPECT_NE(thisFactOf("inst"), nullptr);
  EXPECT_EQ(thisFactOf("stat"), nullptr);
  EXPECT_EQ(thisFactOf("freefn"), nullptr);
}

//===--- A definition that uses nothing -----------------------------------===//

// Every analyzed parameter carries a fact whether or not it escapes; an unused
// one carries the clean fact, not an absent entry. CandidateParams is a subset
// of the fact keys (asserted in the extractor), so a missing clean fact would
// trip that assert -- and a candidate's whole summary could be dropped as
// empty().
TEST_F(ParameterEscapeExtractorTest, UnusedParametersAndThisAreClean) {
  ASSERT_TRUE(setUp("struct [[gsl::Pointer(int)]] View { int *d; };\n"
                    "struct S { void g(int *p, View v, int &r, int n) { } };",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const ParameterEscapeSummary *S = summaryOf("g");
  ASSERT_NE(S, nullptr);
  EXPECT_TRUE(S->IsCandidate);
  // `v` is a view type, which M1 does not track.
  EXPECT_EQ(analyzedParamsOf("g"), (std::set<unsigned>{0, 2}));
  for (unsigned I : {0u, 2u}) {
    ASSERT_NE(factOf("g", I), nullptr) << "index " << I;
    EXPECT_TRUE(clean("g", I)) << "index " << I;
  }
  EXPECT_TRUE(isClean(thisFactOf("g")));
}

TEST_F(ParameterEscapeExtractorTest, CoroutineThisIsSunkAsCoroutine) {
  tooling::FileContentMappings Files = {
      {"/inc/std-coroutine.h", CoroutineHeader}};
  ASSERT_TRUE(
      setUp("#include <std-coroutine.h>\n"
            "struct task { struct promise_type { task get_return_object(); "
            "std::suspend_never initial_suspend(); std::suspend_never "
            "final_suspend() noexcept; "
            "void return_void(); void unhandled_exception(); }; };\n"
            "struct C { task co(int *p) { co_return; } };",
            {"-std=c++20", "-I/inc"}, true, Files));
  EXPECT_EQ(sinkOf("co", 0), EscapeReason::Coroutine);
  const EscapeFact *F = thisFactOf("co");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Reason, EscapeReason::Coroutine);
}

//===--- Definition-level rules the extractor can never reach -------------===//

// isCandidateDefinition is called by the extractor only on a definition, so
// these two rules have to be exercised on the predicate directly.

// The degradation rebuilds the summary from the definition rather than editing
// whatever the builder happened to retain, so nothing is carried over. The
// stub produces neither FlowsTo edges nor ReturnsSelfAt, so a summary holding
// them can only be built by hand.
TEST_F(ParameterEscapeExtractorTest,
       DegradeToMultipleDefinitionsRebuildsFromTheDefinition) {
  ASSERT_TRUE(setUp("struct H { void host(int *p, int n, View v) { } };\n"
                    "void other(int *q) { }"));
  const FunctionDecl *Host = findFnByName("host", AST->getASTContext());
  const FunctionDecl *Other = findFnByName("other", AST->getASTContext());
  ASSERT_NE(Host, nullptr);
  ASSERT_NE(Other, nullptr);
  std::optional<EntityId> Callee = Extractor->addEntity(Other);
  ASSERT_TRUE(Callee.has_value());

  // A summary describing some other body, with every field populated and a
  // node index that `host` does not have.
  ParameterEscapeSummary S;
  S.IsCandidate = true;
  S.CandidateParams = {7};
  EscapeFact Dirty;
  Dirty.FlowsTo[FlowTarget{*Callee, 0}] = SourceLocationRecord{"a.cc", 7, 3};
  Dirty.ReturnsSelfAt = SourceLocationRecord{"a.cc", 9, 5};
  S.Params[7] = Dirty;
  S.This = Dirty;

  degradeToMultipleDefinitions(S, Host, AST->getASTContext());

  EXPECT_FALSE(S.IsCandidate);
  // Rebuilt from `host`: index 7 is gone, and 0 is its only pointer-carrying
  // parameter. `n` is not analyzed, and neither is the view-typed `v`.
  EXPECT_EQ(S.CandidateParams, (std::set<unsigned>{0}));
  std::set<unsigned> Keys;
  for (const auto &[Index, Fact] : S.Params)
    Keys.insert(Index);
  EXPECT_EQ(Keys, (std::set<unsigned>{0}));
  ASSERT_TRUE(S.This.has_value()) << "host is an instance method";
  for (const EscapeFact *F : {&S.Params.at(0), &*S.This}) {
    EXPECT_TRUE(F->FlowsTo.empty());
    EXPECT_FALSE(F->returnsSelf());
    ASSERT_TRUE(F->OtherSink.has_value());
    EXPECT_EQ(F->OtherSink->Reason, EscapeReason::UnrecognizedUse);
    EXPECT_EQ(F->OtherSink->Detail, MultipleDefinitionsDetail.str());
  }

  // Degrading the same summary again, now against a free function: `this`
  // has to go away rather than survive from the previous shape.
  degradeToMultipleDefinitions(S, Other, AST->getASTContext());
  EXPECT_FALSE(S.This.has_value());
  EXPECT_EQ(S.CandidateParams, (std::set<unsigned>{0}));
}

TEST_F(ParameterEscapeExtractorTest, DeductionGuideIsNotACandidateDefinition) {
  ASSERT_TRUE(setUp("template <class T> struct Wrap { Wrap(T *p); };\n"
                    "Wrap(int *) -> Wrap<int>;\n"));
  const CXXDeductionGuideDecl *Guide = nullptr;
  for (const Decl *D : AST->getASTContext().getTranslationUnitDecl()->decls())
    if (const auto *G = dyn_cast<CXXDeductionGuideDecl>(D);
        G && !G->isImplicit())
      Guide = G;
  ASSERT_NE(Guide, nullptr) << "no explicit deduction guide in the test input";
  EXPECT_FALSE(isCandidateDefinition(Guide, AST->getASTContext()));
}

TEST_F(ParameterEscapeExtractorTest,
       BodilessDeclarationIsNotACandidateDefinition) {
  ASSERT_TRUE(setUp("void decl_only(int *p);"));
  const FunctionDecl *FD = findFnByName("decl_only", AST->getASTContext());
  ASSERT_NE(FD, nullptr);
  ASSERT_FALSE(FD->doesThisDeclarationHaveABody());
  EXPECT_FALSE(isCandidateDefinition(FD, AST->getASTContext()));
}

//===--- Candidate parameters are never dropped with an "empty" summary ---===//

// ParameterEscapeSummary::empty() looks only at the escape facts, and
// extractAndAddSummaries drops any summary whose empty() is true. That is only
// safe because every candidate parameter also carries a fact. Pin the
// invariant on a definition that mixes candidate, analyzed-only and ignored
// parameter types.
TEST_F(ParameterEscapeExtractorTest, EveryCandidateParameterCarriesAFact) {
  ASSERT_TRUE(setUp("void f(int *p, int n, View v, int &r, void (*fp)(int), "
                    "Owner o) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_FALSE(S->CandidateParams.empty());
  for (unsigned I : S->CandidateParams)
    EXPECT_EQ(S->Params.count(I), 1u)
        << "candidate parameter " << I << " has no escape fact, so a summary "
        << "holding only it would be dropped as empty()";
  EXPECT_FALSE(S->empty());
}

//===--- Benign uses ------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ReadsComparesAndBoolAreBenign) {
  ASSERT_TRUE(setUp("int f(int *p, int *q, int &r) { int x = *p + p[1] + r; if "
                    "(p && p == q) x++; return x + (int)sizeof(*p); }"));
  EXPECT_TRUE(clean("f", 0));
  EXPECT_TRUE(clean("f", 1));
  EXPECT_TRUE(clean("f", 2));
}

TEST_F(ParameterEscapeExtractorTest, WritesThroughAliasAreBenign) {
  ASSERT_TRUE(setUp("struct S { int m; int *n; }; void f(S *s, int *p, int &r) "
                    "{ *p = 1; p[2] = 3; s->m = 4; r = 5; s->n = g_ptr; }"));
  EXPECT_TRUE(clean("f", 0));
  EXPECT_TRUE(clean("f", 1));
  EXPECT_TRUE(clean("f", 2));
}

// The central precision rule, and the one that makes the whole analysis
// tractable: the value loaded out of the pointee is a different object's
// address, not the parameter's own. Storing it anywhere says nothing about
// whether the parameter outlives the call (design section 1.1).
TEST_F(ParameterEscapeExtractorTest, LoadsThroughNonViewAliasAreFresh) {
  ASSERT_TRUE(setUp("struct N { N *next; int *v; };\n"
                    "void f(N *n) { g_ptr = n->v; N *m = n->next; g_ptr = m->v; }\n"
                    "void h(N *n) { N c = *n; g_ptr = c.v; }"));
  EXPECT_TRUE(clean("f", 0));
  // Copying the pointee by value through a trivial implicit copy constructor
  // is a load of its bytes, not a use of the pointer that designates it.
  EXPECT_TRUE(clean("h", 0));
}

// `r` receives the alias from `q` before `q` receives it from `p`, so one pass
// over the body leaves `r` out of the alias set and the store below unseen.
TEST_F(ParameterEscapeExtractorTest, AliasGrowthReachesAFixpoint) {
  ASSERT_TRUE(setUp("void f(int *p) { int *q = nullptr; int *r = nullptr; r = "
                    "q; q = p; g_ptr = r; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, DeclaredNoescapeCalleeIsBenign) {
  ASSERT_TRUE(setUp("void f(int *p) { noesc(p); }"));
  EXPECT_TRUE(clean("f", 0));
}

TEST_F(ParameterEscapeExtractorTest, LibraryKnowledgeIsBenign) {
  ASSERT_TRUE(setUp("size_t f(const char *s, char *d) { memcpy(d, s, 1); "
                    "return strlen(s); }"));
  EXPECT_TRUE(clean("f", 0));
  // memcpy's destination is only 'returned' in LLVM's model, never
  // `captures(none)`, so the table does not make it benign. It does not become
  // an ordinary flow either: getEntityName() refuses every FunctionDecl that
  // carries a builtin id, so no library function can be named as a flow
  // target, and an unnameable callee has to sink rather than lose the use.
  EXPECT_EQ(sinkOf("f", 1), EscapeReason::UnnamedCallee);
}

// The table is only a fact where clang would actually treat the callee as the
// library function. `no_builtin` is written on the *caller*, so the callee-only
// signature of LibraryFunctionKnowledge cannot see it; honoring it is this call
// site's documented obligation.
TEST_F(ParameterEscapeExtractorTest, NoBuiltinRefusesLibraryKnowledge) {
  ASSERT_TRUE(setUp(
      "size_t plain(const char *s) { return strlen(s); }\n"
      "__attribute__((no_builtin(\"strlen\"))) size_t named(const char *s) { "
      "return strlen(s); }\n"
      "__attribute__((no_builtin)) size_t all(const char *s) { return "
      "strlen(s); }"));
  EXPECT_TRUE(clean("plain", 0));
  // Not benign any more, and not a flow either: a builtin cannot be named
  // (see LibraryKnowledgeIsBenign), so refusing the table's answer leaves an
  // unnameable callee, which sinks.
  EXPECT_EQ(sinkOf("named", 0), EscapeReason::UnnamedCallee);
  EXPECT_EQ(sinkOf("all", 0), EscapeReason::UnnamedCallee);
}

// Assigning an alias into a local automatic pointer is benign: the variable
// joins the alias set and its own uses are classified instead.
TEST_F(ParameterEscapeExtractorTest, StoringIntoALocalVariableIsBenign) {
  ASSERT_TRUE(setUp("void f(int *p) { int *q = nullptr; q = p; (void)q; }"));
  EXPECT_TRUE(clean("f", 0));
}

TEST_F(ParameterEscapeExtractorTest, LocalScalarCopiesJoinAliasSet) {
  ASSERT_TRUE(setUp("void f(int *p) { int *q = p; int *r; r = q + 1; int &x = "
                    "*r; g_ptr = &x; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, DiscardedValuesAreBenign) {
  ASSERT_TRUE(setUp("void f(int *p) { p; (void)p; for (int *q = p; q; ++q) {} }"));
  EXPECT_TRUE(clean("f", 0));
}

TEST_F(ParameterEscapeExtractorTest, DiscardedOperandsAndDifferencesAreBenign) {
  ASSERT_TRUE(setUp("long f(int *p, int *q) { int x = (p, 0); bool n = "
                    "noexcept(*p); return (p - q) + x + n + (long)sizeof(*p); }"));
  EXPECT_TRUE(clean("f", 0));
  EXPECT_TRUE(clean("f", 1));
}

// C has no CK_PointerToBoolean cast in a boolean context, so the alias is the
// direct operand of `!`, `&&`, `||` and `?:` there. Without rows for those,
// `if (!p) return;` sinks -- and almost no C function could be annotated.
TEST_F(ParameterEscapeExtractorTest, BooleanContextsAreBenignInC) {
  ASSERT_TRUE(setUp("void nots(int *p) { if (!p) return; }\n"
                    "void ands(int *p, int *q) { if (p && q) return; }\n"
                    "void ors(int *p) { if (p || 1) return; }\n"
                    "void cond(int *p) { int x = p ? 1 : 2; (void)x; }\n",
                    {"-x", "c", "-std=c17"}, /*WithPrelude=*/false));
  EXPECT_TRUE(clean("nots", 0));
  EXPECT_TRUE(clean("ands", 0));
  EXPECT_TRUE(clean("ands", 1));
  EXPECT_TRUE(clean("ors", 0));
  EXPECT_TRUE(clean("cond", 0));
}

// A braced initializer for a scalar carries the value through: `int *q = {p}`
// is `int *q = p`, and the variable joins the alias set instead of the
// initializer list reading as a store into an aggregate's field.
TEST_F(ParameterEscapeExtractorTest, BracedScalarInitializersPropagate) {
  ASSERT_TRUE(setUp("struct A { int *m; };\n"
                    "void take(int *);\n"
                    "void f(int *p) { int *q = {p}; (void)q; }\n"
                    "void g(int *p) { int *q = {p}; g_ptr = q; }\n"
                    "void h(int *p) { A a = {p}; (void)a; }\n"
                    "void i(int *p) { A a = { {p} }; (void)a; }\n"
                    "void j(int *p) { take({p}); }\n"
                    "int *k(int *p) { return {p}; }\n"
                    "void l(int *p) { static int *q = {p}; (void)q; }"));
  EXPECT_TRUE(clean("f", 0));
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::StoreToGlobal);
  // An aggregate's initializer list still stores into its fields, whether the
  // element is braced or not.
  EXPECT_EQ(sinkOf("h", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("i", 0), EscapeReason::StoreToField);
  // Transparent everywhere else the list can appear, too.
  EXPECT_TRUE(flowsTo("j", 0, "take", 0));
  EXPECT_EQ(sinkOf("j", 0), std::nullopt);
  EXPECT_TRUE(factOf("k", 0)->returnsSelf());
  EXPECT_EQ(sinkOf("l", 0), EscapeReason::StoreToGlobal);
}

// clang models std::move and std::forward as builtins, which the entity model
// refuses to name, so recording a flow would degrade to UnnamedCallee and no
// parameter that is ever moved could be annotated. They re-type their operand
// and nothing else, so the result is the alias and the argument's use is not an
// escape of its own.
TEST_F(ParameterEscapeExtractorTest, StdReferenceCastsPassThrough) {
  ASSERT_TRUE(setUp(
      "namespace std {\n"
      "template <class T> struct remove_reference { typedef T type; };\n"
      "template <class T> struct remove_reference<T &> { typedef T type; };\n"
      "template <class T> typename remove_reference<T>::type &&move(T &&t) "
      "noexcept;\n"
      "}\n"
      "void discard(int *p) { (void)std::move(p); }\n"
      "void store(int *p) { g_ptr = std::move(p); }\n"
      "void pass(int *p) { unknown(std::move(p)); }"));
  EXPECT_TRUE(clean("discard", 0));
  EXPECT_EQ(sinkOf("store", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(flowsTo("pass", 0, "unknown", 0));
  EXPECT_EQ(sinkOf("pass", 0), std::nullopt);
}

// The row above is a third trusted external source, and clang hands out those
// builtin ids on a shape test -- namespace, name, one parameter -- without ever
// looking at a body. The library table refuses a callee it can see a definition
// for ("definitions are analyzed, never trusted by name"); match that, except
// for the standard library's own definitions, which live in system headers and
// are the whole point of the row.
TEST_F(ParameterEscapeExtractorTest, StdReferenceCastsWithABodyAreNotTrusted) {
  ASSERT_TRUE(setUp(
      "namespace std {\n"
      "template <class T> struct remove_reference { typedef T type; };\n"
      "template <class T> struct remove_reference<T &> { typedef T type; };\n"
      "template <class T> typename remove_reference<T>::type &&move(T &&t) "
      "noexcept { g_ptr = (int *)t; return static_cast<typename "
      "remove_reference<T>::type &&>(t); }\n"
      "}\n"
      "void pass(int *p) { unknown(std::move(p)); }"));
  EXPECT_EQ(sinkOf("pass", 0), EscapeReason::UnnamedCallee);
}

// A labelled or attributed statement wraps a statement without changing what
// happens to its value; outside a statement expression that value is discarded.
TEST_F(ParameterEscapeExtractorTest, LabelledAndAttributedStatementsDiscard) {
  ASSERT_TRUE(setUp("void f(int *p, int c) { lbl: p; if (c) [[likely]] p; "
                    "(void)c; }"));
  EXPECT_TRUE(clean("f", 0));
}

//===--- Flows ------------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ArgumentsFlowToCalleeParameters) {
  ASSERT_TRUE(setUp("void callee(int *a, int *b); void f(int *p, int &r) { "
                    "callee(p, &r); }"));
  EXPECT_TRUE(flowsTo("f", 0, "callee", 0));
  EXPECT_TRUE(flowsTo("f", 1, "callee", 1));
  EXPECT_EQ(sinkOf("f", 0), std::nullopt);
}

TEST_F(ParameterEscapeExtractorTest, ImplicitObjectFlowsToThis) {
  ASSERT_TRUE(setUp("void f(Owner *o, int *p) { o->push(p); }"));
  EXPECT_TRUE(flowsTo("f", 0, "push", ThisParamIndex));
  EXPECT_TRUE(flowsTo("f", 1, "push", 0));
}

TEST_F(ParameterEscapeExtractorTest,
       CallResultsAliasArgumentsAndArgumentsStillFlow) {
  ASSERT_TRUE(setUp("int *id(int *x); void f(int *p) { int *q = id(p); g_ptr = "
                    "q; } void g(int *p) { (void)id(p); }"));
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

// One edge per target, carrying the *first* call site.
TEST_F(ParameterEscapeExtractorTest, FlowRecordsTheFirstCallSite) {
  // No prelude, so the location is absolute: the first argument is on line 3
  // at column 10. Line and column differ, so a transposition cannot hide.
  ASSERT_TRUE(setUp("void callee(int *a);\n"
                    "void f(int *p) {\n"
                    "  callee(p);\n"
                    "      callee(p);\n"
                    "}\n",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const EscapeFact *F = factOf("f", 0);
  ASSERT_TRUE(F && F->FlowsTo.size() == 1);
  const SourceLocationRecord &L = F->FlowsTo.begin()->second;
  EXPECT_EQ(L.FilePath, "input.cc");
  EXPECT_EQ(L.Line, 3u);
  EXPECT_EQ(L.Column, 10u);
}

//===--- Return -----------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ReturnIsRecorded) {
  ASSERT_TRUE(setUp("int *f(int *p) { return p; } int &g(int *p) { return *p; "
                    "} int h(int *p) { return *p; }"));
  EXPECT_TRUE(factOf("f", 0)->returnsSelf());
  EXPECT_TRUE(factOf("g", 0)->returnsSelf());
  EXPECT_FALSE(factOf("h", 0)->returnsSelf());
}

//===--- Sinks ------------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, StoreSinks) {
  ASSERT_TRUE(setUp(R"cpp(
    struct S { int *m; };
    void to_global(int *p) { g_ptr = p; }
    void to_static(int *p) { s_ptr = p; }
    void to_static_local(int *p) { static int *l; l = p; }
    void to_static_local_init(int *p) { static int *l = p; (void)l; }
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
  EXPECT_EQ(sinkOf("to_static_local_init", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("to_thread_local", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("to_field", 1), EscapeReason::StoreToField);
  EXPECT_TRUE(clean("to_field", 0));
  EXPECT_EQ(sinkOf("to_local_aggregate", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("to_init_list", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("through_pointer", 1), EscapeReason::StoreThroughPointer);
  EXPECT_TRUE(clean("through_pointer", 0));
  EXPECT_EQ(sinkOf("through_subscript", 1), EscapeReason::StoreThroughPointer);
  EXPECT_EQ(sinkOf("through_ref_param", 1), EscapeReason::StoreThroughPointer);
  // Storing *into* the referent of a reference parameter does not leak the
  // reference itself.
  EXPECT_TRUE(clean("through_ref_param", 0));
  EXPECT_EQ(sinkOf("through_local_ref", 0), EscapeReason::StoreThroughPointer);
  EXPECT_EQ(sinkOf("self_store", 0), EscapeReason::StoreThroughPointer);
}

TEST_F(ParameterEscapeExtractorTest, AggregateInitializersStoreToFields) {
  ASSERT_TRUE(setUp("struct A { int *m; };\n"
                    "void braced(int *p) { A a{p}; (void)a; }\n"
                    "void parens(int *p) { A a(p); (void)a; }"));
  EXPECT_EQ(sinkOf("braced", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("parens", 0), EscapeReason::StoreToField);
}

TEST_F(ParameterEscapeExtractorTest, DesignatedInitializersStoreToFields) {
  ASSERT_TRUE(setUp("struct A { int *m; };\n"
                    "void f(int *p) { struct A a = { .m = p }; (void)a; }",
                    {"-x", "c", "-std=c17"}, /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToField);
}

TEST_F(ParameterEscapeExtractorTest, AddressAndReferenceToTheVariable) {
  ASSERT_TRUE(setUp("void take(int **); void f(int *p) { take(&p); } void "
                    "g(int *p) { int *&r = p; (void)r; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::AddressTaken);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::AddressTaken);
}

TEST_F(ParameterEscapeExtractorTest, ArrayMemberDecayIsAnInteriorPointer) {
  ASSERT_TRUE(setUp("struct A { int a[4]; };\n"
                    "void f(A *s) { g_ptr = s->a; }\n"
                    "void w(A *s) { *(s->a) = 1; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  // The decayed array is a pointer *value*, so dereferencing it lands back on
  // the pointee and writing through it stays benign.
  EXPECT_TRUE(clean("w", 0));
}

TEST_F(ParameterEscapeExtractorTest, StatementExpressionPropagates) {
  ASSERT_TRUE(setUp("void f(int *p) { g_ptr = ({ p; }); }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

// clang gives a statement expression the value of its last statement even when
// a label or an attribute wraps it. Without unwrapping, the wrapper has no
// alias kind and the wrapped expression reads as a discarded value, so the
// store below is classified by nobody.
TEST_F(ParameterEscapeExtractorTest, StatementExpressionPropagatesThroughALabel) {
  ASSERT_TRUE(setUp("void f(int *p) { g_ptr = ({ lbl: p; }); }\n"
                    "void g(int *p) { g_ptr = ({ int *q = p; q; }); }\n"
                    "void h(int *p) { g_ptr = ({ if (1) ; p; }); }\n"
                    "void o(int *p, int *q) { g_ptr = ({ lbl: q; }); (void)p; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("h", 0), EscapeReason::StoreToGlobal);
  // The value has to be *resolved*, not merely assumed: only `q` is stored, so
  // `p` stays clean. Falling back to "some pointer-carrying value" would sink
  // both.
  EXPECT_TRUE(clean("o", 0));
  EXPECT_EQ(sinkOf("o", 1), EscapeReason::StoreToGlobal);
}

// Both halves of the increment rule, which the alias kind of the *operand*
// decides: incrementing storage that holds an alias yields the alias, while
// incrementing through a place reads the pointee.
TEST_F(ParameterEscapeExtractorTest, IncrementsYieldAnAliasOnlyFromStorage) {
  ASSERT_TRUE(setUp(
      "void f(int *p, int **pp) { (*p)++; ++p[1]; (*pp)++; }\n"
      "void g(int **pp) { g_ptr = (*pp)++; }\n"
      "void h(int *p, int *q, int *r, int *s) { g_ptr = p++; g_ptr = ++q; "
      "g_ptr = r--; g_ptr = --s; }"));
  // A read-modify-write through a place writes the pointee and yields a value
  // loaded from it; neither is the pointer that designates it.
  EXPECT_TRUE(clean("f", 0));
  EXPECT_TRUE(clean("f", 1));
  // `(*pp)++` yields the old *pointee*, which design section 1.1 makes fresh.
  EXPECT_TRUE(clean("g", 0));
  // `p++` yields the parameter's own pointer, which is an alias -- one opcode
  // per parameter, because the rule takes four of them and the classifier's
  // paired row calls an increment benign whenever the operand reaches it.
  for (unsigned I : {0u, 1u, 2u, 3u})
    EXPECT_EQ(sinkOf("h", I), EscapeReason::StoreToGlobal) << "index " << I;
}

// `__extension__ e` is a UnaryOperator wrapping e. Without propagating through
// it the wrapped expression has no alias kind, the alias reaches no use rule of
// its own, and the store below goes unseen.
TEST_F(ParameterEscapeExtractorTest, ExtensionExpressionsPropagate) {
  ASSERT_TRUE(setUp("void f(int *p) { g_ptr = __extension__ p; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, StructuredBindingsResolveThroughTheirBinding) {
  ASSERT_TRUE(setUp("struct P { int x, y; }; void f(P *ps) { auto &[a, b] = "
                    "*ps; g_ptr = &a; } void g(P *ps) { auto &[a, b] = *ps; "
                    "int s = a + b; (void)s; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("g", 0));
}

TEST_F(ParameterEscapeExtractorTest, CleanupVariablesAreNotAliasStorage) {
  ASSERT_TRUE(setUp("void keep(int **);\n"
                    "void f(int *p) { int *q __attribute__((cleanup(keep))) = "
                    "p; (void)q; }\n"
                    "void g(int *p) { int *q __attribute__((cleanup(keep))) = "
                    "g_ptr; q = p; (void)q; }"));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::AddressTaken);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::AddressTaken);
}

// The refusal side of the library table has the opposite polarity to the trust
// side: a configuration that makes clang unsure this declaration is the library
// function must not switch the refusal off. A pool allocator compiled
// -ffreestanding writes a header through the pointer and so reports no escape
// of its own, which is exactly the body that would let a caller be annotated.
TEST_F(ParameterEscapeExtractorTest, DeallocatorsAreRefusedWithoutBuiltins) {
  constexpr const char *Code =
      "typedef __SIZE_TYPE__ size_t;\n"
      "void free(void *p);\n"
      "void *realloc(void *p, size_t n);\n"
      "void rel(int *p) { free(p); }\n"
      "void rea(int *p) { realloc(p, 8); }\n";
  for (std::vector<std::string> Args :
       {std::vector<std::string>{"-x", "c", "-std=c17"},
        {"-x", "c", "-std=c17", "-ffreestanding"},
        {"-x", "c", "-std=c17", "-fno-builtin"},
        {"-x", "c", "-std=c17", "-fno-builtin-free"}}) {
    SCOPED_TRACE(Args.back());
    ASSERT_TRUE(setUp(Code, Args, /*WithPrelude=*/false));
    EXPECT_EQ(sinkOf("rel", 0), EscapeReason::Deallocation);
    EXPECT_EQ(sinkOf("rea", 0), EscapeReason::Deallocation);
  }
  // In C++ a plain `void free(void *)` is not even the C library function by
  // name mangling, and it is refused all the same: whatever it is, this
  // analysis cannot tell a body that frees from one that only writes.
  ASSERT_TRUE(setUp("void free(void *p);\nvoid rel(int *p) { free(p); }\n",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("rel", 0), EscapeReason::Deallocation);
}

TEST_F(ParameterEscapeExtractorTest, CallSinks) {
  ASSERT_TRUE(setUp(R"cpp(
    void v(int, ...);
    void indirect(void (*fp)(int *), int *p) { fp(p); }
    void variadic(int *p) { v(1, p); }
    void dealloc(void *p) { free(p); }
    void del(int *p) { delete p; }
    void op_del(int *p) { ::operator delete(p); }
    void heap(int *p) { new Holder(p); }
    void heap_scalar(int *p) { int **q = new int *(p); (void)q; }
    struct B { virtual void m(int *); };
    void virt(B *b, int *p) { b->m(p); }
  )cpp"));
  EXPECT_EQ(sinkOf("indirect", 1), EscapeReason::IndirectCall);
  EXPECT_EQ(sinkOf("variadic", 0), EscapeReason::VarArgs);
  EXPECT_EQ(sinkOf("dealloc", 0), EscapeReason::Deallocation);
  EXPECT_EQ(sinkOf("del", 0), EscapeReason::Deallocation);
  EXPECT_EQ(sinkOf("op_del", 0), EscapeReason::Deallocation);
  EXPECT_EQ(sinkOf("heap", 0), EscapeReason::HeapAllocation);
  EXPECT_EQ(sinkOf("heap_scalar", 0), EscapeReason::HeapAllocation);
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
    void lam_init(int *p) { auto l = [q = p] { return *q; }; (void)l; }
  )cpp"));
  EXPECT_EQ(sinkOf("cast", 0), EscapeReason::CastToNonPointer);
  EXPECT_EQ(sinkOf("thr", 0), EscapeReason::Throw);
  EXPECT_EQ(sinkOf("asm_", 0), EscapeReason::Asm);
  EXPECT_EQ(sinkOf("lam", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("lam_ref", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("lam_init", 0), EscapeReason::Capture);
}

// An init-capture's initializer runs in the enclosing function, so an alias
// nested inside one is an ordinary use. Checking only the initializer's own
// alias kind sees nothing when the capture's type is not pointer-carrying.
TEST_F(ParameterEscapeExtractorTest, InitCaptureInitializersAreOrdinaryCode) {
  ASSERT_TRUE(setUp(R"cpp(
    int record(int *x);
    struct Rec { int *m; };
    void call(int *p) { auto l = [n = record(p)] { return n; }; (void)l; }
    void store(int *p) { auto l = [n = (g_ptr = p, 0)] { return n; }; (void)l; }
    void field(int *p) { auto l = [r = Rec{p}] { return r.m; }; (void)l; }
    void convert(int *p) { auto l = [n = (long)p] { return n; }; (void)l; }
    void grow(int *p) { int *q = nullptr; auto l = [n = (q = p, 0)] { return n; }; g_ptr = q; (void)l; }
  )cpp"));
  EXPECT_TRUE(flowsTo("call", 0, "record", 0));
  EXPECT_EQ(sinkOf("store", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("field", 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOf("convert", 0), EscapeReason::CastToNonPointer);
  // The growth pass has to walk the initializer too, or `q` never joins the
  // alias set and the store after the lambda is invisible.
  EXPECT_EQ(sinkOf("grow", 0), EscapeReason::StoreToGlobal);
}

TEST_F(ParameterEscapeExtractorTest, BlockCapturesAndCallableUsesSink) {
  ASSERT_TRUE(setUp("void f(int *p) { void (^b)(void) = ^{ (void)*p; }; (void)b; }\n"
                    "void g(int *p) { __block int *q = p; (void)q; }\n"
                    "void g2(int *p) { __block int *q = g_ptr; q = p; (void)q; }\n"
                    "void h(void (^b)(void)) { b(); }\n"
                    "struct S { int *f;\n"
                    "  void m() { void (^b)(void) = ^{ (void)f; }; (void)b; } };",
                    {"-std=c++20", "-fblocks"}));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("g2", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("h", 0), EscapeReason::CallableUse);
  const EscapeFact *M = thisFactOf("m");
  ASSERT_TRUE(M && M->OtherSink);
  EXPECT_EQ(M->OtherSink->Reason, EscapeReason::Capture);
}

TEST_F(ParameterEscapeExtractorTest, UnrecognizedUseIsTheDefault) {
  // An atomic builtin operating on the alias value is deliberately unmodeled.
  ASSERT_TRUE(setUp("void f(int *p) { __atomic_store_n(&g_ptr, p, "
                    "__ATOMIC_SEQ_CST); }"));
  const EscapeFact *F = factOf("f", 0);
  ASSERT_TRUE(F && F->OtherSink);
  EXPECT_EQ(F->OtherSink->Reason, EscapeReason::UnrecognizedUse);
  // The detail is what distinguishes "the default row fired on this shape"
  // from "the classifier produced nothing".
  EXPECT_EQ(F->OtherSink->Detail, "AtomicExpr");
}

// Statement parents are an allow list: an expression statement, a condition and
// a loop clause discard or read the value, and anything else -- here ObjC fast
// enumeration, which hands the collection to the runtime -- reaches the default
// sink rather than being taken for a discarded value.
TEST_F(ParameterEscapeExtractorTest, ObjCUsesAreSinks) {
  ASSERT_TRUE(setUp("@interface Foo\n- (void)take:(int *)p;\n@end\n"
                    "void msg(Foo *o, int *p) { [o take:p]; }\n"
                    "void iterate(Foo *c) { for (Foo *x in c) { (void)x; } }\n",
                    {"-x", "objective-c++", "-std=c++20"},
                    /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("msg", 0), EscapeReason::ObjCMessage);
  EXPECT_EQ(sinkOf("msg", 1), EscapeReason::ObjCMessage);
  EXPECT_EQ(sinkOf("iterate", 0), EscapeReason::UnrecognizedUse);
}

// An instance variable reached through the parameter is a subobject of the
// object it designates: reading one is a load, writing one writes into the
// pointee, and neither is a use of the pointer itself.
TEST_F(ParameterEscapeExtractorTest, ObjCInstanceVariablesAreSubobjects) {
  ASSERT_TRUE(setUp("int *g_ptr;\n"
                    "@interface T { @public int *ivar; }\n@end\n"
                    "void read(T *t) { g_ptr = t->ivar; }\n"
                    "void write(T *t) { t->ivar = g_ptr; }\n",
                    {"-x", "objective-c++", "-std=c++20"},
                    /*WithPrelude=*/false));
  EXPECT_TRUE(clean("read", 0));
  EXPECT_TRUE(clean("write", 0));
}

// Only the first non-return sink is kept, and it is the first in source order.
TEST_F(ParameterEscapeExtractorTest, FirstSinkOnlyIsRecordedWithLocation) {
  // No prelude, so the location is absolute: the alias is used on line 3 at
  // column 13. Line and column differ, so a transposition cannot hide.
  ASSERT_TRUE(setUp("int *g_ptr;\n"
                    "void f(int *p) {\n"
                    "    g_ptr = p;\n"
                    "  throw p;\n"
                    "}\n",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const EscapeFact *F = factOf("f", 0);
  ASSERT_TRUE(F && F->OtherSink);
  EXPECT_EQ(F->OtherSink->Reason, EscapeReason::StoreToGlobal);
  EXPECT_EQ(F->OtherSink->Detail, "g_ptr");
  EXPECT_EQ(F->OtherSink->Location.FilePath, "input.cc");
  EXPECT_EQ(F->OtherSink->Location.Line, 3u);
  EXPECT_EQ(F->OtherSink->Location.Column, 13u);
}

// An argument with no callee parameter is a variadic tail only when the callee
// is a prototyped variadic function; an unprototyped callee's arguments are
// unmatched for an unrelated reason and must not be blamed on varargs.
TEST_F(ParameterEscapeExtractorTest, UnmatchedArgumentsDistinguishTheVariadicTail) {
  ASSERT_TRUE(setUp("void v(int, ...);\nvoid f(int *p) { v(1, p); }\n"
                    "void u();\nvoid g(int *p) { u(p); }\n",
                    {"-x", "c", "-std=c17"}, /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("f", 0), EscapeReason::VarArgs);
  EXPECT_EQ(sinkOf("g", 0), EscapeReason::UnmatchedArgument);
}

//===--- Constructor initializers -----------------------------------------===//

// The parent map does not model CXXCtorInitializer, so a member initializer's
// expression has the constructor itself as its parent. Without that rule the
// store below is invisible and the constructor's parameter reads as clean.
TEST_F(ParameterEscapeExtractorTest, WrittenConstructorInitializersStoreToFields) {
  ASSERT_TRUE(setUp("struct H { int *p; H(int *q) : p(q) { } H(int *q, int n) "
                    ": p(q) { } };",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const CXXConstructorDecl *CD = ctorOf("H", 1);
  ASSERT_NE(CD, nullptr);
  EXPECT_EQ(sinkOfDecl(CD, 0), EscapeReason::StoreToField);
  // Writing into the object `this` designates does not leak `this` itself.
  EXPECT_TRUE(isClean(thisFactOfDecl(CD)));
}

// A defaulted special member has an empty body and does its work in implicit
// member initializers, so the traversal has to visit implicit code. Here the
// initializers only *load* each field out of the source object, which is not a
// use of the source's own address -- so clean is the right answer, and the
// tracked-view case below is what proves the initializers are seen at all.
TEST_F(ParameterEscapeExtractorTest, DefaultedCopyConstructorCopiesFieldsByLoad) {
  ASSERT_TRUE(setUp("struct S { int *p; S(const S &o) = default; S(int *q, int "
                    "n) : p(q) { } };\n"
                    "void use(const S &a) { S b = a; (void)b; }",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const CXXConstructorDecl *Copy = ctorOf("S", 1);
  ASSERT_NE(Copy, nullptr);
  ASSERT_TRUE(Copy->doesThisDeclarationHaveABody())
      << "the defaulted copy constructor must be defined for this to mean "
         "anything";
  EXPECT_TRUE(isClean(factOfDecl(Copy, 0)));
  EXPECT_TRUE(isClean(thisFactOfDecl(Copy)));
}

//===--- `this` as a source -----------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ThisIsAnAliasSource) {
  ASSERT_TRUE(setUp("struct S; S *g_s; void helper(S *);\n"
                    "int *g_ip; char *g_cp;\n"
                    "struct S { int *f; int n;\n"
                    "  void store() { g_s = this; }\n"
                    "  void member_addr() { g_ip = &this->n; }\n"
                    "  void launder() { g_cp = (char *)this; }\n"
                    "  void offset() { g_s = this + 0; }\n"
                    "  S *self() { return this; }\n"
                    "  void pass() { helper(this); }\n"
                    "  void call() { other(); }\n"
                    "  void other();\n"
                    "  void write(int *p) { this->f = p; }\n"
                    "  void capture() { auto l = [this] { return f; }; (void)l; } };",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  ASTContext &Ctx = AST->getASTContext();
  const EscapeFact *Store = thisFactOf("store");
  ASSERT_TRUE(Store && Store->OtherSink);
  EXPECT_EQ(Store->OtherSink->Reason, EscapeReason::StoreToGlobal);
  EXPECT_TRUE(
      flowsToDecl(thisFactOf("pass"), findFnByName("helper", Ctx), 0));
  EXPECT_TRUE(flowsToDecl(thisFactOf("call"), findFnByName("other", Ctx),
                          ThisParamIndex));
  EXPECT_TRUE(isClean(thisFactOf("write")));
  EXPECT_EQ(sinkOf("write", 0), EscapeReason::StoreToField);
  const EscapeFact *Cap = thisFactOf("capture");
  ASSERT_TRUE(Cap && Cap->OtherSink);
  EXPECT_EQ(Cap->OtherSink->Reason, EscapeReason::Capture);
  // The spellings that reach the object through `this` rather than naming it:
  // an interior pointer, a cast to another pointer type, pointer arithmetic,
  // and the return. Each arm is also pinned through a parameter source; these
  // keep the `this` source itself honest.
  const EscapeFact *Addr = thisFactOf("member_addr");
  ASSERT_TRUE(Addr && Addr->OtherSink);
  EXPECT_EQ(Addr->OtherSink->Reason, EscapeReason::StoreToGlobal);
  const EscapeFact *Laundered = thisFactOf("launder");
  ASSERT_TRUE(Laundered && Laundered->OtherSink);
  EXPECT_EQ(Laundered->OtherSink->Reason, EscapeReason::StoreToGlobal);
  const EscapeFact *Offset = thisFactOf("offset");
  ASSERT_TRUE(Offset && Offset->OtherSink);
  EXPECT_EQ(Offset->OtherSink->Reason, EscapeReason::StoreToGlobal);
  EXPECT_TRUE(thisFactOf("self")->returnsSelf());
}

//===--- Constructor initializers -----------------------------------------===//

// A base initializer is an ordinary call: its arguments are paired with the
// base constructor's parameters by position, and the escape is expressed by
// the base constructor's own facts rather than by a sink here.
//
// The pairing is asserted crosswise -- D's `p` is Base's `y` and D's `q` is
// Base's `x` -- and negatively as well as positively, so that pairing every
// argument to index 0, or swapping the two, cannot pass.
TEST_F(ParameterEscapeExtractorTest, BaseInitializerIsAnOrdinaryCall) {
  ASSERT_TRUE(setUp(R"cpp(
    struct Base { int *a; int *b; Base(int *x, int *y) : a(y), b(x) { } };
    struct D : Base { int n; D(int *p, int *q) : Base(q, p), n(0) { } };
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const CXXConstructorDecl *BC = ctorOf("Base", 2);
  const CXXConstructorDecl *DC = ctorOf("D", 2);
  ASSERT_NE(BC, nullptr);
  ASSERT_NE(DC, nullptr);
  const EscapeFact *P = factOfDecl(DC, 0);
  const EscapeFact *Q = factOfDecl(DC, 1);
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Q, nullptr);
  EXPECT_TRUE(flowsToDecl(P, BC, 1));
  EXPECT_TRUE(flowsToDecl(Q, BC, 0));
  EXPECT_FALSE(flowsToDecl(P, BC, 0));
  EXPECT_FALSE(flowsToDecl(Q, BC, 1));
  // One edge each: a use classified twice, or an argument also read as the
  // implicit object, would show up here.
  EXPECT_EQ(P->FlowsTo.size(), 1u);
  EXPECT_EQ(Q->FlowsTo.size(), 1u);
  EXPECT_EQ(sinkOfDecl(DC, 0), std::nullopt);
  EXPECT_EQ(sinkOfDecl(DC, 1), std::nullopt);
  // The escape is expressed one node along, by the base's own facts.
  EXPECT_EQ(sinkOfDecl(BC, 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOfDecl(BC, 1), EscapeReason::StoreToField);
  // Building the base subobject hands `this` to the base constructor; see
  // SubobjectConstructorsReceiveThis below. Nothing else touches it, and the
  // base constructor's own `this` is clean, so the fixpoint closes clean.
  const EscapeFact *T = thisFactOfDecl(DC);
  ASSERT_NE(T, nullptr);
  EXPECT_FALSE(T->OtherSink.has_value());
  EXPECT_TRUE(flowsToDecl(T, BC, ThisParamIndex));
  EXPECT_TRUE(isClean(thisFactOfDecl(BC)));
}

// A delegating initializer is the same ordinary call, to a sibling
// constructor rather than to a base's.
TEST_F(ParameterEscapeExtractorTest, DelegatingInitializerIsAnOrdinaryCall) {
  ASSERT_TRUE(setUp(R"cpp(
    struct G { int *p; G(int *x, int *y) : p(x) { (void)y; }
               G(int *a) : G(nullptr, a) { } };
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const CXXConstructorDecl *Two = ctorOf("G", 2);
  const CXXConstructorDecl *One = ctorOf("G", 1);
  ASSERT_NE(Two, nullptr);
  ASSERT_NE(One, nullptr);
  const EscapeFact *A = factOfDecl(One, 0);
  ASSERT_NE(A, nullptr);
  // `a` is the delegate's *second* parameter, and its first parameter is the
  // one that escapes -- so a pairing that ignored position would report a
  // sink one node along instead of a clean fact.
  EXPECT_TRUE(flowsToDecl(A, Two, 1));
  EXPECT_FALSE(flowsToDecl(A, Two, 0));
  EXPECT_EQ(A->FlowsTo.size(), 1u);
  EXPECT_EQ(sinkOfDecl(One, 0), std::nullopt);
  EXPECT_EQ(sinkOfDecl(Two, 0), EscapeReason::StoreToField);
  EXPECT_EQ(sinkOfDecl(Two, 1), std::nullopt);
}

// A member initializer appears in no statement: ParentMapContext models no
// CXXCtorInitializer, so the parent of the initializer's expression is the
// CXXConstructorDecl itself. The first two assertions pin that directly --
// without them, "the containment rules hold here" would be an inference from
// the outcomes rather than a statement about the shape.
//
// The outcomes then show the ordinary containment rules running under that
// parent: an expression nested inside the initializer is covered by its own
// parent and classified once, and only the initializer's top-level expression
// reaches the constructor row.
TEST_F(ParameterEscapeExtractorTest,
       ConstructorInitializerParentIsTheConstructor) {
  ASSERT_TRUE(setUp(R"cpp(
    int *id(int *);
    struct Nest { int *p; int *q;
      Nest(int *a, int *b) : p(a + 1), q(id(b)) { } };
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const CXXConstructorDecl *CD = ctorOf("Nest", 2);
  ASSERT_NE(CD, nullptr);
  ASTContext &Ctx = AST->getASTContext();
  ASSERT_NE(CD->init_begin(), CD->init_end());
  const CXXCtorInitializer *First = *CD->init_begin();
  ASSERT_NE(First->getInit(), nullptr);
  DynTypedNodeList Parents =
      Ctx.getParentMapContext().getParents(*First->getInit());
  ASSERT_EQ(Parents.size(), 1u);
  EXPECT_EQ(Parents[0].get<CXXConstructorDecl>(), CD)
      << "the parent is the constructor";
  EXPECT_EQ(Parents[0].get<Stmt>(), nullptr)
      << "and not an enclosing statement";

  // `a + 1` is a value alias whose operand is covered by the arithmetic; only
  // the sum reaches the constructor row, as one store into `p`.
  const EscapeFact *FA = factOfDecl(CD, 0);
  ASSERT_NE(FA, nullptr);
  ASSERT_TRUE(FA->OtherSink.has_value());
  EXPECT_EQ(FA->OtherSink->Reason, EscapeReason::StoreToField);
  EXPECT_TRUE(FA->FlowsTo.empty());
  // `id(b)` records the argument's flow *and* the store of the result, so the
  // initializer's expression and the expression nested in it are both
  // classified, each by its own rule.
  const EscapeFact *FB = factOfDecl(CD, 1);
  ASSERT_NE(FB, nullptr);
  EXPECT_TRUE(flowsToDecl(FB, findFnByName("id", Ctx), 0));
  EXPECT_EQ(FB->FlowsTo.size(), 1u);
  ASSERT_TRUE(FB->OtherSink.has_value());
  EXPECT_EQ(FB->OtherSink->Reason, EscapeReason::StoreToField);
}

// Every constructor initializer that runs a constructor hands that
// constructor a pointer to a subobject of `*this` -- to `*this` itself, when
// delegating -- and design section 1.1 counts a pointer to any subobject as
// derived from the source. A CXXConstructExpr spells no object operand, so
// this is the one implicit object argument with no expression for the use
// rules to classify; without classifySubobjectConstruction() a base
// constructor that publishes `this` leaves the derived constructor's `this`
// reading clean.
TEST_F(ParameterEscapeExtractorTest, SubobjectConstructorsReceiveThis) {
  ASSERT_TRUE(setUp(R"cpp(
    struct Base { Base(); };
    struct Mem { Mem(); };
    struct Plain { int x; };
    Base *g_b; Mem *g_m;
    Base::Base() { g_b = this; }
    Mem::Mem() { g_m = this; }
    struct D : Base   { D(int *p) : Base() { (void)p; } };
    struct M { Mem m; M(int *p) : m() { (void)p; } };
    struct Del { Del(); Del(int *p) : Del() { (void)p; } };
    struct Triv { Plain a; Triv(int *p) : a() { (void)p; } };
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const CXXConstructorDecl *BaseC = ctorOf("Base", 0);
  const CXXConstructorDecl *MemC = ctorOf("Mem", 0);
  ASSERT_NE(BaseC, nullptr);
  ASSERT_NE(MemC, nullptr);
  // Each constructor publishes the object it is building.
  EXPECT_EQ(sinkOfThisDecl(BaseC), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOfThisDecl(MemC), EscapeReason::StoreToGlobal);

  const CXXConstructorDecl *DC = ctorOf("D", 1);
  ASSERT_NE(DC, nullptr);
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(DC), BaseC, ThisParamIndex))
      << "a base subobject";
  const CXXConstructorDecl *MC = ctorOf("M", 1);
  ASSERT_NE(MC, nullptr);
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(MC), MemC, ThisParamIndex))
      << "a member subobject";
  const CXXConstructorDecl *DelOne = ctorOf("Del", 1);
  const CXXConstructorDecl *DelZero = ctorOf("Del", 0);
  ASSERT_NE(DelOne, nullptr);
  ASSERT_NE(DelZero, nullptr);
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(DelOne), DelZero, ThisParamIndex))
      << "`*this` itself, when delegating";
  // The parameters are untouched by any of it: the object argument is not an
  // argument, so nothing is attributed to them.
  for (const CXXConstructorDecl *C : {DC, MC, DelOne})
    EXPECT_TRUE(isClean(factOfDecl(C, 0))) << C->getParent()->getNameAsString();

  // A trivial constructor runs no code that could keep the object, and is
  // also the shape the entity model most often cannot name -- flowing into it
  // would degrade to UnnamedCallee and make `this` escape out of every
  // aggregate member.
  const CXXConstructorDecl *TrivC = ctorOf("Triv", 1);
  ASSERT_NE(TrivC, nullptr);
  EXPECT_TRUE(isClean(thisFactOfDecl(TrivC)));
}

// Each arm of classifySubobjectConstruction() that unwraps an initializer's
// expression down to the constructor running on the subobject. Every arm is
// reached by exactly one of these structs, against a `Leak` whose three
// constructors all publish the object being built. The expected callee differs
// per arm wherever the shape allows it, so an arm that answered some other
// constructor -- or that fell through to another arm -- would not pass.
TEST_F(ParameterEscapeExtractorTest, SubobjectConstructionShapes) {
  ASSERT_TRUE(setUp(R"cpp(
    struct Leak { Leak(); Leak(int); Leak(const Leak &); ~Leak(); };
    Leak *g_l;
    Leak::Leak()              { g_l = this; }
    Leak::Leak(int)           { g_l = this; }
    Leak::Leak(const Leak &)  { g_l = this; }
    struct Agg { Leak a; Leak b; };
    struct Plain { int x; };
    // CXXConstructExpr, written.
    struct S1 { Leak m;      S1(int *p) : m(1) { (void)p; } };
    // CXXDefaultInitExpr wrapping ExprWithCleanups: an implicit member
    // initializer built from a default member initializer.
    struct S2 { Leak m = Leak(2); S2(int *p) { (void)p; } };
    // InitListExpr: an aggregate member's elements.
    struct S3 { Agg g;       S3(int *p) : g{} { (void)p; } };
    // CXXParenListInitExpr under ExprWithCleanups (C++20 paren aggregate
    // init), whose elements are CXXBindTemporaryExpr-wrapped.
    struct S4 { Agg g;       S4(int *p) : g(Leak(3), Leak(4)) { (void)p; } };
    // A CXXConstructExpr of array type: all elements at once.
    struct S5 { Leak arr[2]; S5(int *p) : arr() { (void)p; } };
    // InitListExpr with both a written element (through
    // CXXBindTemporaryExpr) and an array filler, which take different
    // constructors so that neither can stand in for the other.
    struct S6 { Leak arr[3]; S6(int *p) : arr{Leak(5)} { (void)p; } };
    // ParenExpr.
    struct S7 { Leak m;      S7(int *p) : m((Leak(6))) { (void)p; } };
    // ArrayInitLoopExpr, which only a defaulted copy constructor produces.
    struct S8 { Leak arr[2]; S8(const S8 &) = default; S8(int *p) { (void)p; } };
    void odr_use(const S8 &a) { S8 b = a; (void)b; }
    // A trivial constructor: no arm records it.
    struct S9 { Plain a;     S9(int *p) : a() { (void)p; } };
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const CXXConstructorDecl *Default = ctorOf("Leak", 0);
  ASSERT_NE(Default, nullptr);
  const auto *RD = findDeclByName<CXXRecordDecl>("Leak", AST->getASTContext());
  ASSERT_NE(RD, nullptr);
  const CXXConstructorDecl *FromInt = nullptr, *Copy = nullptr;
  for (const CXXConstructorDecl *C : RD->getDefinition()->ctors()) {
    if (C->isImplicit() || C->getNumParams() != 1)
      continue;
    if (C->isCopyConstructor())
      Copy = C;
    else
      FromInt = C;
  }
  ASSERT_NE(FromInt, nullptr);
  ASSERT_NE(Copy, nullptr);
  // All three publish the object they are building, so each is a node worth
  // reaching.
  EXPECT_EQ(sinkOfThisDecl(Default), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOfThisDecl(FromInt), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOfThisDecl(Copy), EscapeReason::StoreToGlobal);

  struct Case {
    const char *Class;
    const CXXConstructorDecl *Callee;
  };
  // The written-element/filler pair of S6 is checked separately below.
  for (auto [Class, Callee] : std::vector<Case>{{"S1", FromInt},
                                                {"S2", FromInt},
                                                {"S3", Default},
                                                {"S4", FromInt},
                                                {"S5", Default},
                                                {"S7", FromInt}}) {
    const CXXConstructorDecl *CD = ctorOf(Class, 1);
    ASSERT_NE(CD, nullptr) << Class;
    const EscapeFact *T = thisFactOfDecl(CD);
    ASSERT_NE(T, nullptr) << Class;
    EXPECT_TRUE(flowsToDecl(T, Callee, ThisParamIndex)) << Class;
    EXPECT_FALSE(T->OtherSink.has_value()) << Class;
    // The parameter is not the object argument.
    EXPECT_TRUE(isClean(factOfDecl(CD, 0))) << Class;
  }
  // S6 reaches both: the written element through CXXBindTemporaryExpr and the
  // filler through InitListExpr::getArrayFiller().
  const CXXConstructorDecl *S6 = ctorOf("S6", 1);
  ASSERT_NE(S6, nullptr);
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(S6), FromInt, ThisParamIndex))
      << "the written element";
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(S6), Default, ThisParamIndex))
      << "the array filler";
  // S8's array member is copied element by element by the defaulted copy
  // constructor, which is the only producer of an ArrayInitLoopExpr.
  const auto *S8RD = findDeclByName<CXXRecordDecl>("S8", AST->getASTContext());
  ASSERT_NE(S8RD, nullptr);
  const CXXConstructorDecl *S8Copy = nullptr;
  for (const CXXConstructorDecl *C : S8RD->getDefinition()->ctors())
    if (C->isCopyConstructor())
      S8Copy = C;
  ASSERT_NE(S8Copy, nullptr);
  ASSERT_TRUE(S8Copy->doesThisDeclarationHaveABody())
      << "the defaulted copy constructor must be defined for this to mean "
         "anything";
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(S8Copy), Copy, ThisParamIndex));
  // A trivial constructor runs no code; nothing is recorded for it.
  const CXXConstructorDecl *S9 = ctorOf("S9", 1);
  ASSERT_NE(S9, nullptr);
  EXPECT_TRUE(isClean(thisFactOfDecl(S9)));
}

// The mirror of SubobjectConstructorsReceiveThis, and the direction that is
// reachable from a candidate parameter today: `h->~Holder()` is a
// CXXMemberCallExpr, so an ordinary parameter flows into `~Holder`'s `this`,
// and `~Holder` implicitly destroys its base without any expression saying so.
// Before classifySubobjectDestruction(), `~Holder` reported clean with no
// edges -- the shape that means "provably does not escape" -- and `recycle`'s
// `h` would have been annotated `noescape` while the call publishes it.
TEST_F(ParameterEscapeExtractorTest, SubobjectDestructorsReceiveThis) {
  ASSERT_TRUE(setUp(R"cpp(
    struct Registry;
    struct Node { Registry *r; Node(Registry *rr); ~Node(); };
    struct Plain { int x; };
    Node *g_last;
    Node::Node(Registry *rr) : r(rr) { }
    Node::~Node() { g_last = this; }
    struct Holder : Node { Holder(Registry *r) : Node(r) { } ~Holder() { } };
    struct Member { Node n; Member(Registry *r) : n(r) { } ~Member() { } };
    struct Arrayed { Node a[2]; Arrayed(Registry *r) : a{Node(r), Node(r)} { } ~Arrayed() { } };
    struct VBase : virtual Node { VBase(Registry *r) : Node(r) { } ~VBase() { } };
    struct VDerived : VBase { VDerived(Registry *r) : Node(r), VBase(r) { } ~VDerived() { } };
    struct Triv { Plain p; int n; ~Triv() { } };
    void recycle(Holder *h) { h->~Holder(); }
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  ASTContext &Ctx = AST->getASTContext();
  auto dtorOf = [&](StringRef Class) -> const CXXDestructorDecl * {
    const auto *RD = findDeclByName<CXXRecordDecl>(Class, Ctx);
    if (!RD || !RD->getDefinition()) {
      ADD_FAILURE() << "no class " << Class;
      return nullptr;
    }
    return RD->getDefinition()->getDestructor();
  };
  const CXXDestructorDecl *NodeD = dtorOf("Node");
  ASSERT_NE(NodeD, nullptr);
  EXPECT_EQ(sinkOfThisDecl(NodeD), EscapeReason::StoreToGlobal);

  // A base subobject, a member subobject, an array member's elements, and a
  // direct virtual base.
  for (const char *Class : {"Holder", "Member", "Arrayed", "VBase"}) {
    const CXXDestructorDecl *DD = dtorOf(Class);
    ASSERT_NE(DD, nullptr) << Class;
    const EscapeFact *T = thisFactOfDecl(DD);
    ASSERT_NE(T, nullptr) << Class;
    EXPECT_TRUE(flowsToDecl(T, NodeD, ThisParamIndex)) << Class;
    EXPECT_FALSE(T->OtherSink.has_value()) << Class;
  }
  // An *indirect* virtual base, which only the most-derived object destroys.
  // It is in VDerived's vbases() and in nobody's bases(), so it is the one
  // subobject the direct-base loop cannot reach -- without it, dropping the
  // vbases() loop changes nothing measurable.
  const CXXDestructorDecl *VBaseD = dtorOf("VBase");
  const CXXDestructorDecl *VDerivedD = dtorOf("VDerived");
  ASSERT_NE(VBaseD, nullptr);
  ASSERT_NE(VDerivedD, nullptr);
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(VDerivedD), VBaseD, ThisParamIndex))
      << "the direct base";
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(VDerivedD), NodeD, ThisParamIndex))
      << "the indirect virtual base";

  // The parameter that reaches the destructor: the edge into `~Holder`'s
  // `this` already existed, and it is the fact behind it that was wrong.
  const CXXDestructorDecl *HolderD = dtorOf("Holder");
  ASSERT_NE(HolderD, nullptr);
  EXPECT_TRUE(flowsToDecl(factOf("recycle", 0), HolderD, ThisParamIndex));
  const ParameterEscapeSummary *R = summaryOf("recycle");
  ASSERT_NE(R, nullptr);
  EXPECT_EQ(R->CandidateParams, (std::set<unsigned>{0}))
      << "no M1 exclusion saves this one";

  // A class with only trivially-destructible subobjects records nothing: a
  // trivial destructor runs no code, and is also the shape the entity model
  // most often cannot name.
  const CXXDestructorDecl *TrivD = dtorOf("Triv");
  ASSERT_NE(TrivD, nullptr);
  EXPECT_TRUE(isClean(thisFactOfDecl(TrivD)));
}

// The error-recovery half of classifySubobjectDestruction(): a subobject whose
// record has no definition.
//
// Unreachable on well-formed input -- a base or a member of class type must be
// complete for the enclosing class to be defined -- but this extractor runs as
// an ASTConsumer whatever the diagnostics said, and clang's recovery keeps the
// FieldDecl with its incomplete type. All three spellings below reach the
// guard. Without it, getDefinition() returns null and hasTrivialDestructor()
// dereferences it: the test process does not fail, it dies.
TEST_F(ParameterEscapeExtractorTest, IncompleteSubobjectsAreSkipped) {
  ASSERT_TRUE(setUp(R"cpp(
    struct Inc;
    template <class T> struct U;
    struct Good { Good(); ~Good(); };
    struct ByValue    { Inc m;     ~ByValue() { } };
    struct ByArray    { Inc m[2];  ~ByArray() { } };
    struct BySpecial  { U<int> m;  ~BySpecial() { } };
    struct Mixed      { Inc bad; Good good; ~Mixed() { } };
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false, /*Files=*/{},
                    /*ExpectedError=*/"incomplete type"));
  auto dtorOf = [&](StringRef Class) -> const CXXDestructorDecl * {
    const auto *RD = findDeclByName<CXXRecordDecl>(Class, AST->getASTContext());
    if (!RD || !RD->getDefinition()) {
      ADD_FAILURE() << "no class " << Class;
      return nullptr;
    }
    return RD->getDefinition()->getDestructor();
  };
  // The unusable subobject is skipped rather than answered.
  for (const char *Class : {"ByValue", "ByArray", "BySpecial"}) {
    const CXXDestructorDecl *DD = dtorOf(Class);
    ASSERT_NE(DD, nullptr) << Class;
    EXPECT_TRUE(isClean(thisFactOfDecl(DD))) << Class;
  }
  // Only that one subobject: a complete member in the same class still gets
  // its edge, so this cannot be read as "an erroneous class is abandoned".
  const CXXDestructorDecl *MixedD = dtorOf("Mixed");
  const CXXDestructorDecl *GoodD = dtorOf("Good");
  ASSERT_NE(MixedD, nullptr);
  ASSERT_NE(GoodD, nullptr);
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(MixedD), GoodD, ThisParamIndex));
}

// The value-delivering arms classifySubobjectConstruction() takes from
// computeKind(): a conditional (both branches, since the analysis is
// flow-insensitive), a comma, a statement expression, `__builtin_choose_expr`,
// `_Generic` (twice, once behind a label), and the GNU `a ?: b` spelling.
// Each shape's operands take
// constructors the others do not, so an arm that answered the wrong operand
// would not pass.
//
// C7 puts a label in front of a statement expression's value, which is what
// separates ValueStmt::getExprStmt() from body_back().
//
// C6 is also where the two omitted arms are measured. The GNU conditional
// materializes its common operand as a *temporary* and initializes the member
// by copying it, so the object `Leak(6)` builds is not a subobject of `*this`:
// the copy constructor is what runs on the member, and descending through the
// MaterializeTemporaryExpr and OpaqueValueExpr into the temporary's own
// constructor would record an edge for a call that never touches `*this`.
TEST_F(ParameterEscapeExtractorTest, SubobjectConstructionValueShapes) {
  // The three extensions this test spells, muted so that the input is clean.
  std::vector<std::string> Args = {
      "-std=c++20", "-Wno-gnu-statement-expression", "-Wno-c11-extensions",
      "-Wno-gnu-conditional-omitted-operand"};
  ASSERT_TRUE(setUp(R"cpp(
    struct Leak { Leak(); Leak(int); Leak(const Leak &); ~Leak();
                  explicit operator bool() const; };
    Leak *g_l;
    Leak::Leak()             { g_l = this; }
    Leak::Leak(int)          { g_l = this; }
    Leak::Leak(const Leak &) { g_l = this; }
    struct C1 { Leak m; C1(int *p) : m(p ? Leak(1) : Leak()) { } };
    struct C2 { Leak m; C2(int *p) : m(((void)0, Leak(2))) { (void)p; } };
    struct C3 { Leak m; C3(int *p) : m(({ Leak(3); })) { (void)p; } };
    struct C4 { Leak m; C4(int *p) : m(__builtin_choose_expr(1, Leak(4), Leak())) { (void)p; } };
    struct C5 { Leak m; C5(int *p) : m(_Generic(0, int: Leak(5))) { (void)p; } };
    struct C6 { Leak m; C6(int *p) : m(Leak(6) ?: Leak()) { (void)p; } };
    struct C7 { Leak m; C7(int *p) : m(({ lbl: Leak(7); })) { (void)p; } };
  )cpp",
                    Args, /*WithPrelude=*/false));
  const auto *RD = findDeclByName<CXXRecordDecl>("Leak", AST->getASTContext());
  ASSERT_NE(RD, nullptr);
  const CXXConstructorDecl *Default = nullptr, *FromInt = nullptr,
                           *Copy = nullptr;
  for (const CXXConstructorDecl *C : RD->getDefinition()->ctors()) {
    if (C->isImplicit())
      continue;
    if (C->getNumParams() == 0)
      Default = C;
    else if (C->isCopyConstructor())
      Copy = C;
    else
      FromInt = C;
  }
  ASSERT_NE(Default, nullptr);
  ASSERT_NE(FromInt, nullptr);
  ASSERT_NE(Copy, nullptr);
  // Every shape but the GNU conditional constructs the member in place from
  // its `Leak(n)` operand.
  // C7's last statement is a LabelStmt, so only ValueStmt::getExprStmt()
  // finds the value -- the same distinction computeKind()'s StmtExpr arm
  // makes, and StatementExpressionPropagatesThroughALabel pins for it.
  for (const char *Class : {"C1", "C2", "C3", "C4", "C5", "C7"}) {
    const CXXConstructorDecl *CD = ctorOf(Class, 1);
    ASSERT_NE(CD, nullptr) << Class;
    const EscapeFact *T = thisFactOfDecl(CD);
    ASSERT_NE(T, nullptr) << Class;
    EXPECT_TRUE(flowsToDecl(T, FromInt, ThisParamIndex)) << Class;
    EXPECT_FALSE(T->OtherSink.has_value()) << Class;
  }
  // A conditional takes both branches; `__builtin_choose_expr` takes only the
  // chosen one, which is what separates the two arms.
  EXPECT_TRUE(
      flowsToDecl(thisFactOfDecl(ctorOf("C1", 1)), Default, ThisParamIndex))
      << "the false branch of a conditional";
  EXPECT_FALSE(
      flowsToDecl(thisFactOfDecl(ctorOf("C4", 1)), Default, ThisParamIndex))
      << "the unchosen operand of __builtin_choose_expr is not evaluated";
  // The GNU conditional: the false branch builds the member directly, the
  // true branch copies a temporary into it, and the temporary's own
  // constructor is not a constructor of any subobject of `*this`.
  const CXXConstructorDecl *C6 = ctorOf("C6", 1);
  ASSERT_NE(C6, nullptr);
  const EscapeFact *T6 = thisFactOfDecl(C6);
  ASSERT_NE(T6, nullptr);
  EXPECT_TRUE(flowsToDecl(T6, Default, ThisParamIndex)) << "the false branch";
  EXPECT_TRUE(flowsToDecl(T6, Copy, ThisParamIndex)) << "the true branch";
  EXPECT_FALSE(flowsToDecl(T6, FromInt, ThisParamIndex))
      << "the materialized temporary is not a subobject";
  EXPECT_FALSE(T6->OtherSink.has_value());
}

//===--- `this` in an ordinary member function ----------------------------===//

// What a member function may do with `this` and stay clean, and what it may
// not. The `leak_field` row is the one that changed with #12: while views
// were tracked, a load out of a field of `*this` was an alias of `this`; now
// it is an ordinary load through a place, which design section 1.1 and
// invariant 8 make fresh, so storing it leaks the *field's* value and not the
// object's address. `leak_field_addr` is the shape that does escape, and the
// two sit together so that neither can be read as the other.
TEST_F(ParameterEscapeExtractorTest, MemberFunctionUsesOfThis) {
  ASSERT_TRUE(setUp(R"cpp(
    int *g_ptr;
    struct T { int *m; int n; int arr[4];
      void set(int *p)        { m = p; n = 1; }
      void set_explicit(int *p) { this->m = p; }
      int  scalars()          { return n + arr[0] + *m; }
      void leak_field()       { g_ptr = m; }
      void leak_field_addr()  { g_ptr = &n; }
      int *ret_derived()      { return &n; }
      int *ret_decay()        { return arr; }
      T   *ret_this()         { return this; } };
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  // `this` as the base of a member store is benign for `this`; the stored
  // value is what escapes. Both spellings, since the implicit one reaches the
  // member through a CXXThisExpr the source never wrote.
  EXPECT_TRUE(isClean(thisFactOf("set")));
  EXPECT_EQ(sinkOf("set", 0), EscapeReason::StoreToField);
  EXPECT_TRUE(isClean(thisFactOf("set_explicit")));
  EXPECT_EQ(sinkOf("set_explicit", 0), EscapeReason::StoreToField);
  // Reading scalars out of the object, including through a pointer field.
  EXPECT_TRUE(isClean(thisFactOf("scalars")));
  // Loads are fresh (design invariant 8): the field's value is not the
  // object's address.
  EXPECT_TRUE(isClean(thisFactOf("leak_field")));
  // Its address is.
  const EscapeFact *A = thisFactOf("leak_field_addr");
  ASSERT_TRUE(A && A->OtherSink);
  EXPECT_EQ(A->OtherSink->Reason, EscapeReason::StoreToGlobal);
  // Returning `this`, or a pointer derived from it -- the address of a member
  // and an array member's decay -- is ReturnsSelf.
  for (const char *Fn : {"ret_this", "ret_derived", "ret_decay"}) {
    const EscapeFact *R = thisFactOf(Fn);
    ASSERT_NE(R, nullptr) << Fn;
    EXPECT_TRUE(R->returnsSelf()) << Fn;
    EXPECT_FALSE(R->OtherSink.has_value()) << Fn;
  }
}

//===--- The view exclusion -----------------------------------------------===//

// #12 made a view-typed parameter non-pointer-carrying, so it carries no fact
// and is never a candidate. The `[[gsl::Pointer]]` and `swift_attr` spellings
// are pinned by ViewTypedParametersDropOutByValueAndByReference; these are the
// standard-library ones, whose attribute Sema infers from the name in
// inferGslPointerAttribute() rather than from a spelling in the source -- a
// different path into the same predicate, and the one a real translation unit
// takes.
//
// By value is not the interesting shape: no record type is pointer-carrying,
// so a view drops out there for the same reason a std::vector does. The
// shapes that isViewRecordType() alone decides are the reference and the
// pointer, and both are asserted, against a std::vector in the same position
// as the control that keeps them honest.
TEST_F(ParameterEscapeExtractorTest, StandardViewTypedParametersCarryNoFact) {
  ASSERT_TRUE(setUp(R"cpp(
    namespace std {
      template <class T> class span { public: T *d; unsigned n; };
      template <class C> class basic_string_view { public: const C *d; unsigned n; };
      using string_view = basic_string_view<char>;
      template <class T> class vector { public: T *d; };
    }
    void by_value(std::span<int> s, std::string_view sv, std::vector<int> v, int *keep) { }
    void by_ref(std::span<int> &s, const std::string_view &sv, std::vector<int> &v, int *keep) { }
    void by_ptr(std::span<int> *s, std::string_view *sv, std::vector<int> *v, int *keep) { }
  )cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  // No record type is pointer-carrying, so the view and the owner alike drop
  // out by value. This row claims nothing about the view predicate.
  const ParameterEscapeSummary *V = summaryOf("by_value");
  ASSERT_NE(V, nullptr);
  EXPECT_EQ(analyzedParamsOf("by_value"), (std::set<unsigned>{3}));
  EXPECT_EQ(V->CandidateParams, (std::set<unsigned>{3}));
  // A reference to a view is refused with the view; a reference to an owner
  // is an ordinary reference, analyzed and annotatable.
  const ParameterEscapeSummary *R = summaryOf("by_ref");
  ASSERT_NE(R, nullptr);
  EXPECT_EQ(analyzedParamsOf("by_ref"), (std::set<unsigned>{2, 3}));
  EXPECT_EQ(R->CandidateParams, (std::set<unsigned>{2, 3}));
  // A pointer to a view keeps its fact -- callers read it -- and leaves
  // candidacy alone.
  const ParameterEscapeSummary *P = summaryOf("by_ptr");
  ASSERT_NE(P, nullptr);
  EXPECT_EQ(analyzedParamsOf("by_ptr"), (std::set<unsigned>{0, 1, 2, 3}));
  EXPECT_EQ(P->CandidateParams, (std::set<unsigned>{2, 3}));
}

// The member-initializer form of AliasesReachingAViewStillSink: a view built
// in a constructor initializer is an ordinary constructor call, so the alias
// does not get away -- it reaches the sink through the view constructor's own
// facts rather than through any view tracking.
//
// The issue this test answers predicted a StoreToField sink on `p` itself.
// That is what the tracked-view model produced, where constructing a view was
// a field store; measured against the model #12 left behind, the store is one
// node along, in View's constructor.
TEST_F(ParameterEscapeExtractorTest, ViewMemberInitializerReachesTheSink) {
  ASSERT_TRUE(setUp("struct H2 { View v; H2(int *p) : v(p, 1) { } };"));
  const CXXConstructorDecl *H2C = ctorOf("H2", 1);
  const CXXConstructorDecl *VC = ctorOf("View", 2);
  ASSERT_NE(H2C, nullptr);
  ASSERT_NE(VC, nullptr);
  const EscapeFact *P = factOfDecl(H2C, 0);
  ASSERT_NE(P, nullptr);
  EXPECT_FALSE(P->OtherSink.has_value());
  EXPECT_TRUE(flowsToDecl(P, VC, 0));
  // Exactly one edge: the subobject's implicit object argument belongs to
  // `this`, not to `p`, so a rule that attributed it to every source would
  // show up here as a second edge into View's `this`.
  EXPECT_EQ(P->FlowsTo.size(), 1u);
  EXPECT_FALSE(flowsToDecl(P, VC, ThisParamIndex));
  // And the escape is expressed, one node along.
  EXPECT_EQ(sinkOfDecl(VC, 0), EscapeReason::StoreToField);
  // `this` is what receives the subobject.
  EXPECT_TRUE(flowsToDecl(thisFactOfDecl(H2C), VC, ThisParamIndex));
}

//===--- Objective-C++, blocks and glvalue edge cases (#14) ---------------===//

// M1 analyzes C and C++ functions in an Objective-C++ TU and models no
// Objective-C construct: every one of them has to answer with a sink, because
// a clean fact with no edges is the shape the fixpoint reads as "provably does
// not escape".
//
// Enumerated over what the language offers rather than over the four spellings
// the issue listed: messages, properties in both directions, subscripting in
// both directions, @synchronized, @throw, boxed expressions, container
// literals and fast enumeration. The Detail string is asserted with the
// reason, so that "the default row fired on this shape" is distinguishable
// from "the classifier produced nothing" -- and so that a future rule that
// answered one of these by a *different* unmodelled shape shows up here.
//
// Deliberately not here, each because it forms no operand out of an alias:
// @selector, @encode, @protocol, @available and the ObjC boolean literals.
// Their C/C++ neighbours in the same function stay clean, which is correct.
TEST_F(ParameterEscapeExtractorTest, ObjCConstructsAreSinks) {
  ASSERT_TRUE(setUp(R"objc(
@interface Foo
- (void)take:(int *)p;
- (id)objectAtIndexedSubscript:(unsigned)i;
- (void)setObject:(id)o atIndexedSubscript:(unsigned)i;
@property (assign) int *prop;
@property (strong) id obj;
@end
@interface NSString
+ (NSString *)stringWithUTF8String:(const char *)s;
@end
@interface NSArray
+ (id)arrayWithObjects:(const id *)o count:(unsigned long)c;
@end
@interface NSDictionary
+ (id)dictionaryWithObjects:(const id *)o forKeys:(const id *)k count:(unsigned long)c;
@end
int *g_ptr;
id g_obj;
void message(Foo *o, int *p) { [o take:p]; }
void propertyWrite(Foo *o, int *p) { o.prop = p; }
void propertyRead(Foo *o) { g_ptr = o.prop; }
void propertyObjWrite(Foo *o, id x) { o.obj = x; }
void subscriptWrite(Foo *o, id x) { o[0] = x; }
void subscriptRead(Foo *o) { g_obj = o[0]; }
void synchronizedOn(Foo *o) { @synchronized(o) { } }
void throwAlias(Foo *o) { @throw o; }
void boxedPointer(const char *p) { g_obj = @(p); }
void arrayLiteral(id o) { g_obj = @[o]; }
void dictionaryLiteral(id k, id v) { g_obj = @{k : v}; }
void fastEnumeration(Foo *c) { for (id x in c) { (void)x; } }
void noOperandForms(int *p) { (void)@selector(take:); (void)@encode(int *);
                              (void)__objc_yes; (void)*p; }
)objc",
                    {"-x", "objective-c++", "-std=c++20", "-fblocks",
                     "-fobjc-arc"},
                    /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("message", 0), EscapeReason::ObjCMessage);
  EXPECT_EQ(sinkOf("message", 1), EscapeReason::ObjCMessage);

  // A property access is a PseudoObjectExpr over an ObjCPropertyRefExpr; the
  // receiver's use is the property reference itself, which nothing models.
  auto detail = [&](StringRef Fn, unsigned I) -> std::string {
    const EscapeFact *F = factOf(Fn, I);
    return F && F->OtherSink ? F->OtherSink->Detail : std::string("<none>");
  };
  EXPECT_EQ(sinkOf("propertyWrite", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("propertyWrite", 0), "ObjCPropertyRefExpr");
  // The stored value's own use: the assignment's left operand is not a
  // variable and not a member, so the store is attributed to a pointer.
  EXPECT_EQ(sinkOf("propertyWrite", 1), EscapeReason::StoreThroughPointer);
  EXPECT_EQ(sinkOf("propertyRead", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("propertyRead", 0), "ObjCPropertyRefExpr");
  EXPECT_EQ(sinkOf("propertyObjWrite", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(sinkOf("propertyObjWrite", 1), EscapeReason::StoreThroughPointer);

  EXPECT_EQ(sinkOf("subscriptWrite", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("subscriptWrite", 0), "ObjCSubscriptRefExpr");
  EXPECT_EQ(sinkOf("subscriptWrite", 1), EscapeReason::StoreThroughPointer);
  EXPECT_EQ(sinkOf("subscriptRead", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("subscriptRead", 0), "ObjCSubscriptRefExpr");

  EXPECT_EQ(sinkOf("synchronizedOn", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("synchronizedOn", 0), "ObjCAtSynchronizedStmt");
  EXPECT_EQ(sinkOf("throwAlias", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("throwAlias", 0), "ObjCAtThrowStmt");
  EXPECT_EQ(sinkOf("boxedPointer", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("boxedPointer", 0), "ObjCBoxedExpr");
  EXPECT_EQ(sinkOf("arrayLiteral", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("arrayLiteral", 0), "ObjCArrayLiteral");
  EXPECT_EQ(sinkOf("dictionaryLiteral", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("dictionaryLiteral", 0), "ObjCDictionaryLiteral");
  EXPECT_EQ(sinkOf("dictionaryLiteral", 1), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(sinkOf("fastEnumeration", 0), EscapeReason::UnrecognizedUse);
  EXPECT_EQ(detail("fastEnumeration", 0), "ObjCForCollectionStmt");

  // The forms that form no operand leave their C neighbours alone.
  EXPECT_TRUE(clean("noOperandForms", 0));
}

// The ARC-specific spellings. The bridging casts preserve provenance -- a
// bridged cast is a CastExpr whose result type is still pointer-carrying -- so
// the alias survives them and its *store* is what sinks. The ownership
// qualifiers are just qualifiers on ordinary local pointer storage.
//
// The statement forms that only scope a body -- @autoreleasepool, @try/@catch/
// @finally, @synchronized -- must not swallow the uses inside them. Each is
// asserted with the escaping body and with a benign one, so that "the body is
// classified" is separated from "the statement is itself a sink".
TEST_F(ParameterEscapeExtractorTest, ObjCARCBridgesQualifiersAndScopedBodies) {
  ASSERT_TRUE(setUp(R"objc(
@interface Foo @end
int *g_ptr;
id g_obj;
void takesOut(Foo * __autoreleasing *out);
void bridgeCast(void *p) { g_obj = (__bridge id)p; }
void bridgeTransfer(void *p) { g_obj = (__bridge_transfer id)p; }
void bridgeRetained(id o) { g_ptr = (int *)(__bridge_retained void *)o; }
void weakLocal(Foo *o) { __weak Foo *w = o; g_obj = w; }
void unsafeUnretainedLocal(Foo *o) { __unsafe_unretained Foo *w = o; g_obj = w; }
void autoreleasingOut(Foo * __autoreleasing *o) { g_obj = *o; }
void writeback(Foo *o) { Foo * __strong local = o; takesOut(&local); }
void poolBody(int *p) { @autoreleasepool { g_ptr = p; } }
void poolClean(int *p) { @autoreleasepool { (void)*p; } }
void tryBody(int *p) { @try { g_ptr = p; } @catch (id e) { } }
void catchBody(int *p) { @try { } @catch (id e) { g_ptr = p; } }
void finallyBody(int *p) { @try { } @finally { g_ptr = p; } }
void syncBody(Foo *o, int *p) { @synchronized(o) { g_ptr = p; } }
void syncClean(Foo *o, int *p) { @synchronized(o) { (void)*p; } }
)objc",
                    {"-x", "objective-c++", "-std=c++20", "-fblocks",
                     "-fobjc-arc"},
                    /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("bridgeCast", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("bridgeTransfer", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("bridgeRetained", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("weakLocal", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("unsafeUnretainedLocal", 0), EscapeReason::StoreToGlobal);
  // Reading the pointee of an `id *` is a load through a Place, which design
  // section 1.1 makes fresh: what escapes is the object the slot held, not the
  // slot the caller handed over. The same answer as `g = *pp` for `int **pp`.
  EXPECT_TRUE(clean("autoreleasingOut", 0));
  // ARC's write-back passes the address of a local, which is an ordinary
  // address-taken escape of the local rather than an ObjC-specific shape.
  EXPECT_EQ(sinkOf("writeback", 0), EscapeReason::AddressTaken);

  EXPECT_EQ(sinkOf("poolBody", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("poolClean", 0));
  EXPECT_EQ(sinkOf("tryBody", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("catchBody", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("finallyBody", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("syncBody", 1), EscapeReason::StoreToGlobal);
  // The @synchronized *operand* is still a sink whatever the body does.
  EXPECT_EQ(sinkOf("syncClean", 0), EscapeReason::UnrecognizedUse);
  EXPECT_TRUE(clean("syncClean", 1));
}

// Blocks, enumerated: a literal capturing the parameter directly, one whose
// captured copy is taken by Block_copy, one stored into a global, one
// returned, one nested in another, a block parameter of a C function that is
// called, stored, passed on and passed to a `noescape` parameter, a block
// called *with* the alias, and a capture of an alias held in a local rather
// than in the parameter itself.
TEST_F(ParameterEscapeExtractorTest, BlockShapes) {
  ASSERT_TRUE(setUp(R"objc(
int *g_ptr;
typedef void (^Blk)(void);
Blk g_blk;
void takesBlock(Blk b);
void takesBlockNoescape(__attribute__((noescape)) Blk b);
extern "C" void *_Block_copy(const void *);
void captured(int *p) { Blk b = ^{ (void)*p; }; (void)b; }
void capturedStored(int *p) { g_blk = ^{ (void)*p; }; }
void capturedCopied(int *p) { Blk b = (__bridge Blk)_Block_copy((__bridge const void *)^{ (void)*p; }); (void)b; }
Blk capturedReturned(int *p) { return ^{ (void)*p; }; }
void capturedNested(int *p) { Blk b = ^{ Blk c = ^{ (void)*p; }; (void)c; }; (void)b; }
void capturedViaLocal(int *p) { int *q = p; Blk b = ^{ (void)*q; }; g_blk = b; }
void noCapture(int *p) { Blk b = ^{ }; (void)b; (void)*p; }
void blockCalled(Blk b) { b(); }
void blockStored(Blk b) { g_blk = b; }
void blockPassed(Blk b) { takesBlock(b); }
void blockPassedNoescape(Blk b) { takesBlockNoescape(b); }
void calledWithAlias(int *p) { void (^b)(int *) = ^(int *x) { (void)x; }; b(p); }
struct S { int *f; void m() { g_blk = ^{ (void)f; }; } };
)objc",
                    {"-x", "objective-c++", "-std=c++20", "-fblocks",
                     "-fobjc-arc"},
                    /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("captured", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("capturedStored", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("capturedCopied", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("capturedReturned", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("capturedNested", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("capturedViaLocal", 0), EscapeReason::Capture);
  // A block that captures nothing does not reach the parameter at all.
  EXPECT_TRUE(clean("noCapture", 0));

  // A block *parameter* is analyzed (its facts feed callers) though it is
  // never a candidate, so each of these has to answer.
  EXPECT_EQ(sinkOf("blockCalled", 0), EscapeReason::CallableUse);
  EXPECT_EQ(sinkOf("blockStored", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(flowsTo("blockPassed", 0, "takesBlock", 0));
  EXPECT_EQ(sinkOf("blockPassed", 0), std::nullopt);
  // Declared `noescape` is one of the two trusted external sources.
  EXPECT_TRUE(clean("blockPassedNoescape", 0));
  // Calling through a block pointer is an indirect call: the body that runs is
  // not in this TU.
  EXPECT_EQ(sinkOf("calledWithAlias", 0), EscapeReason::IndirectCall);
  EXPECT_EQ(sinkOfThisDecl(findFnByName("m", AST->getASTContext())),
            EscapeReason::Capture);
}

// A C++ lambda converted to a block. Clang does not leave the LambdaExpr where
// it was written: it wraps the conversion in a BlockExpr whose BlockDecl
// captures a synthetic temporary of the closure's type, and hangs the
// LambdaExpr off *that capture's copy expression*. Before #14 the block
// traversal looked only at the captured variables -- the temporary, which is
// never an alias -- and returned without descending, so every one of these
// reported no sink, no flow and no return: the shape the fixpoint reads as
// "provably does not escape" and CodeGen lowers to `captures(none)`.
//
// All four spellings that put a parameter inside such a closure are here: by
// copy, by reference, `this`, and an init-capture whose *initializer* runs in
// the enclosing function and so must still grow the alias set.
TEST_F(ParameterEscapeExtractorTest, LambdaConvertedToBlockIsACapture) {
  ASSERT_TRUE(setUp(R"objc(
int *g_ptr;
typedef void (^Blk)(void);
Blk g_blk;
void byCopy(int *p) { Blk b = [p]() { (void)*p; }; g_blk = b; }
void byReference(int *p) { Blk b = [&p]() { (void)*p; }; g_blk = b; }
struct S { int *f;
  void byThis() { Blk b = [this]() { (void)f; }; g_blk = b; }
  void byStarThis() { Blk b = [*this]() { (void)f; }; g_blk = b; } };
void initCaptureGrowth(int *p) { int *q = nullptr;
  Blk b = [n = (q = p, 0)]() { (void)n; }; g_blk = b; g_ptr = q; }
void viaLocalClosure(int *p) { auto l = [p]() { (void)*p; }; Blk b = l; g_blk = b; }
void closureOfLocalAlias(int *p) { int *q = p; Blk b = [q]() { (void)*q; }; g_blk = b; }
void emptyClosure(int *p) { Blk b = []() { }; g_blk = b; (void)*p; }
)objc",
                    {"-x", "objective-c++", "-std=c++20", "-fblocks",
                     "-fobjc-arc"},
                    /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("byCopy", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("byReference", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOfThisDecl(findFnByName("byThis", AST->getASTContext())),
            EscapeReason::Capture);
  EXPECT_EQ(sinkOfThisDecl(findFnByName("byStarThis", AST->getASTContext())),
            EscapeReason::Capture);
  // Not a Capture: `p` reaches no closure. It reaches `q`, and the growth pass
  // only sees that through the same traversal -- without it `q` never joins
  // the alias set and the store two statements later is invisible.
  //
  // Measured by removing the growth pass's traversal alone: with assertions on
  // this case does not merely go clean, it aborts on classifyStoreInto()'s
  // "must have joined the alias set during the growth pass" assertion, which
  // is that assertion's whole purpose. Assertions off, it is a silent clean.
  EXPECT_EQ(sinkOf("initCaptureGrowth", 0), EscapeReason::StoreToGlobal);
  // Written out rather than converted in place, so the LambdaExpr is a direct
  // child of the declaration and the ordinary lambda rule sees it. Here to
  // separate "a closure that captures p is a Capture" from "the conversion
  // hides it": removing the block traversal leaves this one green.
  EXPECT_EQ(sinkOf("viaLocalClosure", 0), EscapeReason::Capture);
  EXPECT_EQ(sinkOf("closureOfLocalAlias", 0), EscapeReason::Capture);
  // A closure that captures nothing still converts to a block; nothing of the
  // parameter is in it, so the conversion must not invent an escape either.
  EXPECT_TRUE(clean("emptyClosure", 0));
}

// The glvalue rows of design section 5.2, over the shapes that make a glvalue
// out of an alias: a reference bound to the pointee, a reference *parameter*
// whose referent's address is taken, a call returning a reference to a
// pointer, a conditional, a comma, a range-for, a std::move-like reference
// cast, an unknown callee, a defaulted parameter and a nested lambda.
//
// The pairs matter more than the individual answers: each escaping spelling is
// asserted next to the neighbouring benign one, because what separates them is
// a single rule -- a load through a Place is fresh, its address is not.
TEST_F(ParameterEscapeExtractorTest, GlvalueEdgeCases) {
  ASSERT_TRUE(setUp(R"cpp(
int *g_ptr;
struct Sub { int m; int *pm; };
int *&slot(int *);
void unknownFn(int *);
void hasDefaultArg(int *a, int *b = nullptr);
namespace std { template <class T> struct rr { typedef T type; };
  template <class T> typename rr<T>::type &&move(T &t) noexcept; }
struct It { int *p; int &operator*(); It &operator++(); bool operator!=(const It &) const; };
struct Range { It begin(); It end(); };
void refToPointeeAddress(int *p) { int &r = *p; g_ptr = &r; }
void refToPointeeWrite(int *p) { int &r = *p; r = 1; }
void refParamFieldAddress(Sub &s) { g_ptr = &s.m; }
void refParamFieldRead(Sub &s) { int x = s.m; (void)x; }
void refParamFieldLoad(Sub &s) { g_ptr = s.pm; }
void glvalueCallResult(int *p) { g_ptr = slot(p); }
void glvalueCallTarget(int *p) { slot(p) = p; }
void conditional(int c, int *p, int *q) { g_ptr = c ? p : q; }
void conditionalCondition(int *p) { g_ptr = p ? g_ptr : nullptr; }
void commaOperand(int *p) { g_ptr = (0, p); }
void rangeForOverPointee(Range *r) { for (int &x : *r) { (void)x; } }
void moveLikeCast(int *p) { g_ptr = std::move(p); }
void moveLikeCastAlone(int *p) { (void)std::move(p); }
void unknownCallee(int *p) { unknownFn(p); }
void defaultedTail(int *p) { hasDefaultArg(p); }
void defaultedSecond(int *p) { hasDefaultArg(nullptr, p); }
void nestedLambda(int *p) { auto o = [&] { auto i = [p] { (void)*p; }; (void)i; }; (void)o; }
)cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("refToPointeeAddress", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("refToPointeeWrite", 0));
  EXPECT_EQ(sinkOf("refParamFieldAddress", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("refParamFieldRead", 0));
  // Reading a pointer *out of* the referent is a load through a Place.
  EXPECT_TRUE(clean("refParamFieldLoad", 0));

  // A glvalue call result whose referent is pointer-carrying is an alias, so
  // reading it stores the alias and assigning to it stores through a pointer.
  // Both also record the flow into slot()'s own parameter, which is what the
  // fixpoint closes; asserting it keeps the sink from hiding a dropped edge.
  EXPECT_EQ(sinkOf("glvalueCallResult", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(flowsTo("glvalueCallResult", 0, "slot", 0));
  EXPECT_EQ(sinkOf("glvalueCallTarget", 0), EscapeReason::StoreThroughPointer);
  EXPECT_TRUE(flowsTo("glvalueCallTarget", 0, "slot", 0));

  EXPECT_EQ(sinkOf("conditional", 1), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("conditional", 2), EscapeReason::StoreToGlobal);
  // Only the branches carry the value; the condition merely reads it.
  EXPECT_TRUE(clean("conditionalCondition", 0));
  EXPECT_EQ(sinkOf("commaOperand", 0), EscapeReason::StoreToGlobal);

  // A range-for calls begin() and end() on the pointee; the alias is their
  // implicit object argument, and neither is a sink of its own.
  EXPECT_EQ(sinkOf("rangeForOverPointee", 0), std::nullopt);
  EXPECT_TRUE(flowsTo("rangeForOverPointee", 0, "begin", ThisParamIndex));
  EXPECT_TRUE(flowsTo("rangeForOverPointee", 0, "end", ThisParamIndex));

  // A std::move-like cast re-types its operand and keeps the alias, recording
  // no flow of its own -- clang models these as builtins the entity model
  // refuses to name, so a flow would degrade to UnnamedCallee.
  EXPECT_EQ(sinkOf("moveLikeCast", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("moveLikeCastAlone", 0));

  // An unknown callee is a flow, not a sink: the fixpoint resolves it.
  EXPECT_FALSE(clean("unknownCallee", 0));
  EXPECT_EQ(sinkOf("unknownCallee", 0), std::nullopt);
  EXPECT_TRUE(flowsTo("unknownCallee", 0, "unknownFn", 0));

  // A defaulted parameter does not disturb positional matching in either
  // direction: an omitted tail, and an explicit argument in the defaulted slot.
  EXPECT_EQ(sinkOf("defaultedTail", 0), std::nullopt);
  EXPECT_TRUE(flowsTo("defaultedTail", 0, "hasDefaultArg", 0));
  EXPECT_EQ(sinkOf("defaultedSecond", 0), std::nullopt);
  EXPECT_TRUE(flowsTo("defaultedSecond", 0, "hasDefaultArg", 1));

  EXPECT_EQ(sinkOf("nestedLambda", 0), EscapeReason::Capture);
}

// The glvalue rows again, over the reference shapes a *parameter* can be.
// Separated from the test above because these are about what a reference
// parameter denotes rather than about what makes a glvalue out of a pointer.
TEST_F(ParameterEscapeExtractorTest, ReferenceParameterGlvalues) {
  ASSERT_TRUE(setUp(R"cpp(
int *g_ptr;
struct B { int m; virtual ~B(); };
struct D : B { };
void refToReferentAddress(int &r) { g_ptr = &r; }
void refToReferentRead(int &r) { int x = r; (void)x; }
void refToPointerRead(int *&r) { g_ptr = r; }
void refToPointerWrite(int *&r) { r = g_ptr; }
void refToPointerAddress(int *&r) { g_ptr = (int *)&r; }
void refStaticCast(D &d) { B &b = static_cast<B &>(d); g_ptr = &b.m; }
void refDynamicCast(B &b) { D &d = dynamic_cast<D &>(b); g_ptr = &d.m; }
void refReinterpretRead(int &r) { long &l = reinterpret_cast<long &>(r); (void)l; }
void arrayReference(int (&a)[4]) { g_ptr = a; }
void localRefToPointerVar(int *p) { int *&r = p; g_ptr = r; }
)cpp",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  EXPECT_EQ(sinkOf("refToReferentAddress", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("refToReferentRead", 0));
  // A reference to a *pointer* denotes storage holding an alias, not the
  // pointee object, so loading it yields the alias -- not the fresh value a
  // load through a Place yields. That is the conservative direction: it can
  // only add sinks to a parameter whose `noescape` would be a promise about
  // the reference rather than about what the slot happens to hold.
  EXPECT_EQ(sinkOf("refToPointerRead", 0), EscapeReason::StoreToGlobal);
  // Writing through a reference parameter writes the referent, which this
  // analysis does not track; the reference itself has not escaped.
  EXPECT_TRUE(clean("refToPointerWrite", 0));
  // Taking the address of the reference exposes the slot itself.
  EXPECT_EQ(sinkOf("refToPointerAddress", 0), EscapeReason::AddressTaken);
  // A reference cast denotes the same object, so an interior pointer out of
  // the result is still derived from the parameter.
  EXPECT_EQ(sinkOf("refStaticCast", 0), EscapeReason::StoreToGlobal);
  EXPECT_EQ(sinkOf("refDynamicCast", 0), EscapeReason::StoreToGlobal);
  EXPECT_TRUE(clean("refReinterpretRead", 0));
  EXPECT_EQ(sinkOf("arrayReference", 0), EscapeReason::StoreToGlobal);
  // Binding a local reference to the pointer *variable* exposes the storage.
  EXPECT_EQ(sinkOf("localRefToPointerVar", 0), EscapeReason::AddressTaken);
}

} // namespace
