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
  TUSummary TUSum{
      llvm::Triple("arm64-apple-macosx"),
      BuildNamespace(BuildNamespaceKind::CompilationUnit, "Mock.cpp")};
  TUSummaryBuilder Builder{TUSum, Opts};
  std::unique_ptr<TUSummaryExtractor> Extractor;
  std::unique_ptr<ASTUnit> AST;

  /// A non-empty \p ExpectedError keeps going on an ill-formed TU, and
  /// requires a diagnostic containing that text. The extractor runs as an
  /// ASTConsumer whatever the diagnostics said, so a few reachable states only
  /// exist on an AST that contains errors -- naming the expected one keeps an
  /// unrelated mistake in the test input from passing for the state under
  /// test.
  bool setUp(StringRef Body, std::vector<std::string> Args = {"-std=c++20"},
             bool WithPrelude = true, tooling::FileContentMappings Files = {},
             StringRef ExpectedError = "") {
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
    Extractor = makeTUSummaryExtractor(ParameterEscapeSummary::Name, Builder);
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
    auto &Data = getData(TUSum);
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
    const auto *RD = findDeclByName<CXXRecordDecl>(Class, AST->getASTContext());
    if (!RD) {
      ADD_FAILURE() << "no class " << Class;
      return nullptr;
    }
    for (const CXXConstructorDecl *CD : RD->getDefinition()->ctors())
      if (!CD->isImplicit() && CD->getNumParams() == NumParams)
        return summaryOfDecl(CD);
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
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{0, 2, 3}));
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
  // Not the collision path: this is the ordinary stub fact.
  const EscapeFact *F = factOf("mv", 0);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Detail, "classifier stub");
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
  const EscapeFact *F = factOf("solo", 0);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Detail, "classifier stub");
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
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{2, 4}));
  // None of the excluded types is pointer-carrying either, so none is even
  // analyzed. (A tracked view and an object pointer are both.)
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{2, 4}));
}

TEST_F(ParameterEscapeExtractorTest, ViewWithNonTrivialCopyIsNotTracked) {
  ASSERT_TRUE(setUp("void f(CopyCtorView v, View w) { }"));
  const ParameterEscapeSummary *S = summaryOf("f");
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{1}));
  EXPECT_EQ(analyzedParamsOf("f"), (std::set<unsigned>{1}));
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

//===--- `this` -----------------------------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, ThisFactOnlyForInstanceMethods) {
  ASSERT_TRUE(
      setUp("struct S { void inst(int *p) { } static void stat(int *p) { } };\n"
            "void freefn(int *p) { }"));
  EXPECT_NE(thisFactOf("inst"), nullptr);
  EXPECT_EQ(thisFactOf("stat"), nullptr);
  EXPECT_EQ(thisFactOf("freefn"), nullptr);
}

//===--- The stub's facts are sound ---------------------------------------===//

TEST_F(ParameterEscapeExtractorTest, StubSinksEveryAnalyzedParameterAndThis) {
  // No prelude, so the locations below are absolute: `g` is named on line 4,
  // column 6. Line and column differ, so a transposition cannot hide.
  ASSERT_TRUE(setUp("struct [[gsl::Pointer(int)]] View { int *d; };\n"
                    "\n"
                    "\n"
                    "void g(int *p, View v, int &r, int n) { }",
                    {"-std=c++20"}, /*WithPrelude=*/false));
  const ParameterEscapeSummary *S = summaryOf("g");
  ASSERT_NE(S, nullptr);
  EXPECT_TRUE(S->IsCandidate);
  EXPECT_EQ(analyzedParamsOf("g"), (std::set<unsigned>{0, 1, 2}));
  for (unsigned I : {0u, 1u, 2u}) {
    const EscapeFact *F = factOf("g", I);
    ASSERT_NE(F, nullptr) << "index " << I;
    EXPECT_TRUE(F->FlowsTo.empty()) << "index " << I;
    EXPECT_FALSE(F->returnsSelf()) << "index " << I;
    ASSERT_TRUE(F->OtherSink.has_value()) << "index " << I;
    EXPECT_EQ(F->OtherSink->Reason, EscapeReason::UnrecognizedUse)
        << "index " << I;
    EXPECT_EQ(F->OtherSink->Detail, "classifier stub") << "index " << I;
    EXPECT_EQ(F->OtherSink->Location.FilePath, "input.cc") << "index " << I;
    EXPECT_EQ(F->OtherSink->Location.Line, 4u) << "index " << I;
    EXPECT_EQ(F->OtherSink->Location.Column, 6u) << "index " << I;
  }
}

TEST_F(ParameterEscapeExtractorTest, StubSinksThisOfAnInstanceMethod) {
  ASSERT_TRUE(setUp("struct S { void m() { } };"));
  const EscapeFact *F = thisFactOf("m");
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->OtherSink.has_value());
  EXPECT_EQ(F->OtherSink->Reason, EscapeReason::UnrecognizedUse);
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
  // Rebuilt from `host`: index 7 is gone, and 0 and 2 are its pointer-carrying
  // parameters. `n` is neither analyzed nor a candidate.
  EXPECT_EQ(S.CandidateParams, (std::set<unsigned>{0, 2}));
  std::set<unsigned> Keys;
  for (const auto &[Index, Fact] : S.Params)
    Keys.insert(Index);
  EXPECT_EQ(Keys, (std::set<unsigned>{0, 2}));
  ASSERT_TRUE(S.This.has_value()) << "host is an instance method";
  for (const EscapeFact *F : {&S.Params.at(0), &S.Params.at(2), &*S.This}) {
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

} // namespace
