//===- ParameterEscape.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-translation-unit summary of how each parameter of a function definition
// may escape: the callee parameters it flows into, whether it is returned, and
// the first non-return sink. See
// devincoughlin/features/noescape-inference/specs/2026-09-19-cross-tu-noescape-inference-design.md.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPE_H
#define LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPE_H

#include "clang/ScalableStaticAnalysis/Analyses/SharedLexicalRepresentation/SharedLexicalRepresentation.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Model/SummaryName.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/EntitySummary.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace clang::ssaf {

/// Parameter index of the implicit object parameter in FlowTarget and Node.
///
/// This value is serialized into summaries and is fixed by the design's node
/// identity rule, so it is part of the on-disk format rather than an
/// implementation detail that may be renumbered.
constexpr int ThisParamIndex = -1;
static_assert(ThisParamIndex == -1,
              "ThisParamIndex is written to disk; changing it is a format "
              "change, not a refactor");

/// A callee parameter that a parameter of the summarized function flows into.
struct FlowTarget {
  EntityId Callee;
  int ParamIndex;

  bool operator<(const FlowTarget &Other) const {
    return std::tie(Callee, ParamIndex) <
           std::tie(Other.Callee, Other.ParamIndex);
  }
  bool operator==(const FlowTarget &Other) const {
    return std::tie(Callee, ParamIndex) ==
           std::tie(Other.Callee, Other.ParamIndex);
  }
};

/// Why a parameter is considered to escape at a given use.
///
/// The enumerators and their serialized names come from one list, so they
/// cannot drift apart. UnrecognizedUse stays last; see EscapeReasons.def.
enum class EscapeReason : uint8_t {
#define REASON(NAME) NAME,
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeReasons.def"
};

/// \returns the stable serialized name of \p R.
llvm::StringRef escapeReasonName(EscapeReason R);

/// \returns the EscapeReason whose name is \p Name, or std::nullopt if \p Name
/// names no reason.
std::optional<EscapeReason> parseEscapeReason(llvm::StringRef Name);

/// A single non-return escape, recorded at its first occurrence.
struct Sink {
  /// Defaults to the sound catch-all, so a Sink that someone forgot to fill
  /// in reports an escape rather than an out-of-range reason.
  EscapeReason Reason = EscapeReason::UnrecognizedUse;
  SourceLocationRecord Location;
  /// Free-form detail, e.g. the statement class for UnrecognizedUse.
  std::string Detail;

  bool operator==(const Sink &Other) const {
    return std::tie(Reason, Location, Detail) ==
           std::tie(Other.Reason, Other.Location, Other.Detail);
  }
};

/// Everything known about how one parameter may escape.
struct EscapeFact {
  /// Callee parameters this parameter flows into, with the first call site.
  std::map<FlowTarget, SourceLocationRecord> FlowsTo;
  /// Where an alias of this parameter is first returned, if it ever is.
  std::optional<SourceLocationRecord> ReturnsSelfAt;
  /// The first non-return sink, if any.
  std::optional<Sink> OtherSink;

  bool returnsSelf() const { return ReturnsSelfAt.has_value(); }

  bool operator==(const EscapeFact &Other) const {
    return std::tie(FlowsTo, ReturnsSelfAt, OtherSink) ==
           std::tie(Other.FlowsTo, Other.ReturnsSelfAt, Other.OtherSink);
  }
};

/// Per-entity ParameterEscape summary for one function definition.
class ParameterEscapeSummary final : public EntitySummary {
public:
  static constexpr llvm::StringLiteral Name = "ParameterEscape";

  static SummaryName summaryName() { return SummaryName(Name.str()); }

  SummaryName getSummaryName() const override { return summaryName(); }

  /// Facts for pointer-carrying parameters, keyed by parameter index.
  std::map<unsigned, EscapeFact> Params;
  /// Fact for the implicit object parameter of an instance method.
  std::optional<EscapeFact> This;
  /// Whether the definition is eligible for annotation (spec section 5.2).
  bool IsCandidate = false;
  /// Parameter indices of candidate type.
  std::set<unsigned> CandidateParams;

  bool empty() const { return Params.empty() && !This; }

  bool operator==(const ParameterEscapeSummary &Other) const {
    return std::tie(Params, This, IsCandidate, CandidateParams) ==
           std::tie(Other.Params, Other.This, Other.IsCandidate,
                    Other.CandidateParams);
  }
};

} // namespace clang::ssaf

#endif // LLVM_CLANG_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPE_H
