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

} // namespace
