//===- InferNoescapeTest.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/SourceTransformation/Transformations/InferNoescape.h"
#include "FindDecl.h"
#include "TestFixture.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/Basic/Sarif.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/PCHContainerOperations.h"
#include "clang/Frontend/SSAFOptions.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"
#include "clang/ScalableStaticAnalysis/Analyses/SharedLexicalRepresentation/SharedLexicalRepresentation.h"
#include "clang/ScalableStaticAnalysis/Core/ASTEntityMapping.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityIdTable.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/WPASuite.h"
#include "clang/ScalableStaticAnalysis/SourceTransformation/SourceEditEmitter.h"
#include "clang/ScalableStaticAnalysis/SourceTransformation/TransformationReportEmitter.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "gtest/gtest.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::ssaf;

namespace {

class RecordingEditEmitter : public SourceEditEmitter {
public:
  std::vector<tooling::Replacement> Replacements;

  void addReplacement(tooling::Replacement R) override {
    Replacements.push_back(std::move(R));
  }
};

class RecordingReportEmitter : public TransformationReportEmitter {
public:
  struct Entry {
    std::string RuleId;
    SarifResultLevel Level;
    std::string Message;
    CharSourceRange Range;
  };
  std::vector<Entry> Results;

  void addResult(StringRef RuleId, SarifResultLevel Level,
                 CharSourceRange Range, StringRef Message) override {
    Results.push_back({RuleId.str(), Level, Message.str(), Range});
  }
};

constexpr llvm::StringLiteral TestCompilationUnitId = "test-cu";
constexpr llvm::StringLiteral TestLinkUnitId = "test-lu";

struct Captured {
  std::string Rewritten;
  std::vector<RecordingReportEmitter::Entry> Reports;

  size_t count(llvm::StringRef Rule) const {
    return llvm::count_if(
        Reports, [&](const RecordingReportEmitter::Entry &E) {
          return E.RuleId == Rule;
        });
  }

  /// The message of the first report carrying \p Rule, or the empty string.
  /// Total on purpose: a probe that weakens a guard must report a clean
  /// failed expectation rather than crash on an empty container.
  std::string firstMessage(llvm::StringRef Rule) const {
    for (const RecordingReportEmitter::Entry &E : Reports)
      if (E.RuleId == Rule)
        return E.Message;
    return std::string();
  }

  /// The range of the first report carrying \p Rule.
  CharSourceRange firstRange(llvm::StringRef Rule) const {
    for (const RecordingReportEmitter::Entry &E : Reports)
      if (E.RuleId == Rule)
        return E.Range;
    return CharSourceRange();
  }

  /// The level of the first report carrying \p Rule.
  SarifResultLevel levelOf(llvm::StringRef Rule) const {
    for (const RecordingReportEmitter::Entry &E : Reports)
      if (E.RuleId == Rule)
        return E.Level;
    return SarifResultLevel::None;
  }
};

class InferNoescapeTest : public TestFixture {
protected:
  NestedBuildNamespace TUNs =
      NestedBuildNamespace::makeCompilationUnit(TestCompilationUnitId);
  NestedBuildNamespace LUNs =
      NestedBuildNamespace::makeLinkUnit(TestLinkUnitId);

  /// Parses \p Code, seeds NonEscapingParametersResult with \p Indices for
  /// the function named \p Fn, runs the transformation, and returns the
  /// rewritten "input.cc" plus the report entries.
  Captured run(llvm::StringRef Code, llvm::StringRef Fn,
               std::vector<unsigned> Indices,
               std::vector<std::string> Args = {"-std=c++20"},
               llvm::StringRef Spelling = "",
               tooling::FileContentMappings Files = {}) {
    std::unique_ptr<ASTUnit> AST = tooling::buildASTFromCodeWithArgs(
        Code, Args, "input.cc", "clang-tool",
        std::make_shared<PCHContainerOperations>(),
        tooling::getClangStripDependencyFileAdjuster(), Files);
    ASTContext &Ctx = AST->getASTContext();

    WPASuite Suite = makeWPASuite();
    auto Result = std::make_unique<NonEscapingParametersResult>();
    const FunctionDecl *FD = findFnByName(Fn, Ctx);
    EXPECT_NE(FD, nullptr);
    if (FD) {
      std::optional<EntityName> Name = getQualifiedEntityName(FD, TUNs, LUNs);
      EXPECT_TRUE(Name.has_value());
      if (Name) {
        EntityId Id = getIdTable(Suite).getId(*Name);
        for (unsigned I : Indices)
          Result->NonEscaping[Id].insert(I);
      }
    }
    getData(Suite)[NonEscapingParametersResult::analysisName()] =
        std::move(Result);

    return apply(Code, Ctx, Suite, Spelling);
  }

  /// Runs the transformation over an already-seeded \p Suite and applies the
  /// replacements that land in "input.cc" to \p Code.
  Captured apply(llvm::StringRef Code, ASTContext &Ctx, const WPASuite &Suite,
                 llvm::StringRef Spelling = "") {
    RecordingEditEmitter Edits;
    RecordingReportEmitter Report;
    SSAFOptions Opts;
    Opts.CompilationUnitId = TestCompilationUnitId.str();
    Opts.LinkUnitId = TestLinkUnitId.str();
    Opts.NoescapeSpelling = Spelling.str();
    InferNoescape(Suite, Opts, Edits, Report).HandleTranslationUnit(Ctx);

    tooling::Replacements Rs;
    for (const tooling::Replacement &R : Edits.Replacements)
      if (R.getFilePath() == "input.cc")
        llvm::cantFail(Rs.add(R));
    return {llvm::cantFail(tooling::applyAllReplacements(Code, Rs)),
            std::move(Report.Results)};
  }
};

//===----------------------------------------------------------------------===//
// Insertion
//===----------------------------------------------------------------------===//

TEST_F(InferNoescapeTest, PrefixInsertionOnNamedParameter) {
  Captured C = run("void f(int *p, int n) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p, int n) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 0u);
  EXPECT_EQ(C.levelOf(NoescapeInsertedRuleId), SarifResultLevel::Note);
}

TEST_F(InferNoescapeTest, EveryRedeclarationIsEdited) {
  Captured C =
      run("void f(int *p);\nvoid f(int *q);\nvoid f(int *r) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p);\n"
                         "void f(__attribute__((noescape)) int *q);\n"
                         "void f(__attribute__((noescape)) int *r) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 3u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 0u);
}

TEST_F(InferNoescapeTest, UnnamedFunctionPointerArrayAndReferenceParameters) {
  Captured C =
      run("void f(int *, void (*cb)(int), int &r, int a[]) { }", "f",
          {0, 1, 2, 3});
  EXPECT_EQ(C.Rewritten,
            "void f(__attribute__((noescape)) int *, "
            "__attribute__((noescape)) void (*cb)(int), "
            "__attribute__((noescape)) int &r, "
            "__attribute__((noescape)) int a[]) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 4u);
}

TEST_F(InferNoescapeTest, OnlyTheListedParameterIsAnnotated) {
  Captured C = run("void f(int *a, int *b, int *c) { }", "f", {1});
  EXPECT_EQ(C.Rewritten,
            "void f(int *a, __attribute__((noescape)) int *b, int *c) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
}

TEST_F(InferNoescapeTest, SpellingOverride) {
  Captured C =
      run("void f(int *p) { }", "f", {0}, {"-std=c++20"}, "[[clang::noescape]]");
  EXPECT_EQ(C.Rewritten, "void f([[clang::noescape]] int *p) { }");
}

TEST_F(InferNoescapeTest, DefaultSpellingIsGNUInC) {
  Captured C = run("void f(int *p) { }", "f", {0}, {"-x", "c", "-std=c17"});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p) { }");
}

TEST_F(InferNoescapeTest, OutOfLineMemberDefinitionAndInClassDeclaration) {
  Captured C = run("struct S { void m(int *p); };\nvoid S::m(int *p) { }",
                   "m", {0});
  EXPECT_EQ(C.Rewritten,
            "struct S { void m(__attribute__((noescape)) int *p); };\n"
            "void S::m(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 2u);
}

//===----------------------------------------------------------------------===//
// Skips
//===----------------------------------------------------------------------===//

TEST_F(InferNoescapeTest, AlreadyAnnotatedIsSilentlySkipped) {
  Captured C = run("void f(__attribute__((noescape)) int *p) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 0u);
  EXPECT_TRUE(C.Reports.empty());
}

// API Notes give a parameter a NoEscapeAttr with an empty SourceRange
// without the attribute appearing in the header bytes. Asking the AST whether
// the site is annotated would suppress a required edit *and* the report that
// section 5.5's pipeline backstop refuses on, so the decision is made on the
// source text. NoEscapeAttr::CreateImplicit reproduces exactly that shape.
TEST_F(InferNoescapeTest, AttributeNotWrittenInTheSourceIsReportedNotSilent) {
  llvm::StringRef Code = "void f(int *p);\nvoid f(int *p) { }";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  // Annotate the first declaration the way API Notes would: in the AST only.
  const FunctionDecl *FD = findFnByName("f", Ctx);
  ASSERT_NE(FD, nullptr);
  const FunctionDecl *First = FD->getFirstDecl();
  ASSERT_FALSE(First->doesThisDeclarationHaveABody())
      << "the declaration, not the definition";
  ParmVarDecl *P = const_cast<ParmVarDecl *>(First->getParamDecl(0));
  P->addAttr(NoEscapeAttr::CreateImplicit(Ctx));
  ASSERT_TRUE(P->hasAttr<NoEscapeAttr>());
  ASSERT_TRUE(P->getAttr<NoEscapeAttr>()->getRange().isInvalid());

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  Result->NonEscaping[getIdTable(Suite).getId(
                          *getQualifiedEntityName(FD, TUNs, LUNs))]
      .insert(0);
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  // The definition is still edited; the unwritten site is reported, never
  // dropped in silence.
  EXPECT_EQ(C.Rewritten,
            "void f(int *p);\nvoid f(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_EQ(C.levelOf(NoescapeSkippedRuleId), SarifResultLevel::Warning);
  EXPECT_NE(C.firstMessage(NoescapeSkippedRuleId).find("not written in this"),
            std::string::npos);
}

// An attribute written after the declarator falls outside the ParmVarDecl's
// own SourceRange, so containment is tested against the whole declaration.
// The bytes carry the annotation, so this is the silent skip, not a report.
TEST_F(InferNoescapeTest, AttributeWrittenAfterTheDeclaratorIsSilent) {
  Captured C = run("void f(int *p __attribute__((noescape))) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(int *p __attribute__((noescape))) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 0u);
  EXPECT_TRUE(C.Reports.empty());
}

TEST_F(InferNoescapeTest, MacroParameterIsSkippedAndReported) {
  Captured C = run("#define P int *p\nvoid f(P) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "#define P int *p\nvoid f(P) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_EQ(C.levelOf(NoescapeSkippedRuleId), SarifResultLevel::Warning);
  EXPECT_NE(C.firstMessage(NoescapeSkippedRuleId).find("macro expansion"),
            std::string::npos);
}

// Lexer::getAsCharRange cannot measure the parameter's last token when the
// expansion continues past it, and the SARIF writer drops a location-less
// result. Section 5.5's policy is keyed on locations, so a skip must always
// carry one.
TEST_F(InferNoescapeTest, SkipInAMultiParameterMacroStillCarriesALocation) {
  Captured C = run("#define PARAMS int *p, int n\nvoid f(PARAMS) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "#define PARAMS int *p, int n\nvoid f(PARAMS) { }");
  ASSERT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_TRUE(C.firstRange(NoescapeSkippedRuleId).isValid())
      << "a location-less skip is dropped by the SARIF writer";
}

TEST_F(InferNoescapeTest, SystemHeaderRedeclIsSkippedAndReported) {
  tooling::FileContentMappings Files = {
      {"/sys/s.h", "#pragma clang system_header\nvoid f(int *p);\n"}};
  Captured C = run("#include <s.h>\nvoid f(int *p) { }", "f", {0},
                   {"-std=c++20", "-isystem/sys"}, "", Files);
  EXPECT_EQ(C.Rewritten,
            "#include <s.h>\nvoid f(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_EQ(C.levelOf(NoescapeSkippedRuleId), SarifResultLevel::Warning);
  EXPECT_NE(C.firstMessage(NoescapeSkippedRuleId).find("system header"),
            std::string::npos);
}

// A redeclaration written through a function typedef has no written parameter
// list at all: clang synthesizes an implicit ParmVarDecl whose location is the
// function's own name. Editing there would corrupt `FT g;`, and resolving the
// site through the declaration's TypeSourceInfo instead would land inside the
// typedef and annotate every function declared through it.
TEST_F(InferNoescapeTest, TypedefDeclaredRedeclHasNoWrittenParameterList) {
  Captured C = run("typedef void FT(int *p);\nFT f;\nvoid f(int *p) { }", "f",
                   {0}, {"-x", "c", "-std=c17"});
  EXPECT_EQ(C.Rewritten, "typedef void FT(int *p);\nFT f;\n"
                         "void f(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_NE(
      C.firstMessage(NoescapeSkippedRuleId).find("no written parameter"),
      std::string::npos);
}

// An unprototyped C declaration has no parameter list to annotate. The
// definition is still edited; the unprototyped site is reported.
TEST_F(InferNoescapeTest, UnprototypedRedeclIsSkippedAndReported) {
  Captured C = run("void f();\nvoid f(int *p) { }", "f", {0},
                   {"-x", "c", "-std=c17"});
  EXPECT_EQ(C.Rewritten,
            "void f();\nvoid f(__attribute__((noescape)) int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_EQ(C.levelOf(NoescapeSkippedRuleId), SarifResultLevel::Warning);
  EXPECT_NE(C.firstMessage(NoescapeSkippedRuleId).find("no parameter at"),
            std::string::npos);
}

// A K&R definition is prototypeless: its ParmVarDecls sit in the declaration
// list after the parenthesis, not in a parameter list, so an insertion there
// would not produce a prototype.
TEST_F(InferNoescapeTest, KAndRDefinitionIsSkippedAndReported) {
  Captured C = run("void f(p) int *p; { }", "f", {0},
                   {"-x", "c", "-std=c17", "-Wno-deprecated-non-prototype"});
  EXPECT_EQ(C.Rewritten, "void f(p) int *p; { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_NE(
      C.firstMessage(NoescapeSkippedRuleId).find("no written prototype"),
      std::string::npos);
}

// Sema's handleNoEscapeAttr diagnoses a noescape on a type that cannot carry
// it and does *not* add the attribute, so an edit there is
// -Wignored-attributes in every translation unit that sees the declaration,
// an error under -Werror, and not idempotent on a later run. Candidacy never
// proposes such an index, which is exactly why it is rejected here rather
// than trusted.
TEST_F(InferNoescapeTest, NonAnnotatableParameterTypeIsSkippedAndReported) {
  Captured C = run("void f(int *p, int n) { }", "f", {1});
  EXPECT_EQ(C.Rewritten, "void f(int *p, int n) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_EQ(C.levelOf(NoescapeSkippedRuleId), SarifResultLevel::Warning);
  EXPECT_NE(C.firstMessage(NoescapeSkippedRuleId).find("type cannot carry"),
            std::string::npos);
}

// The predicate is clang's own, so every type handleNoEscapeAttr accepts is
// still annotated: pointer, reference, block pointer, and record -- the last
// being the one a narrower "pointer-like only" test would wrongly reject.
TEST_F(InferNoescapeTest, EveryTypeSemaAcceptsIsStillAnnotated) {
  Captured C = run("struct S { int x; };\n"
                   "void f(int *a, int &b, struct S c, void (^d)(void)) { }",
                   "f", {0, 1, 2, 3}, {"-std=c++20", "-fblocks"});
  EXPECT_EQ(C.Rewritten,
            "struct S { int x; };\n"
            "void f(__attribute__((noescape)) int *a, "
            "__attribute__((noescape)) int &b, "
            "__attribute__((noescape)) struct S c, "
            "__attribute__((noescape)) void (^d)(void)) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 4u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 0u);
}

// A parameter index past the end of a site's parameter list can only come from
// a result file that does not match this source. It is reported, never
// applied to whatever parameter happens to sit at another index.
TEST_F(InferNoescapeTest, OutOfRangeParameterIndexIsReported) {
  Captured C = run("void f(int *p) { }", "f", {7});
  EXPECT_EQ(C.Rewritten, "void f(int *p) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
  EXPECT_EQ(C.count(NoescapeSkippedRuleId), 1u);
  EXPECT_NE(C.firstMessage(NoescapeSkippedRuleId).find("no parameter at"),
            std::string::npos);
}

// A template pattern's parameter list is the only written one a function
// template has: an instantiation's ParmVarDecls point back into it. Editing
// there would annotate every instantiation from one proof, so neither the
// pattern nor an instantiation may be annotated -- and M1 candidacy excludes
// both. Seeding the result with either entity must still change nothing.
//
// The two halves are decided at different layers, which matters when reading
// this as coverage. The *pattern* half is what pins isTemplated(). The
// *instantiation* half is decided one layer earlier, by
// DynamicRecursiveASTVisitor's ShouldVisitTemplateInstantiations defaulting
// to false, so an implicit instantiation is never traversed at all and
// removing the TemplateSpecializationKind disjunct leaves this test green.
// What that disjunct actually stops is *explicit* specializations and
// explicit instantiations, which are pinned by their own two tests below.
TEST_F(InferNoescapeTest, NeitherTemplatePatternNorInstantiationIsAnnotated) {
  llvm::StringRef Code = "template <typename T> void g(T *p) { }\n"
                         "void f(int *p) { g(p); }";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  // findDeclByName skips templated decls, so this is the instantiation.
  const FunctionDecl *Inst = findFnByName("g", Ctx);
  ASSERT_NE(Inst, nullptr);
  EXPECT_FALSE(Inst->isTemplated());
  const FunctionDecl *Pattern = Inst->getTemplateInstantiationPattern();
  ASSERT_NE(Pattern, nullptr);
  EXPECT_TRUE(Pattern->isTemplated());

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  for (const FunctionDecl *FD : {Inst, Pattern}) {
    std::optional<EntityName> Name = getQualifiedEntityName(FD, TUNs, LUNs);
    ASSERT_TRUE(Name.has_value());
    Result->NonEscaping[getIdTable(Suite).getId(*Name)].insert(0);
  }
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  EXPECT_EQ(C.Rewritten, Code);
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
}

// Decl::isTemplated() is isDependentContext(), which is false for an
// explicit function-template specialization: it has a written parameter list
// and would be edited, but its parameter types must match the primary
// template, so the edit is `error: no function template matches function
// template specialization`.
TEST_F(InferNoescapeTest, ExplicitFunctionTemplateSpecializationIsNotEdited) {
  llvm::StringRef Code = "template <typename T> void f(T *p);\n"
                         "template <> void f<int>(int *p);\n"
                         "template <> void f<int>(int *p) { (void)p; }";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  const FunctionDecl *Spec = findFnByName("f", Ctx);
  ASSERT_NE(Spec, nullptr);
  EXPECT_FALSE(Spec->isTemplated()) << "not a dependent context";
  EXPECT_EQ(Spec->getTemplateSpecializationKind(), TSK_ExplicitSpecialization);

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  Result->NonEscaping[getIdTable(Suite).getId(
                          *getQualifiedEntityName(Spec, TUNs, LUNs))]
      .insert(0);
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  EXPECT_EQ(C.Rewritten, Code);
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
}

// The same for an out-of-line member specialization, whose edit is
// `error: conflicting types for 'm'`.
TEST_F(InferNoescapeTest, OutOfLineMemberSpecializationIsNotEdited) {
  llvm::StringRef Code = "template <typename T> struct A { void m(T *p); };\n"
                         "template <> void A<int>::m(int *p) { (void)p; }";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  const FunctionDecl *Spec = findFnByName("m", Ctx);
  ASSERT_NE(Spec, nullptr);
  EXPECT_FALSE(Spec->isTemplated());
  EXPECT_NE(Spec->getTemplateSpecializationKind(), TSK_Undeclared);

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  Result->NonEscaping[getIdTable(Suite).getId(
                          *getQualifiedEntityName(Spec, TUNs, LUNs))]
      .insert(0);
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  EXPECT_EQ(C.Rewritten, Code);
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 0u);
}

TEST_F(InferNoescapeTest, NonTemplateCallerOfATemplateIsStillAnnotated) {
  Captured C = run("template <typename T> void g(T *p) { }\n"
                   "void f(int *p) { g(p); }",
                   "f", {0});
  EXPECT_EQ(C.Rewritten, "template <typename T> void g(T *p) { }\n"
                         "void f(__attribute__((noescape)) int *p) { g(p); }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
}

TEST_F(InferNoescapeTest, FunctionsAbsentFromTheResultAreUntouched) {
  Captured C = run("void f(int *p) { }\nvoid other(int *q) { }", "f", {0});
  EXPECT_EQ(C.Rewritten, "void f(__attribute__((noescape)) int *p) { }\n"
                         "void other(int *q) { }");
  EXPECT_EQ(C.count(NoescapeInsertedRuleId), 1u);
}

TEST_F(InferNoescapeTest, MissingResultIsANoOp) {
  std::unique_ptr<ASTUnit> AST = tooling::buildASTFromCodeWithArgs(
      "void f(int *p) { }", {"-std=c++20"}, "input.cc");
  WPASuite Suite = makeWPASuite();
  Captured C = apply("void f(int *p) { }", AST->getASTContext(), Suite);
  EXPECT_EQ(C.Rewritten, "void f(int *p) { }");
  EXPECT_TRUE(C.Reports.empty());
}

//===----------------------------------------------------------------------===//
// Rejections
//===----------------------------------------------------------------------===//

TEST_F(InferNoescapeTest, RejectedCandidatesAreReportedAtTheDefinition) {
  llvm::StringRef Code = "void f(int *p);\nvoid f(int *p) { }";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  const FunctionDecl *FD = findFnByName("f", Ctx);
  ASSERT_NE(FD, nullptr);
  EntityId Id = getIdTable(Suite).getId(*getQualifiedEntityName(FD, TUNs, LUNs));
  Result->Rejected[Node{Id, 0}] = {EscapeReason::StoreToGlobal,
                                   SourceLocationRecord{"input.cc", 2, 20}, "g",
                                   std::nullopt};
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  EXPECT_EQ(C.Rewritten, Code);
  ASSERT_EQ(C.Reports.size(), 1u) << "once, at the definition only";
  EXPECT_EQ(C.Reports[0].RuleId, NoescapeRejectedRuleId);
  EXPECT_EQ(C.Reports[0].Level, SarifResultLevel::Note);
  EXPECT_NE(C.Reports[0].Message.find("StoreToGlobal"), std::string::npos);
  EXPECT_NE(C.Reports[0].Message.find("input.cc:2:20"), std::string::npos);
  EXPECT_NE(C.Reports[0].Message.find("(g)"), std::string::npos);
}

TEST_F(InferNoescapeTest, RejectionBlameIsRendered) {
  llvm::StringRef Code = "void callee(int *q);\nvoid f(int *p) { callee(p); }";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  const FunctionDecl *F = findFnByName("f", Ctx);
  const FunctionDecl *Callee = findFnByName("callee", Ctx);
  ASSERT_NE(F, nullptr);
  ASSERT_NE(Callee, nullptr);
  EntityId FId = getIdTable(Suite).getId(*getQualifiedEntityName(F, TUNs, LUNs));
  EntityId CId =
      getIdTable(Suite).getId(*getQualifiedEntityName(Callee, TUNs, LUNs));
  Result->Rejected[Node{FId, 0}] = {EscapeReason::EscapesViaCallee,
                                    SourceLocationRecord{"input.cc", 2, 18}, "",
                                    Node{CId, 0}};
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  ASSERT_EQ(C.Reports.size(), 1u);
  EXPECT_NE(C.Reports[0].Message.find("EscapesViaCallee"), std::string::npos);
  EXPECT_NE(C.Reports[0].Message.find("via callee parameter 0"),
            std::string::npos);
  // This rejection's Detail is empty; it must be omitted, not rendered as an
  // empty pair of parentheses in the precision-tuning feed.
  EXPECT_EQ(C.Reports[0].Message.find(" ()"), std::string::npos);
}

// A rejection whose function has no definition in this TU is not reported
// here: the defining TU's pass reports it, exactly once.
TEST_F(InferNoescapeTest, RejectionIsNotReportedWithoutADefinition) {
  llvm::StringRef Code = "void f(int *p);\n";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  const FunctionDecl *FD = findFnByName("f", Ctx);
  ASSERT_NE(FD, nullptr);
  EntityId Id = getIdTable(Suite).getId(*getQualifiedEntityName(FD, TUNs, LUNs));
  Result->Rejected[Node{Id, 0}] = {EscapeReason::StoreToGlobal,
                                   SourceLocationRecord{"input.cc", 1, 6}, "",
                                   std::nullopt};
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  EXPECT_TRUE(C.Reports.empty());
}

// ThisParamIndex is representable in a Rejection's Node but names no written
// parameter, so it must not be rendered against getParamDecl(-1).
TEST_F(InferNoescapeTest, ThisParameterRejectionIsNotReported) {
  llvm::StringRef Code = "struct S { void m(int *p) { } };";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  const FunctionDecl *FD = findFnByName("m", Ctx);
  ASSERT_NE(FD, nullptr);
  EntityId Id = getIdTable(Suite).getId(*getQualifiedEntityName(FD, TUNs, LUNs));
  Result->Rejected[Node{Id, ThisParamIndex}] = {
      EscapeReason::StoreToGlobal, SourceLocationRecord{"input.cc", 1, 29}, "",
      std::nullopt};
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  EXPECT_TRUE(C.Reports.empty());
}

// A rejection index past the end of the parameter list can only come from a
// result file that does not describe this source.
TEST_F(InferNoescapeTest, OutOfRangeRejectionIndexIsNotReported) {
  llvm::StringRef Code = "void f(int *p) { }";
  std::unique_ptr<ASTUnit> AST =
      tooling::buildASTFromCodeWithArgs(Code, {"-std=c++20"}, "input.cc");
  ASTContext &Ctx = AST->getASTContext();

  WPASuite Suite = makeWPASuite();
  auto Result = std::make_unique<NonEscapingParametersResult>();
  const FunctionDecl *FD = findFnByName("f", Ctx);
  ASSERT_NE(FD, nullptr);
  EntityId Id = getIdTable(Suite).getId(*getQualifiedEntityName(FD, TUNs, LUNs));
  Result->Rejected[Node{Id, 7}] = {EscapeReason::StoreToGlobal,
                                   SourceLocationRecord{"input.cc", 1, 18}, "",
                                   std::nullopt};
  getData(Suite)[NonEscapingParametersResult::analysisName()] =
      std::move(Result);

  Captured C = apply(Code, Ctx, Suite);
  EXPECT_TRUE(C.Reports.empty());
}

} // namespace
