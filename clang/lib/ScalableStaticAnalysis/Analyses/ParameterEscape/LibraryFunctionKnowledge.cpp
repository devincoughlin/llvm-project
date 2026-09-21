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
#include "clang/Basic/TargetInfo.h"

#include <string>

using namespace clang;
using namespace clang::ssaf;

// Bit i of NonCapturingParams <=> BuildLibCalls.cpp marks parameter i of that
// LibFunc `captures(none)`. Deallocators are listed so callers can refuse them
// even though LLVM marks their pointer captures(none).
//
// Every row is proved against LLVM by LibraryFunctionKnowledgeTest; do not edit
// one without rerunning that test.
//
// Nothing here consults a target triple, so a row may only state a fact LLVM
// grants on *every* target. `TargetLibraryInfoImpl` withholds inference for a
// function its triple does not provide, so a row available on only some targets
// would have the analysis trusting what the optimizer refuses to. Two omissions
// follow from that, and the cross-check enforces it over a fixed triple list:
//
//   - `bcmp`: available only on glibc/musl Linux, FreeBSD and Solaris
//     (`hasBcmp` in llvm/lib/Analysis/TargetLibraryInfo.cpp). `memcmp` covers
//     the same shape on every target.
//   - `stpcpy`: unavailable on both Windows environments and on PS4/PS5
//     (TargetLibraryInfo.cpp setUnavailable(LibFunc_stpcpy), in the MSVC,
//     MinGW and isPS blocks). `strcpy` carries the identical mask.
//   - `strnlen`: unavailable on PS4/PS5 (the isPS block). `strlen` covers the
//     shape. It was dropped even though it is not a clang builtin and so could
//     not be reached today, precisely because the reachability note below says
//     a row may light up later: a row that goes live must already be valid.
//
// `reallocf` is likewise unavailable on PS4/PS5 (same isPS block), but it is a
// deallocator and so refused on every target regardless; it stays, to be
// refused by name.
//
// Audit, repeatable in one command: no name below is ever made unavailable on
// any target except `reallocf`. This is a complete check of the file, not a
// sample of triples, so the multi-triple cross-check is guarding against
// *drift* -- a future LLVM adding a `setUnavailable` for one of these under a
// new OS guard -- rather than against a gap in the current facts. Re-run it
// when adding a row or rebasing on LLVM; the only expected output is
// "reallocf":
//
//   grep -oE 'setUnavailable\(LibFunc_[a-z0-9_]+\)' \
//     llvm/lib/Analysis/TargetLibraryInfo.cpp |
//     sed 's/.*LibFunc_//; s/)//' | sort -u |
//     grep -xE 'strlen|wcslen|memcpy|memmove|memcmp|strcmp|strncmp|strcpy|strncpy|strcat|strncat|puts|fputs|fwrite|printf|fprintf|atoi|strtol|free|realloc|reallocf'

//
// Reachability: `lookup` requires clang to model the callee as a builtin, and
// four of these names are not clang builtins at all (absent from Builtins.td):
// `puts`, `fputs`, `atoi` and `reallocf`. Their rows are therefore never
// matched today. They are kept, not deleted, because the cross-check keeps
// proving them against LLVM, and because they light up on their own if clang
// ever gains the builtin. `LibraryFunctionKnowledgeTest` pins the exact list,
// so a change in either direction is noticed rather than silent -- and because
// every row above is valid on every target in that test's triple list, a row
// that does light up is already sound on all of them.
static constexpr LibraryFunctionFact Facts[] = {
    {"strlen", 1, 0b1, false},    {"wcslen", 1, 0b1, false},
    {"memcpy", 3, 0b10, false},   {"memmove", 3, 0b10, false},
    {"memcmp", 3, 0b11, false},   {"strcmp", 2, 0b11, false},
    {"strncmp", 3, 0b11, false},  {"strcpy", 2, 0b10, false},
    {"strncpy", 3, 0b10, false},  {"strcat", 2, 0b10, false},
    {"strncat", 3, 0b10, false},  {"puts", 1, 0b1, false},
    {"fputs", 2, 0b11, false},    {"fwrite", 4, 0b1001, false},
    {"printf", 1, 0b1, false},    {"fprintf", 2, 0b11, false},
    {"atoi", 1, 0b1, false},      {"strtol", 3, 0b10, false},
    {"free", 1, 0b1, true},       {"realloc", 2, 0b1, true},
    {"reallocf", 2, 0b1, true},
};

bool clang::ssaf::targetHasNoCRuntime(const llvm::Triple &T) {
  // Mirrors the `disableAllFunctions()` early-outs in
  // llvm/lib/Analysis/TargetLibraryInfo.cpp: AMDGPU and DXIL disable every
  // libcall and return immediately, and NVPTX disables everything before
  // re-enabling a small handful (`malloc`, `free`, `nvvm_reflect`) that this
  // table either does not list or lists only to refuse.
  return T.isAMDGPU() || T.isDXIL() || T.isNVPTX();
}

llvm::ArrayRef<LibraryFunctionFact> clang::ssaf::libraryFunctionFacts() {
  return Facts;
}

/// True if any redeclaration of FD changes how the spelled name resolves to a
/// definition, so that the name no longer denotes the function the table
/// describes.
///
/// The property is "symbol resolution is redirected or multiplexed", not any
/// particular attribute. The attribute list below is a set of *instances* of
/// that property, and enumerating instances is precisely how multiversioning
/// was missed once already: `target_clones` lowers to an ifunc -- the same
/// construct as `__attribute__((ifunc))`, under a different spelling that the
/// `IFuncAttr` term does not match. Prefer a semantic predicate to a new
/// attribute name when extending this. `isMultiVersion()` is one such
/// predicate: it covers `target_clones`, `target_version`, multiversion
/// `target` and `cpu_dispatch` in one test, and is false for a plain
/// declaration.
///
/// The instances:
///   - `alias`/`ifunc` and an `__asm__` label send the call to another
///     definition entirely; libc shims and symbol-versioning headers do this.
///   - multiversioning (`isMultiVersion()`) resolves the call through a
///     resolver clang emits as an ifunc, to user code.
///   - `overloadable` makes the name one of several unrelated functions.
///   - `weak` means the definition may be replaced at link time by a different
///     one.
///
/// Which terms actually decide, measured rather than assumed:
///   - `__asm__`, `weak` and `isMultiVersion()` are decided here and nowhere
///     else. The declaration keeps its builtin id and is not a definition, so
///     this is the only refusal.
///   - `alias` and `ifunc` are in practice decided one layer deeper, by the
///     `isDefined()` refusal in `lookup`: on targets that support them the
///     attributed declaration *is* a definition, and on Darwin Sema rejects the
///     attribute outright so it never attaches. The terms are kept as
///     belt-and-braces for any target where an alias is not a definition; that
///     they are currently redundant is a fact about those targets, not a reason
///     to rely on either layer alone. `isDefined()` is pinned by
///     DefinedLibraryFunctionsAreNotTrusted, so the claim stays true.
///   - `overloadable` is decided both here and by the builtin-id requirement,
///     which clang zeroes for an overloadable redeclaration.
///
/// `weak_import` is deliberately NOT listed, though `weak` is. They mean
/// different things: `weak` says the definition may be replaced at link time by
/// a different one, which is exactly the loss of identity this function guards
/// against, while `weak_import` says the symbol may be *absent* at runtime and
/// resolve to null -- if it is present it is still the library's.
/// (An earlier version of this comment also claimed that listing it would cost
/// coverage on Darwin, because availability annotations attach
/// `WeakImportAttr` to libc declarations. That was measured and is false: an
/// `-ast-dump` over the macOS SDK's <string.h>, <stdio.h> and <stdlib.h> at
/// deployment targets 10.13 through 14.0 yields no `WeakImportAttr` on any name
/// in this table. The decision rests on the distinction above, not on a cost.)
static bool renamesOrOverloadsTheSymbol(const FunctionDecl *FD) {
  for (const FunctionDecl *R : FD->redecls()) {
    // Semantic predicate first: covers target_clones / target_version /
    // multiversion target / cpu_dispatch, all of which clang lowers to an
    // ifunc without ever attaching IFuncAttr.
    if (R->isMultiVersion())
      return true;
    if (R->hasAttr<AliasAttr>() || R->hasAttr<IFuncAttr>() ||
        R->hasAttr<AsmLabelAttr>() || R->hasAttr<OverloadableAttr>() ||
        R->hasAttr<WeakAttr>())
      return true;
  }
  return false;
}

/// Find the table row FD is recognized as, or null. A callee qualifies only if
/// it is bodiless in this TU (definitions are analyzed, never trusted by name),
/// builtins are enabled both globally and for this name, the spelled name is
/// not redirected or overloaded, clang itself models the declaration as the
/// corresponding builtin, and -- outside the `__builtin_` spelling -- it has C
/// language linkage.
///
/// What the gate guarantees: the callee is a declaration clang resolved to a
/// specific builtin id, with a prototype clang accepted for that builtin, not
/// redirected by `alias`/`ifunc`/`__asm__`, not `overloadable` or `weak`, and
/// not opted out of by `-fno-builtin`, `-fno-builtin-<name>` or
/// `-ffreestanding`. Requiring the builtin id is deference to clang's own
/// identity judgement rather than a rule of our own: where clang declines to
/// model a declaration as the library function -- a `static` shadow, an
/// `overloadable` redeclaration, a prototype that earns
/// `-Wincompatible-library-redeclaration` -- we decline to trust it.
///
/// What it still does not guarantee: the out-of-TU *definition* really is the C
/// library's. A declaration clang accepts as `strlen`, linked against an object
/// that defines a capturing `strlen`, is still trusted. Nothing visible in one
/// TU can rule that out; it is the same assumption the optimizer already makes.
static const LibraryFunctionFact *lookup(const FunctionDecl *FD,
                                         ASTContext &Ctx) {
  if (!FD || Ctx.getLangOpts().NoBuiltin)
    return nullptr;
  // On a target with no C runtime, LLVM grants no libcall inference at all, so
  // no row here is a fact. Without this the analysis would assume what the
  // optimizer explicitly refuses on that very target -- the unsound direction.
  // Concretely: a CUDA/HIP device pass where a device header declares
  // `extern "C" size_t strlen(const char *)` and the device runtime's `strlen`
  // captures.
  if (targetHasNoCRuntime(Ctx.getTargetInfo().getTriple()))
    return nullptr;
  // `FD->hasAttr<NoBuiltinAttr>()` is deliberately NOT checked; the caller
  // obligation that creates is documented on `parameterDoesNotEscape`.
  if (FD->isDefined() || FD->isCXXClassMember())
    return nullptr; // definitions are analyzed, never trusted by name
  if (renamesOrOverloadsTheSymbol(FD))
    return nullptr;

  // Clang must recognize this declaration as the builtin. This is what closes
  // the cases the attribute checks above cannot see: a `static` shadow, an
  // `overloadable` redeclaration, and a prototype clang rejects for the library
  // function all have a zero builtin id.
  unsigned ID = FD->getBuiltinID();
  if (!ID)
    return nullptr;

  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return nullptr;
  // The library spelling must have C language linkage. The `__builtin_`
  // spelling is the compiler's own and carries no such requirement, so it is
  // exempt -- matching on the spelled identifier rather than on the resolved
  // builtin name, which has the prefix stripped below.
  //
  // Measured: this check currently decides nothing. Sema attaches `BuiltinAttr`
  // only when the language linkage is C (clang/lib/Sema/SemaDecl.cpp, the
  // `getLanguageLinkage() == CLanguageLinkage` guard around
  // `BuiltinAttr::CreateImplicit`), and without it `getBuiltinID()` returns 0,
  // so the builtin-id requirement above has already refused every non-extern-"C"
  // spelling. A search for a declaration of any table name with a non-zero
  // builtin id and no C linkage -- across `extern "C++"`, `namespace std`, a
  // named namespace, a C++ redeclaration following an `extern "C"` one, block
  // scope, Objective-C++ and `clang_builtin_alias` -- found none. Kept as
  // belt-and-braces because it states the rule directly instead of depending on
  // a Sema implementation detail.
  if (!II->getName().starts_with("__builtin_") && !FD->isExternC())
    return nullptr;

  // `Builtins::Context::getName` returns by value, so the storage has to
  // outlive the StringRef that views it.
  std::string BuiltinName = Ctx.BuiltinInfo.getName(ID);
  llvm::StringRef Name(BuiltinName);
  Name.consume_front("__builtin_");

  // `-fno-builtin-<name>` does not set `LangOpts.NoBuiltin`; it clears the
  // builtin id of the *library* spelling only. `__builtin_<name>` keeps its id
  // under `-fno-builtin-<name>`, `-fno-builtin` and `-ffreestanding` alike, so
  // honoring the opt-out for that spelling has to be done by name here (and by
  // the `NoBuiltin` early-out above).
  if (Ctx.getLangOpts().isNoBuiltinFunc(Name))
    return nullptr;

  for (const LibraryFunctionFact &F : Facts)
    if (F.Name == Name && F.Arity == FD->getNumParams())
      return &F;
  return nullptr;
}

bool LibraryFunctionKnowledge::isDeallocationFunction(const FunctionDecl *FD,
                                                      ASTContext &Ctx) {
  const LibraryFunctionFact *F = lookup(FD, Ctx);
  return F && F->IsDeallocator;
}

bool LibraryFunctionKnowledge::parameterDoesNotEscape(const FunctionDecl *FD,
                                                      unsigned ParamIndex,
                                                      ASTContext &Ctx) {
  const LibraryFunctionFact *F = lookup(FD, Ctx);
  if (!F || F->IsDeallocator || ParamIndex >= F->Arity)
    return false;
  return (F->NonCapturingParams >> ParamIndex) & 1u;
}
