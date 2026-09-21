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

/// \returns true iff \p T (after stripping any reference) is a record type
/// annotated `[[gsl::Pointer]]` or `swift_attr("~Escapable")`, regardless of
/// its triviality.
bool isViewLikeRecordType(QualType T);

/// \returns true iff \p T is a view-like record type (isViewLikeRecordType)
/// that is also trivially copyable and trivially destructible.
///
/// Only such views are field-sensitively tracked: a non-trivial view can
/// stash the pointer in its constructor or destructor, so it is treated as an
/// opaque object rather than as a carrier of the parameter's provenance.
bool isTrackedViewType(QualType T);

/// \returns true iff a value of type \p T can carry the provenance of a
/// parameter: references, any non-function pointer (including ObjC object
/// pointers), block pointers, and tracked views.
///
/// This is the *analysis* population: every parameter of such a type gets an
/// EscapeFact, whether or not it may be annotated.
bool isPointerCarryingType(QualType T);

/// \returns true iff a parameter of type \p T is eligible for a `noescape`
/// annotation in M1: references, object pointers, and tracked views.
///
/// This is a subset of isPointerCarryingType; function pointers, block
/// pointers and ObjC object pointers are analyzed but never annotated.
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
