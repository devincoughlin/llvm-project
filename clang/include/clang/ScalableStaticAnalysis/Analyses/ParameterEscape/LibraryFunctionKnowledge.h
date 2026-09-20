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
// optimizer assumes -- without linking LLVM IR into the frontend.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_LIBRARYFUNCTIONKNOWLEDGE_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_LIBRARYFUNCTIONKNOWLEDGE_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"

namespace clang::ssaf {

/// True for targets LLVM models as having no C runtime library at all. On these
/// `TargetLibraryInfoImpl` calls `disableAllFunctions()`, so LLVM's libcall
/// inference grants *nothing* -- not even `strlen` or `memcpy` -- and a table
/// row is not a fact there.
///
/// This is deliberately a predicate rather than a list of triples, and it is
/// shared by `LibraryFunctionKnowledge` and by the unit test's LLVM oracle, so
/// the two cannot drift: the oracle exempts exactly the targets the analysis
/// refuses. Keep it in step with the `disableAllFunctions()` early-outs in
/// llvm/lib/Analysis/TargetLibraryInfo.cpp.
bool targetHasNoCRuntime(const llvm::Triple &T);

/// One row of the static C library capture table.
struct LibraryFunctionFact {
  /// The library function's name, without any `__builtin_` prefix.
  llvm::StringLiteral Name;
  /// Number of fixed parameters; the variadic tail is excluded.
  unsigned Arity;
  /// Bit i is set iff LLVM's libcall inference gives parameter i
  /// `captures(none)` on every target that *provides* this function. The
  /// qualification matters for deallocator rows: `reallocf` carries `0b1` here,
  /// but LLVM infers nothing for it on PS4/PS5, where it is unavailable. That
  /// is harmless because for a deallocator the mask is documentation only --
  /// `parameterDoesNotEscape` short-circuits on `IsDeallocator` before reading
  /// it. For every non-deallocator row the bit holds on every target the unit
  /// test's triple list covers.
  unsigned NonCapturingParams;
  /// True for deallocation and reallocation functions, which are listed only so
  /// that they can be refused: `noescape` also forbids deallocating through the
  /// parameter, so LLVM's `captures(none)` is not enough to trust them.
  bool IsDeallocator;
};

/// The whole table, exposed so the unit test can cross-check every row against
/// LLVM's `inferNonMandatoryLibFuncAttrs`.
llvm::ArrayRef<LibraryFunctionFact> libraryFunctionFacts();

class LibraryFunctionKnowledge {
public:
  /// True iff FD is a recognized bodiless C library function (or the matching
  /// builtin), builtins are not disabled, FD is not a deallocator, and the
  /// table marks ParamIndex non-capturing.
  ///
  /// Both results are one-directional. `true` is a positive guarantee: LLVM's
  /// own libcall inference gives that parameter `captures(none)` on every
  /// target. `false` means only "not recognized here", never "this parameter
  /// escapes" -- so `false` is the safe answer and callers must already default
  /// to escape. `true` additionally presumes the target has a C runtime;
  /// targets where LLVM disables all libcalls (AMDGPU, NVPTX, DXIL) always
  /// answer `false`.
  ///
  /// Caller obligation: this does NOT honor `__attribute__((no_builtin))`. That
  /// attribute's subject is the function *containing* the calls, not the
  /// callee, so it is invisible from this signature; a caller holding the
  /// enclosing function must apply it itself. (`-fno-builtin` and
  /// `-fno-builtin-<name>` ARE honored -- they are language options, not
  /// caller-side attributes.) This is a contract note rather than a defect:
  /// LLVM places `captures(none)` on the callee *declaration*, so wherever the
  /// attribute is not applied the optimizer is making the same assumption this
  /// function does.
  static bool parameterDoesNotEscape(const FunctionDecl *FD,
                                     unsigned ParamIndex, ASTContext &Ctx);

  /// True iff FD is a recognized C library deallocation or reallocation
  /// function. Passing an alias to one is an escape, never a benign use.
  ///
  /// `false` must NOT be read as "safe to treat as benign". It only means this
  /// table did not recognize FD as a deallocator -- which it will not, for
  /// instance, under `-fno-builtin` or `-fno-builtin-free`, where `free` itself
  /// answers `false`. That is sound only because an unrecognized callee is a
  /// sink by default; this predicate exists to *add* refusals, never to license
  /// a use.
  static bool isDeallocationFunction(const FunctionDecl *FD, ASTContext &Ctx);
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_LIBRARYFUNCTIONKNOWLEDGE_H
