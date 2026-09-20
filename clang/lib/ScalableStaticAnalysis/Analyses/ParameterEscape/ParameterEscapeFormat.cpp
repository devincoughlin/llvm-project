//===- ParameterEscapeFormat.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ParameterEscapeJSON.h"
#include "SSAFAnalysesCommon.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Registry.h"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace clang;
using namespace ssaf;
using Array = llvm::json::Array;
using Object = llvm::json::Object;
using Value = llvm::json::Value;

namespace {
constexpr llvm::StringLiteral ParamsKey = "params";
constexpr llvm::StringLiteral IndexKey = "index";
constexpr llvm::StringLiteral ThisKey = "this";
constexpr llvm::StringLiteral IsCandidateKey = "is_candidate";
constexpr llvm::StringLiteral CandidateParamsKey = "candidate_params";
constexpr llvm::StringLiteral FlowsToKey = "flows_to";
constexpr llvm::StringLiteral CalleeKey = "callee";
constexpr llvm::StringLiteral ParamKey = "param";
constexpr llvm::StringLiteral LocationKey = "location";
constexpr llvm::StringLiteral ReturnsSelfAtKey = "returns_self_at";
constexpr llvm::StringLiteral SinkKey = "sink";
constexpr llvm::StringLiteral ReasonKey = "reason";
constexpr llvm::StringLiteral DetailKey = "detail";
constexpr llvm::StringLiteral FileKey = "file";
constexpr llvm::StringLiteral LineKey = "line";
constexpr llvm::StringLiteral ColumnKey = "column";

/// Reads a field that is legitimately allowed to be absent, without letting a
/// corrupt value pass for an absent one.
///
/// \returns a null pointer when \p Key is absent, an error when \p Key is
/// present but does not hold an object, and the object otherwise. Treating
/// "present but malformed" as "absent" would silently drop an escape fact and
/// so invert the analysis' sound default; see the FlowsTo/sink readers below.
llvm::Expected<const Object *> getOptionalObject(const Object &O,
                                                 llvm::StringLiteral Key) {
  const Value *V = O.get(Key);
  if (!V)
    return nullptr;
  const Object *Obj = V->getAsObject();
  if (!Obj)
    return makeSawButExpectedError(*V, "an object in field %s", Key.data());
  return Obj;
}

/// Reads an optional string field, with the same absent/corrupt distinction as
/// getOptionalObject.
llvm::Expected<std::string> getOptionalString(const Object &O,
                                              llvm::StringLiteral Key) {
  const Value *V = O.get(Key);
  if (!V)
    return std::string();
  std::optional<llvm::StringRef> S = V->getAsString();
  if (!S)
    return makeSawButExpectedError(*V, "a string in field %s", Key.data());
  return S->str();
}

/// Narrows a JSON integer to a parameter index, rejecting values that would
/// wrap on conversion. \p Min admits ThisParamIndex where the sentinel is
/// meaningful and excludes it where it is not.
llvm::Expected<int64_t> checkedParamIndex(const Value &V, int64_t Raw,
                                          int64_t Min, int64_t Max,
                                          llvm::StringLiteral Key) {
  if (Raw < Min || Raw > Max)
    return makeSawButExpectedError(
        V, "a parameter index within range in field %s", Key.data());
  return Raw;
}

/// Narrows a JSON integer to a line or column number. Locations are advisory
/// and never key a container, but an unchecked cast here would still let a
/// negative or huge value reappear as a wildly different number, so it gets
/// the same treatment as the parameter indices above.
llvm::Expected<unsigned> checkedLineOrColumn(const Object &O, int64_t Raw,
                                             llvm::StringLiteral Key) {
  if (Raw < 0 || Raw > std::numeric_limits<unsigned>::max())
    return makeSawButExpectedError(
        O, "a line or column within range in field %s", Key.data());
  return static_cast<unsigned>(Raw);
}

} // namespace

Object clang::ssaf::sourceLocationRecordToJSON(const SourceLocationRecord &R) {
  return Object{{FileKey.data(), R.FilePath},
                {LineKey.data(), static_cast<int64_t>(R.Line)},
                {ColumnKey.data(), static_cast<int64_t>(R.Column)}};
}

llvm::Expected<SourceLocationRecord>
clang::ssaf::sourceLocationRecordFromJSON(const Object &O) {
  auto File = O.getString(FileKey);
  auto Line = O.getInteger(LineKey);
  auto Column = O.getInteger(ColumnKey);
  if (!File || !Line || !Column)
    return makeSawButExpectedError(
        O, "an object with string %s and integer %s and %s", FileKey.data(),
        LineKey.data(), ColumnKey.data());

  llvm::Expected<unsigned> CheckedLine = checkedLineOrColumn(O, *Line, LineKey);
  if (!CheckedLine)
    return CheckedLine.takeError();
  llvm::Expected<unsigned> CheckedColumn =
      checkedLineOrColumn(O, *Column, ColumnKey);
  if (!CheckedColumn)
    return CheckedColumn.takeError();

  return SourceLocationRecord{File->str(), *CheckedLine, *CheckedColumn};
}

Object clang::ssaf::escapeFactToJSON(const EscapeFact &F,
                                     JSONFormat::EntityIdToJSONFn IdToJSON) {
  Array Flows;
  for (const auto &[Target, Site] : F.FlowsTo)
    Flows.push_back(
        Object{{CalleeKey.data(), IdToJSON(Target.Callee)},
               {ParamKey.data(), static_cast<int64_t>(Target.ParamIndex)},
               {LocationKey.data(), sourceLocationRecordToJSON(Site)}});

  Object O{{FlowsToKey.data(), std::move(Flows)}};
  if (F.ReturnsSelfAt)
    O[ReturnsSelfAtKey.data()] = sourceLocationRecordToJSON(*F.ReturnsSelfAt);
  if (F.OtherSink) {
    const Sink &S = *F.OtherSink;
    O[SinkKey.data()] =
        Object{{ReasonKey.data(), escapeReasonName(S.Reason).str()},
               {DetailKey.data(), S.Detail},
               {LocationKey.data(), sourceLocationRecordToJSON(S.Location)}};
  }
  return O;
}

llvm::Expected<EscapeFact>
clang::ssaf::escapeFactFromJSON(const Object &O,
                                JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  EscapeFact F;

  const Array *Flows = O.getArray(FlowsToKey);
  if (!Flows)
    return makeSawButExpectedError(O, "an object with an array field %s",
                                   FlowsToKey.data());

  for (const Value &V : *Flows) {
    const Object *Target = V.getAsObject();
    const Object *Callee = Target ? Target->getObject(CalleeKey) : nullptr;
    std::optional<int64_t> Param =
        Target ? Target->getInteger(ParamKey) : std::nullopt;
    const Object *LocObj = Target ? Target->getObject(LocationKey) : nullptr;
    if (!Callee || !Param || !LocObj)
      return makeSawButExpectedError(
          V, "an object with fields %s, %s and %s", CalleeKey.data(),
          ParamKey.data(), LocationKey.data());

    // ThisParamIndex is the lowest meaningful index; anything below it names a
    // node no callee has.
    llvm::Expected<int64_t> CheckedParam = checkedParamIndex(
        V, *Param, ThisParamIndex, std::numeric_limits<int>::max(), ParamKey);
    if (!CheckedParam)
      return CheckedParam.takeError();

    llvm::Expected<EntityId> Id = IdFromJSON(*Callee);
    if (!Id)
      return Id.takeError();
    llvm::Expected<SourceLocationRecord> Loc =
        sourceLocationRecordFromJSON(*LocObj);
    if (!Loc)
      return Loc.takeError();
    if (!F.FlowsTo
             .emplace(FlowTarget{*Id, static_cast<int>(*CheckedParam)},
                      std::move(*Loc))
             .second)
      return makeSawButExpectedError(V, "a flow target not already seen");
  }

  llvm::Expected<const Object *> Ret =
      getOptionalObject(O, ReturnsSelfAtKey);
  if (!Ret)
    return Ret.takeError();
  if (*Ret) {
    llvm::Expected<SourceLocationRecord> Loc =
        sourceLocationRecordFromJSON(**Ret);
    if (!Loc)
      return Loc.takeError();
    F.ReturnsSelfAt = std::move(*Loc);
  }

  llvm::Expected<const Object *> SinkObj = getOptionalObject(O, SinkKey);
  if (!SinkObj)
    return SinkObj.takeError();
  if (const Object *S = *SinkObj) {
    auto ReasonName = S->getString(ReasonKey);
    const Object *LocObj = S->getObject(LocationKey);
    if (!ReasonName || !LocObj)
      return makeSawButExpectedError(*S, "an object with fields %s and %s",
                                     ReasonKey.data(), LocationKey.data());
    std::optional<EscapeReason> Reason = parseEscapeReason(*ReasonName);
    if (!Reason)
      return makeSawButExpectedError(*S, "a known escape reason in field %s",
                                     ReasonKey.data());
    llvm::Expected<SourceLocationRecord> Loc =
        sourceLocationRecordFromJSON(*LocObj);
    if (!Loc)
      return Loc.takeError();
    llvm::Expected<std::string> Detail = getOptionalString(*S, DetailKey);
    if (!Detail)
      return Detail.takeError();
    F.OtherSink = Sink{*Reason, std::move(*Loc), std::move(*Detail)};
  }

  return F;
}

static Object serialize(const EntitySummary &ES,
                        JSONFormat::EntityIdToJSONFn IdToJSON) {
  const auto &S = static_cast<const ParameterEscapeSummary &>(ES);

  Array Params;
  for (const auto &[Index, Fact] : S.Params) {
    Object P = escapeFactToJSON(Fact, IdToJSON);
    P[IndexKey.data()] = static_cast<int64_t>(Index);
    Params.push_back(std::move(P));
  }

  Array Candidates;
  for (unsigned I : S.CandidateParams)
    Candidates.push_back(static_cast<int64_t>(I));

  Object O{{ParamsKey.data(), std::move(Params)},
           {IsCandidateKey.data(), S.IsCandidate},
           {CandidateParamsKey.data(), std::move(Candidates)}};
  if (S.This)
    O[ThisKey.data()] = escapeFactToJSON(*S.This, IdToJSON);
  return O;
}

static llvm::Expected<std::unique_ptr<EntitySummary>>
deserialize(const Object &Data, EntityIdTable &,
            JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  const Array *Params = Data.getArray(ParamsKey);
  std::optional<bool> IsCandidate = Data.getBoolean(IsCandidateKey);
  const Array *Candidates = Data.getArray(CandidateParamsKey);
  if (!Params || !IsCandidate || !Candidates)
    return makeSawButExpectedError(Data,
                                   "an object with fields %s, %s and %s",
                                   ParamsKey.data(), IsCandidateKey.data(),
                                   CandidateParamsKey.data());

  auto S = std::make_unique<ParameterEscapeSummary>();
  S->IsCandidate = *IsCandidate;

  // Params and CandidateParams are keyed by unsigned, so a value above
  // UINT_MAX would wrap and collide with a legitimate parameter index.
  constexpr int64_t MaxIndex = std::numeric_limits<unsigned>::max();

  for (const Value &V : *Candidates) {
    std::optional<int64_t> I = V.getAsInteger();
    if (!I)
      return makeSawButExpectedError(V, "a parameter index");
    llvm::Expected<int64_t> Checked =
        checkedParamIndex(V, *I, 0, MaxIndex, CandidateParamsKey);
    if (!Checked)
      return Checked.takeError();
    // A repeat here is absorbed rather than rejected, unlike a repeated
    // params index or flow target below. Deliberate: those two discard a
    // distinct fact on collision, whereas a duplicate in a set carries no
    // information to lose. The cost is that non-canonical input holding a
    // duplicate is not read/write idempotent -- it comes back deduplicated.
    S->CandidateParams.insert(static_cast<unsigned>(*Checked));
  }

  for (const Value &V : *Params) {
    const Object *P = V.getAsObject();
    std::optional<int64_t> Index = P ? P->getInteger(IndexKey) : std::nullopt;
    if (!P || !Index)
      return makeSawButExpectedError(
          V, "an object with an integer field %s", IndexKey.data());
    llvm::Expected<int64_t> Checked =
        checkedParamIndex(V, *Index, 0, MaxIndex, IndexKey);
    if (!Checked)
      return Checked.takeError();
    llvm::Expected<EscapeFact> Fact = escapeFactFromJSON(*P, IdFromJSON);
    if (!Fact)
      return Fact.takeError();
    // Last-wins on a repeated index would silently discard one parameter's
    // escape facts in favour of another's.
    if (!S->Params.emplace(static_cast<unsigned>(*Checked), std::move(*Fact))
             .second)
      return makeSawButExpectedError(V, "a parameter index not already seen");
  }

  llvm::Expected<const Object *> This = getOptionalObject(Data, ThisKey);
  if (!This)
    return This.takeError();
  if (*This) {
    llvm::Expected<EscapeFact> Fact = escapeFactFromJSON(**This, IdFromJSON);
    if (!Fact)
      return Fact.takeError();
    S->This = std::move(*Fact);
  }

  return std::move(S);
}

namespace {
struct ParameterEscapeJSONFormatInfo final : JSONFormat::FormatInfo {
  ParameterEscapeJSONFormatInfo()
      : JSONFormat::FormatInfo(ParameterEscapeSummary::summaryName(), serialize,
                               deserialize) {}
};
} // namespace

static llvm::Registry<JSONFormat::FormatInfo>::Add<
    ParameterEscapeJSONFormatInfo>
    RegisterParameterEscapeJSONFormatInfo(
        ParameterEscapeSummary::Name,
        "JSON Format info for ParameterEscapeSummary");

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int ParameterEscapeJSONFormatAnchorSource = 0;
} // namespace clang::ssaf
