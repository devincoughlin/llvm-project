//===- EscapeClassifier.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Classifies every use of every parameter alias in one function definition.
// Private to the ParameterEscape extractor.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_ESCAPECLASSIFIER_H
#define LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_ESCAPECLASSIFIER_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryExtractor.h"

#include <map>
#include <optional>

namespace clang::ssaf {

/// \returns true iff a value of type \p T can carry the provenance of a
/// parameter: references, any non-function pointer (including ObjC object
/// pointers), and block pointers.
///
/// This is the *analysis* population: every parameter of such a type gets an
/// EscapeFact, whether or not it may be annotated.
///
/// No record type is pointer-carrying, and that includes the view-like ones
/// (`[[gsl::Pointer]]`, `swift_attr("~Escapable")`): M1 tracked those
/// field-sensitively and no longer does.
///
/// A *reference* to one is refused with it -- `V &`, `const V &` and `V &&` --
/// so that the deferral claims nothing anywhere: reading a field through such
/// a reference is a load that design section 1.1 makes fresh, so tracking or
/// not decides what M1 *claims* about `std::span<char> &`, and M1 claims
/// nothing while the byte-versus-pointer question (#32) is open.
///
/// A *pointer* to one stays here, and leaves candidacy instead (see below).
/// Refusing it from this population would not be subtractive: a pointer cast
/// preserves provenance, so `takes((V *)p)` would stop being classified as a
/// flow and become a cast to an unanalyzed type, and a `V *` parameter would
/// stop carrying facts that callers read.
///
/// An alias stored into a view, or passed to its constructor, still sinks
/// exactly as it does for any other record, and an argument matched to a
/// parameter this predicate refuses sinks rather than flowing -- which is what
/// keeps the refusal reject-only.
///
// M1 scope: tracked views deferred; see #34. The plan still lists them among
// M1's candidate types and the design's section 1.1 still defines a by-value
// view semantics; this declaration is where the implementation diverges.
bool isPointerCarryingType(QualType T);

/// \returns true iff a parameter of type \p T is eligible for a `noescape`
/// annotation in M1: references and object pointers.
///
/// This is a subset of isPointerCarryingType; function pointers, block
/// pointers and ObjC object pointers are analyzed but never annotated.
///
/// A view is not a candidate by value or by reference, because neither is
/// pointer-carrying, and not as a pointer to the record itself either, which
/// is refused here alone so that it keeps carrying facts while emitting no
/// annotation. See the M1 scope note there.
///
/// Those three are the whole of it, because isViewRecordType() looks one level
/// down and only at a CXXRecordDecl. A parameter that reaches a view through
/// another type constructor -- `V (&)[3]`, `V (*)[3]`, `V **`, `V *&`,
/// `const V *const *`, a record deriving from a view, or `V &` / `V *` where
/// `V` is incomplete here -- is still a candidate. That is scope rather than a
/// gap: each is answered exactly as the same shape over a non-view record is,
/// because the handle comes out of a load through a Place, which design
/// section 1.1 makes fresh. #34 is where the boundary moves.
bool isCandidateParameterType(QualType T);

/// \returns true iff the definition \p Def is eligible for annotation
/// (design section 5.2). \p Def must be a declaration with a body.
///
/// Non-candidates are still analyzed -- their facts feed callers -- they are
/// just never annotated.
bool isCandidateDefinition(const FunctionDecl *Def, ASTContext &Ctx);

/// The `Detail` written on every sink of a summary that
/// degradeToMultipleDefinitions() has rewritten.
extern const llvm::StringLiteral MultipleDefinitionsDetail;

/// Overwrite \p S with the degraded summary of \p Def: not a candidate, one
/// node per pointer-carrying parameter of \p Def (and `this`, for an instance
/// method), each reporting an escape blamed on \p Def.
///
/// Used when one entity has more than one definition: a summary can only
/// describe one of possibly-differing bodies, so neither that function nor any
/// caller that passes an argument into it may be annotated from its facts.
///
/// Every field is derived from \p Def and none is carried over from whatever
/// \p S held, so the result depends only on which definition the caller picks
/// -- pass the lexically first one and the output is reproducible even though
/// contributor iteration order is not.
void degradeToMultipleDefinitions(ParameterEscapeSummary &S,
                                  const FunctionDecl *Def, ASTContext &Ctx);

/// The escape facts of one function definition.
struct FunctionEscapeFacts {
  /// Keyed by parameter index. Contains an entry for every parameter whose
  /// type is pointer-carrying, whether or not that parameter escapes.
  std::map<unsigned, EscapeFact> Params;
  /// The fact for the implicit object parameter of an instance method.
  std::optional<EscapeFact> This;
};

/// Classify how each pointer-carrying parameter of \p Def -- and `this`, for
/// an instance method -- may escape.
FunctionEscapeFacts classifyFunctionEscapes(const FunctionDecl *Def,
                                            ASTContext &Ctx,
                                            TUSummaryExtractor &Extractor);

} // namespace clang::ssaf

#endif // LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_ESCAPECLASSIFIER_H
