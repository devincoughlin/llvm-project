//===- InferNoescape.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The infer-noescape transformation inserts the noescape attribute on every
// redeclaration of each parameter the NonEscapingParameters analysis proved
// non-escaping, and reports skipped sites and rejected candidates as SARIF.
//
// See
// devincoughlin/features/noescape-inference/specs/2026-09-19-cross-tu-noescape-inference-design.md
// sections 5.4 and 5.5.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_SOURCETRANSFORMATION_TRANSFORMATIONS_INFERNOESCAPE_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_SOURCETRANSFORMATION_TRANSFORMATIONS_INFERNOESCAPE_H

#include "clang/ScalableStaticAnalysis/SourceTransformation/Transformation.h"
#include "llvm/ADT/StringRef.h"

namespace clang {
class ASTContext;
} // namespace clang

namespace clang::ssaf {

/// One note per inserted attribute, carrying the function USR and the
/// parameter index.
constexpr llvm::StringLiteral NoescapeInsertedRuleId = "noescape-inserted";

/// One warning per declaration site that could not be edited. It
/// pre-announces a possible `conflicting types` mismatch against a
/// redeclaration that another translation unit does edit; see design
/// section 5.5.
constexpr llvm::StringLiteral NoescapeSkippedRuleId = "noescape-skipped";

/// One note per rejected candidate parameter, emitted once at the
/// definition, carrying the reason, its location, and one hop of blame.
constexpr llvm::StringLiteral NoescapeRejectedRuleId = "noescape-rejected";

/// The spelling inserted when --ssaf-noescape-spelling= is not given. The
/// same in every language, so a header shared by C and C++ translation units
/// receives byte-identical insertions that the merge tool deduplicates.
constexpr llvm::StringLiteral DefaultNoescapeSpelling =
    "__attribute__((noescape))";

class InferNoescape final : public Transformation {
public:
  using Transformation::Transformation;

  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_SOURCETRANSFORMATION_TRANSFORMATIONS_INFERNOESCAPE_H
