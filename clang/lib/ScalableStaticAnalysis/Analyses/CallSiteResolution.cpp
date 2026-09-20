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

/// Pairs \p Args positionally with the callee's parameters, routing every
/// argument past the parameter count into \c UnmatchedArgs.
static void pairArguments(ssaf::CallSite &CS,
                          llvm::ArrayRef<const Expr *> Args) {
  unsigned NumParams = CS.Callee->getNumParams();
  for (unsigned I = 0, E = Args.size(); I < E; ++I) {
    if (I < NumParams)
      CS.Arguments.push_back({Args[I], I});
    else
      CS.UnmatchedArgs.push_back(Args[I]);
  }
}

std::optional<ssaf::CallSite> ssaf::resolveCallSite(const Stmt *S) {
  if (!S)
    return std::nullopt;

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
    // CXXMemberCallExpr keeps the object out of getArgs(), so the pairing
    // below is already aligned; only record the object.
    CS.ImplicitObjectArg = MCE->getImplicitObjectArgument();
  } else if (isa<CXXOperatorCallExpr>(CE)) {
    // A member operator receives its object as the first argument, unless it
    // is an explicit-object member function, whose object is parameter 0.
    // The shift is unconditional here because it mirrors
    // PointerFlowExtractor's historical pairing.
    if (const auto *MD = dyn_cast<CXXMethodDecl>(CS.Callee);
        MD && !MD->isExplicitObjectMemberFunction() && !Args.empty()) {
      // A static operator() / operator[] (C++23) is written with an object
      // expression but has no implicit object parameter, so the dropped
      // argument is not an implicit object. Report it as unmatched rather
      // than mislabel it or silently discard it.
      if (MD->isInstance())
        CS.ImplicitObjectArg = Args.front();
      else
        CS.UnmatchedArgs.push_back(Args.front());
      Args = Args.drop_front();
    }
  }

  pairArguments(CS, Args);
  return CS;
}
