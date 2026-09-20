//===- ParameterEscapeAnalysis.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Whole-program results for noescape inference:
//  - ParameterEscapeResult aggregates per-parameter escape facts by node.
//  - NonEscapingParametersResult is the fixpoint's verdict: parameters to
//    annotate, and rejected candidates with a reason and one hop of blame.
//
// See
// devincoughlin/features/noescape-inference/specs/2026-09-19-cross-tu-noescape-inference-design.md
// section 5.3.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEANALYSIS_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEANALYSIS_H

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisName.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisResult.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace clang::ssaf {

/// Registry and on-disk name of the summary-aggregation result.
constexpr llvm::StringLiteral ParameterEscapeResultName =
    "ParameterEscapeResult";

/// Registry and on-disk name of the fixpoint's result.
constexpr llvm::StringLiteral NonEscapingParametersResultName =
    "NonEscapingParametersResult";

/// One parameter of one function: the unit the escape fixpoint iterates over.
///
/// \c ParamIndex is \c ThisParamIndex for the implicit object parameter, and
/// the zero-based parameter position otherwise. Ordering is by entity first,
/// which is what makes the "smallest escaping flow target" blame rule in
/// NonEscapingParametersAnalysis deterministic.
struct Node {
  EntityId Function;
  int ParamIndex;

  bool operator<(const Node &Other) const {
    return std::tie(Function, ParamIndex) <
           std::tie(Other.Function, Other.ParamIndex);
  }
  bool operator==(const Node &Other) const {
    return std::tie(Function, ParamIndex) ==
           std::tie(Other.Function, Other.ParamIndex);
  }
  bool operator!=(const Node &Other) const { return !(*this == Other); }
};

/// Every ParameterEscapeSummary in the link unit, re-keyed by Node.
///
/// Membership of \c Facts is load-bearing and is not the same as "has no
/// escapes": a node absent from \c Facts was never analyzed (its translation
/// unit was not extracted, its summary was dropped as empty, or the callee
/// has no such parameter) and the fixpoint must treat it as escaping.
struct ParameterEscapeResult final : AnalysisResult {
  static AnalysisName analysisName() {
    return AnalysisName(ParameterEscapeResultName.str());
  }

  /// Escape facts for every summarized parameter, including \c this nodes.
  std::map<Node, EscapeFact> Facts;
  /// Parameters eligible for annotation. Never holds a \c this node: there is
  /// nowhere to spell the attribute. A candidate with no entry in \c Facts is
  /// representable and means "declared annotatable, nothing known" -- which
  /// the fixpoint rejects rather than annotates.
  std::set<Node> Candidates;
};

/// The fixpoint's verdict over the candidates.
struct NonEscapingParametersResult final : AnalysisResult {
  static AnalysisName analysisName() {
    return AnalysisName(NonEscapingParametersResultName.str());
  }

  /// Why one candidate was not annotated, with at most one hop of blame.
  struct Rejection {
    /// Defaults to the sound catch-all so a Rejection nobody filled in still
    /// reads as a rejection with a meaningful reason rather than as
    /// EscapeReason(0).
    EscapeReason Reason = EscapeReason::UnrecognizedUse;
    /// The sink, the return site, or the call site of the blamed flow.
    SourceLocationRecord Location;
    /// Free-form detail, carried over from the sink where there is one.
    std::string Detail;
    /// The flow target that escapes, for EscapesViaCallee and
    /// UnanalyzedCallee. Absent for a rejection decided inside the function.
    std::optional<Node> Blame;

    bool operator==(const Rejection &Other) const {
      return std::tie(Reason, Location, Detail, Blame) ==
             std::tie(Other.Reason, Other.Location, Other.Detail, Other.Blame);
    }
  };

  /// Candidate functions to the candidate parameter indices that provably do
  /// not escape. Absence means "do not annotate", which is the safe reading.
  std::map<EntityId, std::set<unsigned>> NonEscaping;
  /// Candidates that failed the Annotate rule.
  std::map<Node, Rejection> Rejected;
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEANALYSIS_H
