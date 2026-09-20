//===- CallSiteResolution.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Resolves a call-like statement to its callee and pairs arguments with
//  parameter indices. Shared by the PointerFlow and ParameterEscape
//  extractors.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_CALLSITERESOLUTION_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_CALLSITERESOLUTION_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>
#include <utility>

namespace clang::ssaf {

/// A call-like statement resolved into its callee, its implicit object
/// argument, and the pairing of its arguments with callee parameter indices.
struct CallSite {
  /// The statically known callee, or null when the call is indirect.
  const FunctionDecl *Callee = nullptr;
  /// The implicit object argument of a member call or a member operator call.
  /// Only set when the callee actually has an implicit object parameter, so
  /// it stays null for a static member operator and for an explicit-object
  /// member function (whose object is ordinary parameter 0).
  const Expr *ImplicitObjectArg = nullptr;
  /// Arguments paired with the callee parameter index they initialize.
  llvm::SmallVector<std::pair<const Expr *, unsigned>, 8> Arguments;
  /// Arguments with no corresponding parameter: variadic tails, unprototyped
  /// callees, arity mismatches, the object expression of a static member
  /// operator call, and every argument of an indirect call.
  llvm::SmallVector<const Expr *, 4> UnmatchedArgs;
};

/// Returns the resolved call site for a CallExpr (including member and
/// operator calls) or a CXXConstructExpr, and std::nullopt for any other
/// statement.
std::optional<CallSite> resolveCallSite(const Stmt *S);

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_CALLSITERESOLUTION_H
