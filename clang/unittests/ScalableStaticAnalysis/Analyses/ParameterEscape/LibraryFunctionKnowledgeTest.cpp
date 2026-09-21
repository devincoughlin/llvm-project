//===- LibraryFunctionKnowledgeTest.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/LibraryFunctionKnowledge.h"
#include "../../FindDecl.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "gtest/gtest.h"

#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

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

// Build a throwaway TU and ask whether parameter `Idx` of `FnName` is trusted.
// Used by the gate tests, each of which needs its own conflicting declaration
// of the same library name and so cannot share one TU.
bool trustsParam(StringRef Code, StringRef FnName, unsigned Idx,
                 std::vector<std::string> Args = {"-x", "c", "-std=c17"}) {
  // The hostile redeclarations below are ill-formed-ish on purpose; their
  // diagnostics are the point, not a problem.
  Args.push_back("-Wno-everything");
  std::unique_ptr<ASTUnit> AST = tooling::buildASTFromCodeWithArgs(Code, Args);
  EXPECT_NE(AST, nullptr);
  if (!AST)
    return false;
  ASTContext &C = AST->getASTContext();
  const FunctionDecl *FD = findFnByName(FnName, C);
  // The implicit declaration clang creates for a `__builtin_` spelling is not
  // reached by the AST visitor behind findFnByName, so fall back to scanning
  // the translation unit's own decls.
  if (!FD)
    for (const Decl *D : C.getTranslationUnitDecl()->decls())
      if (const auto *F = dyn_cast<FunctionDecl>(D);
          F && F->getIdentifier() && F->getName() == FnName)
        FD = F;
  EXPECT_NE(FD, nullptr) << FnName.str() << " not found";
  return FD && LibraryFunctionKnowledge::parameterDoesNotEscape(FD, Idx, C);
}

// The targets every non-deallocator row must hold on. The production lookup
// consults no triple, so a fact LLVM withholds on any of these is a fact this
// table may not state. `bcmp` (not on Darwin/Windows/PS), `stpcpy` (not on
// either Windows environment, nor PS) and `strnlen` (not on PS) were dropped
// for exactly this reason; without the loop, a host-only cross-check would
// happily re-accept them.
constexpr llvm::StringLiteral OracleTriples[] = {
    // One per mechanism by which TargetLibraryInfo.cpp can withhold an
    // inference, rather than an arbitrary sample of popular targets.
    "arm64-apple-macosx14.0.0",        // Darwin
    "x86_64-unknown-linux-gnu",        // glibc, 64-bit
    "i686-unknown-linux-gnu",          // glibc, 32-bit: size_t/int widths
    "aarch64-unknown-linux-musl",      // musl
    "aarch64-unknown-linux-android29", // Android (own setUnavailable block)
    "x86_64-unknown-freebsd",          // FreeBSD
    "powerpc64-ibm-aix7.2.0",          // AIX (own block)
    "x86_64-pc-windows-msvc",          // MSVC environment
    "x86_64-w64-windows-gnu",          // MinGW environment
    "x86_64-scei-ps4",                 // isPS() block
    "amdgcn-amd-amdhsa",               // no C runtime: disableAllFunctions()
    "nvptx64-nvidia-cuda",             // no C runtime: ditto, minus a handful
    "dxil-pc-shadermodel6.0-compute",  // no C runtime: ditto
};

// Pin the list itself. Without this, the coverage assertion in
// TableMatchesLLVMInference compares iterations over OracleTriples against a
// count derived from the same array, so *shrinking* the list passes.
//
// Adding a target is NOT automatically fine: if it fails a row, the question is
// which mechanism it exercises. A target with no C runtime belongs in
// `targetHasNoCRuntime` so `lookup` refuses it; a target whose libc merely
// lacks one function invalidates that row. Removing a target drops a mechanism
// from the sample and needs a reason. Either way, update this count
// deliberately.
static_assert(std::size(OracleTriples) == 13,
              "OracleTriples changed: the set of withholding mechanisms this "
              "table is checked against changed with it. Say which mechanism "
              "was added or dropped, and why.");

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
  EXPECT_TRUE(noEscape("printf", 0));
  EXPECT_TRUE(noEscape("strtol", 1));
  // `puts` and `fputs` are NOT here, though the table states facts for them:
  // clang does not model them as builtins, so the gate never reaches the row.
  // See RowsClangDoesNotModelAsBuiltinsAreUnreachable.
}

TEST_F(LibraryFunctionKnowledgeTest, ParametersLLVMDoesNotMarkAreNotTrusted) {
  EXPECT_FALSE(noEscape("memcpy", 0)); // only 'returned' in LLVM's model
  EXPECT_FALSE(noEscape("memset", 0));
  EXPECT_FALSE(noEscape("strchr", 0)); // result aliases the argument
  EXPECT_FALSE(noEscape("strcpy", 0));
  EXPECT_FALSE(noEscape("strtol", 0)); // stored through *end
  // `strlen` has one parameter, so index 1 is past the end. NOTE: this is
  // decided by the mask having no bit there, NOT by the `ParamIndex >= Arity`
  // bound in `parameterDoesNotEscape` -- deleting that bound leaves this case
  // green. The bound's own coverage is OutOfRangeParameterIndexIsRefused.
  EXPECT_FALSE(noEscape("strlen", 1));
}

// The `ParamIndex >= F->Arity` bound is not redundant with the mask, even
// though every row has zero bits at and above its arity. `NonCapturingParams`
// is a 32-bit `unsigned`, so a shift count of 32 or more is undefined
// behaviour, and on AArch64 the count is taken modulo 32 -- index 32 aliases
// bit 0, which is set for `printf`, and would report the argument as
// non-capturing. This is reachable: a consumer asking about a variadic argument
// of a `printf` call with 33 or more arguments.
TEST_F(LibraryFunctionKnowledgeTest, OutOfRangeParameterIndexIsRefused) {
  EXPECT_FALSE(noEscape("printf", 32));  // aliases bit 0 without the bound
  EXPECT_FALSE(noEscape("printf", 40));  // aliases bit 8: zero either way
  EXPECT_FALSE(noEscape("fprintf", 33)); // aliases bit 1, also set
  EXPECT_FALSE(noEscape("printf", 1));   // just past the end, no aliasing
  EXPECT_FALSE(noEscape("memcmp", 3));
  // Sanity: the in-range answers these are contrasted with.
  EXPECT_TRUE(noEscape("printf", 0));
  EXPECT_TRUE(noEscape("memcmp", 2 - 1));
}

TEST_F(LibraryFunctionKnowledgeTest, DeallocatorsAreExcluded) {
  EXPECT_TRUE(LibraryFunctionKnowledge::isDeallocationFunction(fn("free"), Ctx));
  EXPECT_FALSE(noEscape("free", 0));
  EXPECT_TRUE(
      LibraryFunctionKnowledge::isDeallocationFunction(fn("realloc"), Ctx));
  EXPECT_FALSE(noEscape("realloc", 0));
  EXPECT_FALSE(
      LibraryFunctionKnowledge::isDeallocationFunction(fn("strlen"), Ctx));
}

TEST_F(LibraryFunctionKnowledgeTest, UnknownFunctionsAreNotTrusted) {
  EXPECT_FALSE(noEscape("mine", 0));
  EXPECT_FALSE(LibraryFunctionKnowledge::isDeallocationFunction(fn("mine"), Ctx));
  // NOTE: this covers an UNKNOWN name, not the `isDefined()` refusal.
  // `defined_here` is not in the table, so it is refused by the table lookup and
  // never reaches that check. Coverage of `isDefined()` lives in
  // DefinedLibraryFunctionsAreNotTrusted, which uses table names.
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
  EXPECT_TRUE(
      LibraryFunctionKnowledge::parameterDoesNotEscape(Builtin, 0, Ctx));
}

// `-fno-builtin` zeroes the builtin id of the *library* spelling, so plain
// `strlen` is refused by the builtin-id gate too. The `__builtin_` spelling
// keeps its id under `-fno-builtin` (measured), so the `LangOpts.NoBuiltin`
// early-out is the only thing that refuses it -- which is what this test
// discriminates, and why the second case is here.
TEST_F(LibraryFunctionKnowledgeTest, NoBuiltinDisablesTrust) {
  std::unique_ptr<ASTUnit> NB = tooling::buildASTFromCodeWithArgs(
      Decls, {"-x", "c", "-std=c17", "-fno-builtin"});
  ASTContext &C2 = NB->getASTContext();
  EXPECT_FALSE(LibraryFunctionKnowledge::parameterDoesNotEscape(
      findFnByName("strlen", C2), 0, C2));

  constexpr const char *UsesBuiltin = R"c(
typedef __SIZE_TYPE__ size_t;
size_t u(const char *s) { return __builtin_strlen(s); }
)c";
  // Baseline: the compiler's own spelling is trusted when nothing opts out.
  EXPECT_TRUE(trustsParam(UsesBuiltin, "__builtin_strlen", 0));
  EXPECT_FALSE(trustsParam(UsesBuiltin, "__builtin_strlen", 0,
                           {"-x", "c", "-std=c17", "-fno-builtin"}));
}

// The per-function opt-out does NOT set LangOpts.NoBuiltin; it clears the
// builtin id of the library spelling only. So for plain `strlen` the builtin-id
// gate now also refuses it -- but `__builtin_strlen` keeps its id under
// `-fno-builtin-strlen` (measured), and there the `isNoBuiltinFunc` check is
// the only thing that refuses. That last case is what gives this test its
// power; without it, it would pass for the new reason rather than its own.
TEST_F(LibraryFunctionKnowledgeTest, NoBuiltinForOneFunctionDisablesTrust) {
  constexpr const char *Code = R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
size_t u(const char *s) { return __builtin_strlen(s); }
)c";
  const std::vector<std::string> NoStrlen = {"-x", "c", "-std=c17",
                                             "-fno-builtin-strlen"};
  // Baseline: without the opt-out all three spellings are trusted, so the
  // FALSEs below are the opt-out working and not the TU failing to build.
  EXPECT_TRUE(trustsParam(Code, "strlen", 0));
  EXPECT_TRUE(trustsParam(Code, "__builtin_strlen", 0));
  EXPECT_FALSE(trustsParam(Code, "strlen", 0, NoStrlen));
  // The load-bearing case: the id survives, so only the by-name opt-out refuses.
  EXPECT_FALSE(trustsParam(Code, "__builtin_strlen", 0, NoStrlen));
  // ...and only that one function loses trust.
  EXPECT_TRUE(trustsParam(Code, "strcmp", 0, NoStrlen));
  EXPECT_FALSE(trustsParam(Code, "strlen", 0,
                           {"-x", "c", "-std=c17", "-ffreestanding"}));
}

// Matching by name and arity is not enough: these declarations all spell a
// library name while denoting something else, and each one links cleanly. The
// `alias` and `__asm__` cases are the dangerous ones -- the call goes to a
// definition that may capture, and the parameter would be wrongly annotated
// `noescape` in shipped source.
TEST_F(LibraryFunctionKnowledgeTest, RedirectedOrShadowedNamesAreNotTrusted) {
  // Baseline: the same TU shape without the redirection IS trusted, so the
  // FALSEs below cannot all be some unrelated build failure.
  EXPECT_TRUE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s);
)c",
                          "strlen", 0));

  // alias: the call really goes to my_strlen, which captures. Pinned to an ELF
  // target because Darwin's Sema rejects `alias` outright ("aliases are not
  // supported on darwin") and would never attach the attribute, making the case
  // pass for a reason that has nothing to do with the gate.
  const std::vector<std::string> ELF = {"-x", "c", "-std=c17",
                                        "--target=x86_64-unknown-linux-gnu"};
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
static const char *g;
size_t my_strlen(const char *p) { g = p; return 0; }
size_t strlen(const char *s) __attribute__((alias("my_strlen")));
)c",
                           "strlen", 0, ELF));

  // Function multiversioning. Clang lowers this to an ifunc -- the IR for the
  // TU contains `@strlen = weak_odr ifunc ... @strlen.resolver` and no plain
  // `declare ... @strlen`, so LLVM infers nothing -- yet no IFuncAttr is ever
  // attached and the declaration is not a definition. Refused by the
  // `isMultiVersion()` predicate and by nothing else. ELF-pinned for the same
  // reason as the alias case.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
__attribute__((target_clones("avx2","default")))
size_t strlen(const char *s);
)c",
                           "strlen", 0, ELF));

  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
void *memcpy(void *d, const void *s, size_t n)
    __attribute__((target_clones("avx2","default")));
)c",
                           "memcpy", 1, ELF));

  // ifunc: same redirection, resolved at load time.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
static const char *g;
static size_t my_strlen(const char *p) { g = p; return 0; }
static void *resolve_strlen(void) { return (void *)my_strlen; }
size_t strlen(const char *s) __attribute__((ifunc("resolve_strlen")));
)c",
                           "strlen", 0, ELF));

  // Baseline for the ELF target too, so the two FALSEs above are the
  // redirection and not the target switch.
  EXPECT_TRUE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s);
)c",
                          "strlen", 0, ELF));

  // __asm__ label: same redirection, and common in libc shims and
  // symbol-versioning headers.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s) __asm__("my_strlen");
)c",
                           "strlen", 0));

  // overloadable: the name is one of several unrelated functions.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s) __attribute__((overloadable));
)c",
                           "strlen", 0));

  // weak: the symbol resolved at link time need not be the library's.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s) __attribute__((weak));
)c",
                           "strlen", 0));

  // The `static` shadow case moved to DeclarationsClangRejectsAreNotTrusted:
  // clang gives it a zero builtin id, so it is the builtin-id gate that closes
  // it, not any attribute rejection here.
}

// The three cases the attribute rejections above cannot see. In each, clang has
// already decided the declaration is not the library function and gives it a
// zero builtin id, so requiring the id is deference to clang's judgement rather
// than a rule of our own. These are attributable to the builtin-id gate: with
// it disabled, all three flip to trusted.
TEST_F(LibraryFunctionKnowledgeTest, DeclarationsClangRejectsAreNotTrusted) {
  // Baseline: the plain declaration is trusted.
  EXPECT_TRUE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s);
)c",
                          "strlen", 0));

  // A `static` shadow is a different function that shares a spelling.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
static size_t strlen(const char *s);
)c",
                           "strlen", 0));

  // `overloadable` gives the name a different mangling; it is one of several
  // unrelated functions.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s) __attribute__((overloadable));
)c",
                           "strlen", 0));

  // A prototype clang refuses for the library function: this earns
  // -Wincompatible-library-redeclaration and a zero builtin id. The arity is
  // deliberately the table's own (3), and the name matches, so name-and-arity
  // alone WOULD have trusted it -- this is the case the builtin-id gate, and
  // nothing else in `lookup`, is what closes.
  EXPECT_FALSE(trustsParam(R"c(
int memcmp(const char *a, const char *b, double n);
)c",
                           "memcmp", 0));

  // C++ without `extern "C"`: C++ linkage, so not the C library function.
  // NOTE: this is decided by the builtin-id gate, not by the extern-"C" check
  // in `lookup` -- Sema only attaches `BuiltinAttr` for C language linkage, so
  // the id is already 0 here. Deleting that check leaves this case green. It is
  // kept as belt-and-braces; see the comment there.
  EXPECT_FALSE(trustsParam(R"cpp(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s);
)cpp",
                           "strlen", 0, {"-x", "c++", "-std=c++17"}));
  // ...and with it, trusted again.
  EXPECT_TRUE(trustsParam(R"cpp(
typedef __SIZE_TYPE__ size_t;
extern "C" size_t strlen(const char *s);
)cpp",
                          "strlen", 0, {"-x", "c++", "-std=c++17"}));
}

// Requiring a builtin id costs trust in rows clang does not model as builtins.
// That cost is pinned here rather than left to be discovered: these four names
// are absent from clang's Builtins.td, so their rows are unreachable today.
// They stay in the table because the LLVM cross-check keeps proving them and
// because they would light up on their own if clang gained the builtin -- at
// which point this test fails and says so.
TEST_F(LibraryFunctionKnowledgeTest, RowsClangDoesNotModelAsBuiltinsAreUnreachable) {
  const std::set<std::string> ExpectedUnreachable = {"puts", "fputs", "atoi",
                                                    "reallocf"};
  constexpr const char *Code = R"c(
typedef __SIZE_TYPE__ size_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef struct __sFILE FILE;
size_t strlen(const char *s);
size_t wcslen(const wchar_t *s);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
int puts(const char *s);
int fputs(const char *s, FILE *f);
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int atoi(const char *s);
long strtol(const char *s, char **end, int base);
void free(void *p);
void *realloc(void *p, size_t n);
void *reallocf(void *p, size_t n);
)c";
  std::unique_ptr<ASTUnit> A =
      tooling::buildASTFromCodeWithArgs(Code, {"-x", "c", "-std=c17"});
  ASSERT_NE(A, nullptr);
  ASTContext &C = A->getASTContext();

  std::set<std::string> ActualUnreachable;
  for (const LibraryFunctionFact &F : libraryFunctionFacts()) {
    const FunctionDecl *D = findFnByName(F.Name, C);
    ASSERT_NE(D, nullptr) << F.Name.str() << " missing from the probe TU";
    if (D->getBuiltinID() == 0)
      ActualUnreachable.insert(F.Name.str());
  }
  // The set comparison is the guard with power here: it fails if a row joins or
  // leaves the unreachable set, and it failed on a delete-a-row mutation. A
  // count of loop iterations would be tautological, so there isn't one.
  EXPECT_EQ(ActualUnreachable, ExpectedUnreachable);

  // And the consequence, through the public entry point rather than by
  // inspecting builtin ids: unreachable rows are not trusted, reachable
  // non-deallocator rows are.
  for (const LibraryFunctionFact &F : libraryFunctionFacts()) {
    const FunctionDecl *D = findFnByName(F.Name, C);
    bool AnyTrusted = false;
    for (unsigned I = 0; I < F.Arity; ++I)
      AnyTrusted |= LibraryFunctionKnowledge::parameterDoesNotEscape(D, I, C);
    bool ShouldBeTrusted =
        !F.IsDeallocator && !ExpectedUnreachable.count(F.Name.str());
    EXPECT_EQ(AnyTrusted, ShouldBeTrusted) << F.Name.str();
  }
}

// A table name *defined* in this TU is analyzed, never trusted. This is the
// only test with power over the `isDefined()` refusal in `lookup`: an input has
// to survive name-and-arity matching before that check matters, so it must use
// a table name. A TU that both defines and calls one of these is not exotic --
// it is any libc built from its own sources, a freestanding implementation, or
// a tracing shim.
TEST_F(LibraryFunctionKnowledgeTest, DefinedLibraryFunctionsAreNotTrusted) {
  // Defined right here, and it captures.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
static const char *g;
size_t strlen(const char *s) { g = s; return 0; }
size_t f(const char *p) { return strlen(p); }
)c",
                           "strlen", 0));

  // A multi-parameter row, with the definition split from the declaration so
  // the refusal has to hold across the redeclaration chain rather than for
  // whichever decl the lookup happens to be handed.
  EXPECT_FALSE(trustsParam(R"c(
typedef __SIZE_TYPE__ size_t;
void *memcpy(void *d, const void *s, size_t n);
static const void *g;
void *memcpy(void *d, const void *s, size_t n) { g = s; return d; }
)c",
                           "memcpy", 1));

  // Baseline: the same two names, declared and not defined, ARE trusted -- so
  // the FALSEs above are the definition and nothing else about these TUs.
  constexpr const char *DeclOnly = R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s);
void *memcpy(void *d, const void *s, size_t n);
)c";
  EXPECT_TRUE(trustsParam(DeclOnly, "strlen", 0));
  EXPECT_TRUE(trustsParam(DeclOnly, "memcpy", 1));
}

// Pin the *environments* OracleTriples covers, not just its length. Deleting a
// triple to make a stubborn row green then fails here by name, which a size
// assertion alone would let through if someone swapped one triple for another.
TEST_F(LibraryFunctionKnowledgeTest, OracleTriplesCoverTheNamedEnvironments) {
  bool Darwin = false, Glibc = false, Musl = false, Android = false,
       FreeBSD = false, AIX = false, MSVC = false, MinGW = false, PS = false,
       Bits32 = false, AMDGPU = false, NVPTX = false, DXIL = false;
  for (llvm::StringLiteral S : OracleTriples) {
    llvm::Triple T{S};
    Darwin |= T.isOSDarwin();
    Glibc |= T.isOSLinux() && T.isGNUEnvironment();
    Musl |= T.isMusl();
    Android |= T.isAndroid();
    FreeBSD |= T.isOSFreeBSD();
    AIX |= T.isOSAIX();
    MSVC |= T.isWindowsMSVCEnvironment();
    MinGW |= T.isWindowsGNUEnvironment();
    PS |= T.isPS();
    Bits32 |= T.isArch32Bit();
    AMDGPU |= T.isAMDGPU();
    NVPTX |= T.isNVPTX();
    DXIL |= T.isDXIL();
  }
  EXPECT_TRUE(Darwin) << "no Darwin triple: `bcmp` could be re-added";
  EXPECT_TRUE(Glibc) << "no glibc triple";
  EXPECT_TRUE(Musl) << "no musl triple";
  EXPECT_TRUE(Android) << "no Android triple: it has its own block";
  EXPECT_TRUE(FreeBSD) << "no FreeBSD triple";
  EXPECT_TRUE(AIX) << "no AIX triple: it has its own block";
  EXPECT_TRUE(MSVC) << "no MSVC triple: `stpcpy` could be re-added";
  EXPECT_TRUE(MinGW) << "no MinGW triple: `stpcpy` could be re-added";
  EXPECT_TRUE(PS) << "no PS triple: `strnlen` could be re-added";
  EXPECT_TRUE(Bits32)
      << "no 32-bit triple: a size_t/int width bug in the oracle's prototypes "
         "would go unnoticed";
  // Each no-C-runtime class has its own early-out in TargetLibraryInfo.cpp, so
  // each is sampled separately -- and each must be classified as such.
  EXPECT_TRUE(AMDGPU) << "no AMDGPU triple";
  EXPECT_TRUE(NVPTX) << "no NVPTX triple";
  EXPECT_TRUE(DXIL) << "no DXIL triple";
  for (llvm::StringLiteral S : OracleTriples) {
    llvm::Triple T{S};
    EXPECT_EQ(targetHasNoCRuntime(T), T.isAMDGPU() || T.isNVPTX() || T.isDXIL())
        << S.str();
  }
}

// Targets with no C runtime. LLVM's `TargetLibraryInfoImpl` calls
// `disableAllFunctions()` for these, so its libcall inference grants nothing at
// all -- not `strlen`, not `memcpy`. The table states facts unconditionally, so
// without an explicit refusal the analysis would assume exactly what the
// optimizer refuses on the target being compiled for: the unsound direction.
// The concrete shape is a CUDA/HIP device pass where a device header declares
// `extern "C" size_t strlen(const char *)` over a tracing device runtime.
TEST_F(LibraryFunctionKnowledgeTest, NoCRuntimeTargetsAreNotTrusted) {
  constexpr const char *Code = R"c(
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *s);
void *memcpy(void *d, const void *s, size_t n);
)c";
  // Baseline: a host target trusts both, so the FALSEs below are the target.
  EXPECT_TRUE(trustsParam(Code, "strlen", 0));
  EXPECT_TRUE(trustsParam(Code, "memcpy", 1));

  for (const char *TS : {"amdgcn-amd-amdhsa", "nvptx64-nvidia-cuda",
                         "dxil-pc-shadermodel6.0-compute"}) {
    const std::vector<std::string> Args = {"-x", "c", "-std=c17",
                                           std::string("--target=") + TS};
    EXPECT_FALSE(trustsParam(Code, "strlen", 0, Args)) << TS;
    EXPECT_FALSE(trustsParam(Code, "memcpy", 1, Args)) << TS;
  }
}

// Every table row must agree with LLVM's own libcall inference, on every target
// the table claims to speak for. The table is the production artifact; LLVM is
// the oracle.
TEST_F(LibraryFunctionKnowledgeTest, TableMatchesLLVMInference) {
  llvm::LLVMContext LLVMCtx;
  ASSERT_FALSE(libraryFunctionFacts().empty());

  std::map<std::string, unsigned> TriplesChecked;
  unsigned ParamsCompared = 0;
  unsigned ExpectedParamComparisons = 0;

  for (llvm::StringLiteral TripleStr : OracleTriples) {
    llvm::Triple T{TripleStr};
    // A fresh module per triple: reusing one would rename the second `strlen`
    // to `strlen.1`, and `getLibFunc` would stop recognizing it. The triple and
    // data layout must be set, because `isValidProtoForLibFunc` derives the
    // width of `size_t` from the module's index size and the width of `int`
    // from the TLI -- hardcoding 64 would silently fail to recognize any
    // function on a 32-bit target.
    llvm::Module M("oracle", LLVMCtx);
    M.setTargetTriple(T);
    unsigned PtrBits = T.isArch64Bit() ? 64 : T.isArch16Bit() ? 16 : 32;
    M.setDataLayout("e-p:" + std::to_string(PtrBits) + ":" +
                    std::to_string(PtrBits));
    llvm::TargetLibraryInfoImpl TLIImpl{T};
    llvm::TargetLibraryInfo TLI(TLIImpl);

    auto *Ptr = llvm::PointerType::get(LLVMCtx, 0);
    auto *Void = llvm::Type::getVoidTy(LLVMCtx);
    auto *I64 = llvm::Type::getInt64Ty(LLVMCtx); // `Long` matches leniently
    auto *Int = llvm::IntegerType::get(LLVMCtx, TLIImpl.getIntSize());
    auto *SizeT = llvm::IntegerType::get(LLVMCtx, TLIImpl.getSizeTSize(M));

    // LLVM prototypes for each table entry. No `bcmp`, `stpcpy` or `strnlen`:
    // LLVM withholds inference for them on some of OracleTriples, so they are
    // not in the table either. See `Facts`.
    const std::map<std::string, llvm::FunctionType *> Protos = {
        {"strlen", llvm::FunctionType::get(SizeT, {Ptr}, false)},
        {"wcslen", llvm::FunctionType::get(SizeT, {Ptr}, false)},
        {"memcpy", llvm::FunctionType::get(Ptr, {Ptr, Ptr, SizeT}, false)},
        {"memmove", llvm::FunctionType::get(Ptr, {Ptr, Ptr, SizeT}, false)},
        {"memcmp", llvm::FunctionType::get(Int, {Ptr, Ptr, SizeT}, false)},
        {"strcmp", llvm::FunctionType::get(Int, {Ptr, Ptr}, false)},
        {"strncmp", llvm::FunctionType::get(Int, {Ptr, Ptr, SizeT}, false)},
        {"strcpy", llvm::FunctionType::get(Ptr, {Ptr, Ptr}, false)},
        {"strncpy", llvm::FunctionType::get(Ptr, {Ptr, Ptr, SizeT}, false)},
        {"strcat", llvm::FunctionType::get(Ptr, {Ptr, Ptr}, false)},
        {"strncat", llvm::FunctionType::get(Ptr, {Ptr, Ptr, SizeT}, false)},
        {"puts", llvm::FunctionType::get(Int, {Ptr}, false)},
        {"fputs", llvm::FunctionType::get(Int, {Ptr, Ptr}, false)},
        {"fwrite",
         llvm::FunctionType::get(SizeT, {Ptr, SizeT, SizeT, Ptr}, false)},
        {"printf", llvm::FunctionType::get(Int, {Ptr}, true)},
        {"fprintf", llvm::FunctionType::get(Int, {Ptr, Ptr}, true)},
        {"atoi", llvm::FunctionType::get(Int, {Ptr}, false)},
        {"strtol", llvm::FunctionType::get(I64, {Ptr, Ptr, Int}, false)},
        {"free", llvm::FunctionType::get(Void, {Ptr}, false)},
        {"realloc", llvm::FunctionType::get(Ptr, {Ptr, SizeT}, false)},
        {"reallocf", llvm::FunctionType::get(Ptr, {Ptr, SizeT}, false)},
    };
    ASSERT_EQ(libraryFunctionFacts().size(), Protos.size());

    // Targets with no C runtime at all: LLVM disables every libcall, so no row
    // is a fact here. `lookup` refuses these targets outright, which is what
    // makes the "every target" claim true rather than merely sampled.
    bool NoCRuntime = targetHasNoCRuntime(T);

    for (const LibraryFunctionFact &Fact : libraryFunctionFacts()) {
      auto It = Protos.find(Fact.Name.str());
      ASSERT_NE(It, Protos.end())
          << "no oracle prototype for " << Fact.Name.str();
      llvm::Function *F = llvm::Function::Create(
          It->second, llvm::GlobalValue::ExternalLinkage, Fact.Name, M);
      llvm::LibFunc LF = TLI.getLibFunc(*F);
      ASSERT_NE(LF, llvm::NotLibFunc)
          << Fact.Name.str() << " on " << TripleStr.str()
          << ": LLVM does not recognize the prototype (check int/size_t width)";

      // `inferNonMandatoryLibFuncAttrs` returns false without touching F when
      // this target does not provide the function. Every parameter then reads
      // as "LLVM does not say captures(none)". The per-parameter loop below
      // catches that for a row with at least one bit set -- but an all-zero
      // row would pass vacuously, and the distinction we need is "LLVM
      // disagrees" versus "LLVM declines to speak". Only the return value
      // tells them apart.
      bool Inferred = llvm::inferNonMandatoryLibFuncAttrs(*F, TLI);

      if (NoCRuntime) {
        // `lookup` refuses these targets outright, so no row is claimed here
        // and there is nothing to compare. What must still hold is that the
        // classification is honest: no row we would ever *trust* may be a fact
        // on a target we call runtime-less. A handful of functions can survive
        // the blanket disable -- NVPTX re-enables `malloc` and `free` -- but
        // `free` is a deallocator, refused everywhere regardless. A
        // non-deallocator row surviving here would mean this target does have
        // a partial C runtime and does not belong in `targetHasNoCRuntime`.
        EXPECT_TRUE(Fact.IsDeallocator || !Inferred)
            << Fact.Name.str() << " is inferred on " << TripleStr.str()
            << ", which targetHasNoCRuntime classifies as runtime-less";
        continue;
      }

      if (!Inferred) {
        EXPECT_TRUE(Fact.IsDeallocator)
            << Fact.Name.str() << ": LLVM infers nothing on " << TripleStr.str()
            << ", so the table must not state a capture fact for it. Either "
               "this target has no C runtime (teach targetHasNoCRuntime about "
               "it, so `lookup` refuses it) or the row is invalid there.";
        continue;
      }
      ++TriplesChecked[Fact.Name.str()];
      ExpectedParamComparisons += Fact.Arity;

      bool IsDealloc = llvm::isLibFreeFunction(F, LF) || llvm::isReallocLikeFn(F);
      EXPECT_EQ(Fact.IsDeallocator, IsDealloc)
          << Fact.Name.str() << " on " << TripleStr.str();
      EXPECT_EQ(Fact.Arity, F->arg_size())
          << Fact.Name.str() << " on " << TripleStr.str();
      for (unsigned I = 0; I < F->arg_size(); ++I) {
        // `Captures` present with no "other" components is exactly LLVM's
        // `isRetOnly()`, so a future row LLVM marked `captures(ret: address)`
        // would satisfy this too. That is sound only under the project-wide
        // rule that every pointer-carrying call result with an alias argument
        // is itself an alias: an argument captured into the return value stays
        // tracked through the result rather than being lost. If that rule ever
        // weakens, this check must tighten to full `capturesNothing`.
        bool LLVMSays =
            F->hasParamAttribute(I, llvm::Attribute::Captures) &&
            llvm::capturesNothing(F->getParamAttribute(I, llvm::Attribute::Captures)
                                      .getCaptureInfo()
                                      .getOtherComponents());
        bool TableSays = (Fact.NonCapturingParams >> I) & 1u;
        EXPECT_EQ(TableSays, LLVMSays)
            << Fact.Name.str() << " param " << I << " on " << TripleStr.str();
        ++ParamsCompared;
      }
    }
  }

  // Anti-vacuity with actual power: the counter advances once per parameter of
  // LLVM's prototype, while the expectation sums the table's own Arity. A row
  // whose stated arity disagrees with LLVM diverges here, and a table truncated
  // to nothing fails the EXPECT_GT.
  EXPECT_EQ(ParamsCompared, ExpectedParamComparisons);
  EXPECT_GT(ParamsCompared, 0u);

  // Count the triples that do have a C runtime; rows are only claimed there.
  unsigned RuntimeTriples = 0;
  for (llvm::StringLiteral S : OracleTriples)
    if (!targetHasNoCRuntime(llvm::Triple(S)))
      ++RuntimeTriples;
  ASSERT_GT(RuntimeTriples, 0u);

  // Every non-deallocator row must be a fact on every C-runtime triple.
  // Deallocators only have to be confirmed as deallocators somewhere, since
  // they are refused on every target whether or not LLVM speaks for them.
  for (const LibraryFunctionFact &Fact : libraryFunctionFacts()) {
    unsigned N = TriplesChecked[Fact.Name.str()];
    if (Fact.IsDeallocator)
      EXPECT_GT(N, 0u) << Fact.Name.str() << " was verified on no triple";
    else
      EXPECT_EQ(N, RuntimeTriples)
          << Fact.Name.str() << " was verified on only " << N << " of "
          << RuntimeTriples << " C-runtime triples";
  }
}

} // namespace
