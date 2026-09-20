//===- CallSiteResolverTest.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Every assertion about Arguments, ImplicitObjectArg and UnmatchedArgs binds
//  to the identity of a named sub-expression of the call. Checking only
//  parameter indices and container sizes would not distinguish a correct
//  pairing from a permuted one.
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/CallSiteResolution.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringRef.h"
#include "gtest/gtest.h"
#include <memory>

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
  const auto *CE = findFirst<CallExpr>(P.ctx());
  ASSERT_NE(CE, nullptr);
  ASSERT_EQ(CE->getNumArgs(), 2u);
  auto CS = resolveCallSite(CE);
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  EXPECT_EQ(CS->Callee->getNameAsString(), "g");
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 2u);
  EXPECT_EQ(CS->Arguments[0].first, CE->getArg(0));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_EQ(CS->Arguments[1].first, CE->getArg(1));
  EXPECT_EQ(CS->Arguments[1].second, 1u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, MemberCallExposesImplicitObject) {
  Parsed P =
      parseCXX("struct S { void m(int *); }; void f(S s, int *p) { s.m(p); }");
  const auto *MCE = findFirst<CXXMemberCallExpr>(P.ctx());
  ASSERT_NE(MCE, nullptr);
  ASSERT_EQ(MCE->getNumArgs(), 1u);
  auto CS = resolveCallSite(MCE);
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  EXPECT_EQ(CS->Callee->getNameAsString(), "m");
  EXPECT_EQ(CS->ImplicitObjectArg, MCE->getImplicitObjectArgument());
  EXPECT_NE(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 1u);
  EXPECT_EQ(CS->Arguments[0].first, MCE->getArg(0));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, MemberOperatorCallShiftsFirstArgument) {
  Parsed P = parseCXX(
      "struct S { int *operator+(int *); }; void f(S s, int *p) { s + p; }");
  const auto *OCE = findFirst<CXXOperatorCallExpr>(P.ctx());
  ASSERT_NE(OCE, nullptr);
  ASSERT_EQ(OCE->getNumArgs(), 2u);
  auto CS = resolveCallSite(OCE);
  ASSERT_TRUE(CS.has_value());
  // The object is argument 0, not argument 1.
  EXPECT_EQ(CS->ImplicitObjectArg, OCE->getArg(0));
  ASSERT_EQ(CS->Arguments.size(), 1u);
  EXPECT_EQ(CS->Arguments[0].first, OCE->getArg(1));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, FreeOperatorCallDoesNotShift) {
  Parsed P = parseCXX("struct S {}; int *operator+(S, int *); void f(S s, int "
                      "*p) { s + p; }");
  const auto *OCE = findFirst<CXXOperatorCallExpr>(P.ctx());
  ASSERT_NE(OCE, nullptr);
  ASSERT_EQ(OCE->getNumArgs(), 2u);
  auto CS = resolveCallSite(OCE);
  ASSERT_TRUE(CS.has_value());
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 2u);
  EXPECT_EQ(CS->Arguments[0].first, OCE->getArg(0));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_EQ(CS->Arguments[1].first, OCE->getArg(1));
  EXPECT_EQ(CS->Arguments[1].second, 1u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, ExplicitObjectMemberOperatorPairsFromZero) {
  Parsed P = parseCXX("struct S { int *operator()(this S, int *p); }; void f(S "
                      "s, int *p) { s(p); }");
  const auto *OCE = findFirst<CXXOperatorCallExpr>(P.ctx());
  ASSERT_NE(OCE, nullptr);
  ASSERT_EQ(OCE->getNumArgs(), 2u);
  auto CS = resolveCallSite(OCE);
  ASSERT_TRUE(CS.has_value());
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 2u);
  // The object is the explicit object parameter, so it pairs with index 0.
  EXPECT_EQ(CS->Arguments[0].first, OCE->getArg(0));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_EQ(CS->Arguments[1].first, OCE->getArg(1));
  EXPECT_EQ(CS->Arguments[1].second, 1u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, StaticMemberOperatorHasNoImplicitObject) {
  // C++23 static operator(): the object expression is still argument 0, but
  // the callee has no implicit object parameter.
  Parsed P = parseCXX("struct S { static int *operator()(int *p); }; void f(S "
                      "s, int *p) { s(p); }");
  const auto *OCE = findFirst<CXXOperatorCallExpr>(P.ctx());
  ASSERT_NE(OCE, nullptr);
  ASSERT_EQ(OCE->getNumArgs(), 2u);
  auto CS = resolveCallSite(OCE);
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  const auto *MD = dyn_cast<CXXMethodDecl>(CS->Callee);
  ASSERT_NE(MD, nullptr);
  ASSERT_TRUE(MD->isStatic());
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 1u);
  EXPECT_EQ(CS->Arguments[0].first, OCE->getArg(1));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  // The object expression is dropped from pairing but must not vanish.
  ASSERT_EQ(CS->UnmatchedArgs.size(), 1u);
  EXPECT_EQ(CS->UnmatchedArgs[0], OCE->getArg(0));
}

TEST(CallSiteResolver, ConstructorPairsArguments) {
  Parsed P = parseCXX("struct S { S(int *, int); }; void f(int *p) { S s(p, 1); }");
  const auto *CCE = findFirst<CXXConstructExpr>(P.ctx());
  ASSERT_NE(CCE, nullptr);
  ASSERT_EQ(CCE->getNumArgs(), 2u);
  auto CS = resolveCallSite(CCE);
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  EXPECT_TRUE(isa<CXXConstructorDecl>(CS->Callee));
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 2u);
  EXPECT_EQ(CS->Arguments[0].first, CCE->getArg(0));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  EXPECT_EQ(CS->Arguments[1].first, CCE->getArg(1));
  EXPECT_EQ(CS->Arguments[1].second, 1u);
  EXPECT_TRUE(CS->UnmatchedArgs.empty());
}

TEST(CallSiteResolver, VariadicTailIsUnmatched) {
  Parsed P = parseCXX("void v(int, ...); void f(int *p, int *q) { v(1, p, q); }");
  const auto *CE = findFirst<CallExpr>(P.ctx());
  ASSERT_NE(CE, nullptr);
  ASSERT_EQ(CE->getNumArgs(), 3u);
  auto CS = resolveCallSite(CE);
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  ASSERT_EQ(CS->Arguments.size(), 1u);
  EXPECT_EQ(CS->Arguments[0].first, CE->getArg(0));
  EXPECT_EQ(CS->Arguments[0].second, 0u);
  ASSERT_EQ(CS->UnmatchedArgs.size(), 2u);
  EXPECT_EQ(CS->UnmatchedArgs[0], CE->getArg(1));
  EXPECT_EQ(CS->UnmatchedArgs[1], CE->getArg(2));
}

TEST(CallSiteResolver, IndirectCallHasNoCallee) {
  Parsed P = parseCXX("void f(void (*fp)(int *), int *p) { fp(p); }");
  const auto *CE = findFirst<CallExpr>(P.ctx());
  ASSERT_NE(CE, nullptr);
  ASSERT_EQ(CE->getNumArgs(), 1u);
  auto CS = resolveCallSite(CE);
  ASSERT_TRUE(CS.has_value());
  EXPECT_EQ(CS->Callee, nullptr);
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  EXPECT_TRUE(CS->Arguments.empty());
  ASSERT_EQ(CS->UnmatchedArgs.size(), 1u);
  EXPECT_EQ(CS->UnmatchedArgs[0], CE->getArg(0));
}

TEST(CallSiteResolver, UnprototypedCCallHasOnlyUnmatchedArgs) {
  Parsed P = parseC("void g(); void f(int *p) { g(p, 2); }");
  const auto *CE = findFirst<CallExpr>(P.ctx());
  ASSERT_NE(CE, nullptr);
  ASSERT_EQ(CE->getNumArgs(), 2u);
  auto CS = resolveCallSite(CE);
  ASSERT_TRUE(CS.has_value());
  ASSERT_NE(CS->Callee, nullptr);
  EXPECT_EQ(CS->ImplicitObjectArg, nullptr);
  EXPECT_TRUE(CS->Arguments.empty());
  ASSERT_EQ(CS->UnmatchedArgs.size(), 2u);
  EXPECT_EQ(CS->UnmatchedArgs[0], CE->getArg(0));
  EXPECT_EQ(CS->UnmatchedArgs[1], CE->getArg(1));
}

TEST(CallSiteResolver, NonCallStatementIsNullopt) {
  Parsed P = parseCXX("void f(int *p) { *p = 1; }");
  const auto *BO = findFirst<BinaryOperator>(P.ctx());
  ASSERT_NE(BO, nullptr);
  EXPECT_FALSE(resolveCallSite(BO).has_value());
}

} // namespace
