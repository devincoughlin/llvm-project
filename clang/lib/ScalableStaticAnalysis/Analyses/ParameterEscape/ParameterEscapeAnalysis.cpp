//===- ParameterEscapeAnalysis.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The whole-program half of noescape inference. ParameterEscapeAnalysis
// re-keys every ParameterEscapeSummary in the link unit by Node;
// NonEscapingParametersAnalysis closes the FlowsTo edges to a least fixpoint
// and decides which candidate parameters may be annotated.
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"
#include "ParameterEscapeJSON.h"
#include "SSAFAnalysesCommon.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisRegistry.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/DerivedAnalysis.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/SummaryAnalysis.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cassert>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

using namespace clang;
using namespace ssaf;
using Array = llvm::json::Array;
using Object = llvm::json::Object;
using Value = llvm::json::Value;

namespace {

//===----------------------------------------------------------------------===//
// JSON
//===----------------------------------------------------------------------===//

constexpr llvm::StringLiteral FactsKey = "facts";
constexpr llvm::StringLiteral CandidatesKey = "candidates";
constexpr llvm::StringLiteral NodeKey = "node";
constexpr llvm::StringLiteral FunctionKey = "function";
constexpr llvm::StringLiteral ParamKey = "param";
constexpr llvm::StringLiteral NonEscapingKey = "non_escaping";
constexpr llvm::StringLiteral ParamsKey = "params";
constexpr llvm::StringLiteral RejectedKey = "rejected";
constexpr llvm::StringLiteral ReasonKey = "reason";
constexpr llvm::StringLiteral LocationKey = "location";
constexpr llvm::StringLiteral DetailKey = "detail";
constexpr llvm::StringLiteral BlameKey = "blame";

Object nodeToJSON(const Node &N, JSONFormat::EntityIdToJSONFn IdToJSON) {
  return Object{{FunctionKey.data(), IdToJSON(N.Function)},
                {ParamKey.data(), static_cast<int64_t>(N.ParamIndex)}};
}

/// Reads one Node. \p MinParamIndex is the lowest parameter index this
/// particular site admits, exactly as checkedParamIndex's Min already
/// distinguishes the summary format's three sites: ThisParamIndex where the
/// implicit object parameter is a meaningful node, and 0 where it is not.
/// Passing it per site is what keeps ParameterEscapeResult::Candidates'
/// documented "never holds a this node" an unrepresentable state rather than
/// an argued one -- nodeFromJSON is shared with facts[].node, where
/// ThisParamIndex is legitimate.
///
/// The upper bound is INT_MAX rather than the int64_t the JSON parser hands
/// back, for the same reason the summary reader's is: Node::ParamIndex is an
/// int whose -1 is ThisParamIndex, so 4294967295 would arrive as the implicit
/// object parameter and 2147483648 as some other wrapped value.
llvm::Expected<Node> nodeFromJSON(const Value &V, int64_t MinParamIndex,
                                  JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  const Object *O = V.getAsObject();
  const Object *Function = O ? O->getObject(FunctionKey) : nullptr;
  std::optional<int64_t> Param = O ? O->getInteger(ParamKey) : std::nullopt;
  if (!Function || !Param)
    return makeSawButExpectedError(V, "an object with fields %s and %s",
                                   FunctionKey.data(), ParamKey.data());

  llvm::Expected<int64_t> CheckedParam = checkedParamIndex(
      V, *Param, MinParamIndex, std::numeric_limits<int>::max(), ParamKey);
  if (!CheckedParam)
    return CheckedParam.takeError();

  llvm::Expected<EntityId> Id = IdFromJSON(*Function);
  if (!Id)
    return Id.takeError();

  return Node{*Id, static_cast<int>(*CheckedParam)};
}

Object serializeParameterEscapeResult(const ParameterEscapeResult &R,
                                      JSONFormat::EntityIdToJSONFn IdToJSON) {
  Array Facts;
  for (const auto &[N, F] : R.Facts) {
    Object E = escapeFactToJSON(F, IdToJSON);
    E[NodeKey.data()] = nodeToJSON(N, IdToJSON);
    Facts.push_back(std::move(E));
  }

  Array Candidates;
  for (const Node &N : R.Candidates)
    Candidates.push_back(nodeToJSON(N, IdToJSON));

  return Object{{ParameterEscapeResultName.data(),
                 Object{{FactsKey.data(), std::move(Facts)},
                        {CandidatesKey.data(), std::move(Candidates)}}}};
}

llvm::Expected<std::unique_ptr<AnalysisResult>>
deserializeParameterEscapeResult(const Object &Obj,
                                 JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  const Object *Content = Obj.getObject(ParameterEscapeResultName);
  const Array *Facts = Content ? Content->getArray(FactsKey) : nullptr;
  const Array *Candidates =
      Content ? Content->getArray(CandidatesKey) : nullptr;
  if (!Facts || !Candidates)
    return makeSawButExpectedError(
        Obj, "an object with a key %s holding arrays %s and %s",
        ParameterEscapeResultName.data(), FactsKey.data(),
        CandidatesKey.data());

  auto R = std::make_unique<ParameterEscapeResult>();

  for (const Value &V : *Facts) {
    const Object *E = V.getAsObject();
    const Value *NodeVal = E ? E->get(NodeKey) : nullptr;
    if (!NodeVal)
      return makeSawButExpectedError(V, "an object with a field %s",
                                     NodeKey.data());
    // Facts is keyed by every summarized parameter, this nodes included.
    llvm::Expected<Node> N = nodeFromJSON(*NodeVal, ThisParamIndex, IdFromJSON);
    if (!N)
      return N.takeError();
    llvm::Expected<EscapeFact> F = escapeFactFromJSON(*E, IdFromJSON);
    if (!F)
      return F.takeError();
    // Last-wins on a repeated node would silently discard one parameter's
    // escape facts in favour of another's.
    if (!R->Facts.emplace(*N, std::move(*F)).second)
      return makeSawButExpectedError(V, "a node not already seen in %s",
                                     FactsKey.data());
  }

  for (const Value &V : *Candidates) {
    // A candidate is a parameter that can be spelled noescape, so the implicit
    // object parameter is not one; see ParameterEscapeResult::Candidates.
    llvm::Expected<Node> N = nodeFromJSON(V, /*MinParamIndex=*/0, IdFromJSON);
    if (!N)
      return N.takeError();
    // A repeat is absorbed rather than rejected: unlike the map keys above, a
    // duplicate in a set carries no information to lose. The cost is that
    // non-canonical input holding a duplicate comes back deduplicated.
    R->Candidates.insert(*N);
  }

  return std::move(R);
}

JSONFormat::AnalysisResultRegistry::Add<ParameterEscapeResult>
    RegisterParameterEscapeResultForJSON(serializeParameterEscapeResult,
                                         deserializeParameterEscapeResult);

Object
serializeNonEscapingParametersResult(const NonEscapingParametersResult &R,
                                     JSONFormat::EntityIdToJSONFn IdToJSON) {
  Array NonEscaping;
  for (const auto &[F, Params] : R.NonEscaping) {
    Array Indices;
    for (unsigned I : Params)
      Indices.push_back(static_cast<int64_t>(I));
    NonEscaping.push_back(Object{{FunctionKey.data(), IdToJSON(F)},
                                 {ParamsKey.data(), std::move(Indices)}});
  }

  Array Rejected;
  for (const auto &[N, Rej] : R.Rejected) {
    Object O{{NodeKey.data(), nodeToJSON(N, IdToJSON)},
             {ReasonKey.data(), escapeReasonName(Rej.Reason).str()},
             {DetailKey.data(), Rej.Detail},
             {LocationKey.data(), sourceLocationRecordToJSON(Rej.Location)}};
    if (Rej.Blame)
      O[BlameKey.data()] = nodeToJSON(*Rej.Blame, IdToJSON);
    Rejected.push_back(std::move(O));
  }

  return Object{{NonEscapingParametersResultName.data(),
                 Object{{NonEscapingKey.data(), std::move(NonEscaping)},
                        {RejectedKey.data(), std::move(Rejected)}}}};
}

llvm::Expected<std::unique_ptr<AnalysisResult>>
deserializeNonEscapingParametersResult(
    const Object &Obj, JSONFormat::EntityIdFromJSONFn IdFromJSON) {
  const Object *Content = Obj.getObject(NonEscapingParametersResultName);
  const Array *NonEscaping =
      Content ? Content->getArray(NonEscapingKey) : nullptr;
  const Array *Rejected = Content ? Content->getArray(RejectedKey) : nullptr;
  if (!NonEscaping || !Rejected)
    return makeSawButExpectedError(
        Obj, "an object with a key %s holding arrays %s and %s",
        NonEscapingParametersResultName.data(), NonEscapingKey.data(),
        RejectedKey.data());

  auto R = std::make_unique<NonEscapingParametersResult>();

  // NonEscaping is keyed by unsigned, so UINT_MAX would be exact here and
  // nothing wraps inside this reader. INT_MAX anyway, for two reasons: the
  // writer can never emit above it (every index reaching NonEscaping came
  // through Node::ParamIndex, an int), so a larger value is input no writer
  // produces; and every index that lands here is used to index a function's
  // parameter list, where the consumer's own static_cast<int> reproduces
  // exactly the 4294967295 -> -1 wrap that nodeFromJSON and the summary
  // reader reject. One bound across the family, and no outlier inviting a
  // "make these consistent" cleanup in the loose direction.
  constexpr int64_t MaxIndex = std::numeric_limits<int>::max();

  for (const Value &V : *NonEscaping) {
    const Object *E = V.getAsObject();
    const Object *Function = E ? E->getObject(FunctionKey) : nullptr;
    const Array *Params = E ? E->getArray(ParamsKey) : nullptr;
    if (!Function || !Params)
      return makeSawButExpectedError(V, "an object with fields %s and %s",
                                     FunctionKey.data(), ParamsKey.data());
    llvm::Expected<EntityId> Id = IdFromJSON(*Function);
    if (!Id)
      return Id.takeError();
    // One entry per function; merging a repeat would make the read
    // non-idempotent and hide which entry contributed which index.
    auto [It, Inserted] = R->NonEscaping.emplace(*Id, std::set<unsigned>());
    if (!Inserted)
      return makeSawButExpectedError(V, "a function not already seen in %s",
                                     NonEscapingKey.data());
    for (const Value &P : *Params) {
      // params holds bare scalars, so this element type check is the only
      // thing between a corrupt element and the bound check below -- and a
      // corrupt element read as 0 would mean "parameter 0 provably does not
      // escape", which is an annotation on real source. See the element-type
      // census in ParameterEscapeResultFormatTest.cpp.
      std::optional<int64_t> I = P.getAsInteger();
      if (!I)
        return makeSawButExpectedError(P, "a parameter index");
      llvm::Expected<int64_t> Checked =
          checkedParamIndex(P, *I, 0, MaxIndex, ParamsKey);
      if (!Checked)
        return Checked.takeError();
      It->second.insert(static_cast<unsigned>(*Checked));
    }
  }

  for (const Value &V : *Rejected) {
    const Object *E = V.getAsObject();
    const Value *NodeVal = E ? E->get(NodeKey) : nullptr;
    std::optional<llvm::StringRef> Reason =
        E ? E->getString(ReasonKey) : std::nullopt;
    const Object *Loc = E ? E->getObject(LocationKey) : nullptr;
    if (!NodeVal || !Reason || !Loc)
      return makeSawButExpectedError(V, "an object with fields %s, %s and %s",
                                     NodeKey.data(), ReasonKey.data(),
                                     LocationKey.data());

    // Rejected is keyed by candidates that failed Annotate, so it carries the
    // same no-this-node invariant as Candidates.
    llvm::Expected<Node> N =
        nodeFromJSON(*NodeVal, /*MinParamIndex=*/0, IdFromJSON);
    if (!N)
      return N.takeError();
    std::optional<EscapeReason> Parsed = parseEscapeReason(*Reason);
    if (!Parsed)
      return makeSawButExpectedError(V, "a known escape reason in field %s",
                                     ReasonKey.data());
    llvm::Expected<SourceLocationRecord> Location =
        sourceLocationRecordFromJSON(*Loc);
    if (!Location)
      return Location.takeError();

    NonEscapingParametersResult::Rejection Rej;
    Rej.Reason = *Parsed;
    Rej.Location = std::move(*Location);

    // A detail that is present but not a string is a corrupt record, not an
    // absent one.
    if (const Value *D = E->get(DetailKey)) {
      std::optional<llvm::StringRef> S = D->getAsString();
      if (!S)
        return makeSawButExpectedError(*D, "a string in field %s",
                                       DetailKey.data());
      Rej.Detail = S->str();
    }

    if (const Value *B = E->get(BlameKey)) {
      // A blame is a flow target, and a caller can flow into a callee's
      // implicit object parameter, so this site admits ThisParamIndex.
      llvm::Expected<Node> BlameNode =
          nodeFromJSON(*B, ThisParamIndex, IdFromJSON);
      if (!BlameNode)
        return BlameNode.takeError();
      Rej.Blame = *BlameNode;
    }

    if (!R->Rejected.emplace(*N, std::move(Rej)).second)
      return makeSawButExpectedError(V, "a node not already seen in %s",
                                     RejectedKey.data());
  }

  return std::move(R);
}

JSONFormat::AnalysisResultRegistry::Add<NonEscapingParametersResult>
    RegisterNonEscapingParametersResultForJSON(
        serializeNonEscapingParametersResult,
        deserializeNonEscapingParametersResult);

//===----------------------------------------------------------------------===//
// Summary aggregation
//===----------------------------------------------------------------------===//

class ParameterEscapeAnalysis final
    : public SummaryAnalysis<ParameterEscapeResult, ParameterEscapeSummary> {
public:
  llvm::Error add(EntityId Id, const ParameterEscapeSummary &S) override {
    // Params first, This second, deliberately. Both Index and the
    // CandidateParams loop below narrow an unsigned to Node::ParamIndex, an
    // int whose -1 is ThisParamIndex, so a parameter index of 4294967295 would
    // land on this function's this node.
    //
    // The real fix for that is the reader's INT_MAX bound (see
    // ParameterEscapeFormat.cpp), which makes the value unrepresentable
    // through any real input -- not unreachable: a unit test builds
    // ParameterEscapeSummary in memory and reaches add() without the reader.
    //
    // This ordering is a partial mitigation, not the fix, and is worth stating
    // precisely. It only bites when S.This is present: then a real this fact,
    // which may carry a sink, overwrites the fictional clean one instead of
    // being overwritten by it, and the reverse order would drop a recorded
    // escape. When S.This is absent the fictional clean fact at Node{Id, -1}
    // survives either ordering, and a caller flowing into this function's
    // implicit object parameter reads it as clean -- the unsound direction,
    // which the ordering does nothing about and only the reader's bound
    // prevents.
    for (const auto &[Index, Fact] : S.Params)
      getResult().Facts[Node{Id, static_cast<int>(Index)}] = Fact;

    if (S.This)
      getResult().Facts[Node{Id, ThisParamIndex}] = *S.This;

    // Candidacy comes from CandidateParams alone, not from CandidateParams
    // intersected with Params. A candidate parameter with no recorded fact
    // must be visible to the fixpoint so it can be rejected: silently
    // dropping it here would be safe but would make the parameter vanish
    // from the report, and intersecting is the edit that would make the
    // fixpoint's "no fact" branch unreachable and therefore untested.
    if (S.IsCandidate)
      for (unsigned Index : S.CandidateParams)
        getResult().Candidates.insert(Node{Id, static_cast<int>(Index)});

    return llvm::Error::success();
  }
};

AnalysisRegistry::Add<ParameterEscapeAnalysis> RegisterParameterEscapeAnalysis(
    "Aggregates per-parameter escape facts by node");

//===----------------------------------------------------------------------===//
// Fixpoint
//===----------------------------------------------------------------------===//

/// Closes the FlowsTo edges of ParameterEscapeResult to a least fixpoint and
/// decides which candidate parameters may be annotated noescape.
///
///   EscapesForCaller(Q) := Q not in Facts
///                        v OtherSink(Q)
///                        v exists R in FlowsTo(Q): EscapesForCaller(R)
///   Annotate(X)         := Candidate(X) ^ X in Facts ^ !OtherSink(X)
///                        ^ !ReturnsSelf(X)
///                        ^ forall Q in FlowsTo(X): !EscapesForCaller(Q)
///
/// The node universe is dom(Facts), plus every node named by a FlowsTo edge,
/// plus Candidates. The third term is not redundant: a candidate parameter the
/// extractor declared annotatable but recorded no fact for is outside the
/// first two, and finalize() queries exactly that node -- see the "no fact"
/// branch there.
///
/// The state is one boolean per node -- membership of \c EscapesForCaller --
/// over the two-point lattice false <= true, joined by union. Every node
/// starts at false except the two seeded classes below, and no node ever
/// leaves the set, so this is the least fixpoint: escape is derived only from
/// positive evidence. That is sound because the per-TU classifier is
/// exhaustive (its default case is a sink), so a node with a fact, no sink
/// and no escaping flow target has no use that can outlive the call. It is
/// also what makes a cycle with no sink come out non-escaping.
///
/// The unsound direction would be to let a node with no fact start at false.
/// A FlowsTo edge can name a callee parameter that has no summary in the link
/// unit -- its TU was never extracted, its summary was dropped as empty, or
/// the callee has no such parameter -- and "absent" is indistinguishable from
/// "analyzed and clean". Those nodes are seeded true in initialize().
///
/// A return is an escape of the callee's own parameter but never of the
/// caller's argument: the caller's classifier already treats every
/// pointer-carrying call result with an alias argument as an alias (spec
/// section 5.2). So ReturnsSelfAt rejects the node itself in finalize() but
/// does not seed EscapesForCaller.
class NonEscapingParametersAnalysis final
    : public DerivedAnalysis<NonEscapingParametersResult,
                             ParameterEscapeResult> {
  const ParameterEscapeResult *In = nullptr;

  /// The fixpoint state. Only ever grows.
  std::set<Node> EscapesForCaller;

  /// Flow target to the nodes that flow into it. Built from Facts, which is a
  /// std::map, so the vectors are in a deterministic order.
  std::map<Node, std::vector<Node>> Reverse;

  /// Nodes newly known to escape whose predecessors have not been visited.
  /// A node is pushed exactly when it first enters EscapesForCaller, so it is
  /// pushed at most once and the drain below terminates in O(V + E).
  std::vector<Node> Worklist;

  void markEscapes(const Node &N) {
    if (EscapesForCaller.insert(N).second)
      Worklist.push_back(N);
  }

  /// \returns the smallest (Node order) flow target of \p F that escapes for
  /// its caller, or nullptr if none does. FlowsTo is a std::map, so "first
  /// match" is the smallest target and the blame is deterministic.
  const std::pair<const FlowTarget, SourceLocationRecord> *
  firstEscapingTarget(const EscapeFact &F) const {
    for (const auto &Entry : F.FlowsTo)
      if (EscapesForCaller.count(
              Node{Entry.first.Callee, Entry.first.ParamIndex}))
        return &Entry;
    return nullptr;
  }

public:
  llvm::Error initialize(const ParameterEscapeResult &R) override {
    In = &R;

    for (const auto &[N, F] : R.Facts) {
      // Seed 1: a recorded sink is direct evidence that the node escapes.
      if (F.OtherSink)
        markEscapes(N);

      for (const auto &[Target, Site] : F.FlowsTo) {
        Node Q{Target.Callee, Target.ParamIndex};
        Reverse[Q].push_back(N);
        // Seed 2: unanalyzed implies escapes. This is the whole reason the
        // membership of Facts is load-bearing.
        if (!R.Facts.count(Q))
          markEscapes(Q);
      }
    }

    return llvm::Error::success();
  }

  llvm::Expected<bool> step() override {
    // The worklist is drained here rather than in initialize() because a
    // predecessor edge discovered late must still be followed: Reverse is
    // only complete once initialize() has walked every fact.
    while (!Worklist.empty()) {
      Node Q = Worklist.back();
      Worklist.pop_back();

      auto It = Reverse.find(Q);
      if (It == Reverse.end())
        continue;
      for (const Node &P : It->second)
        markEscapes(P);
    }

    // The drain above computes the whole fixpoint, so one pass is enough.
    return false;
  }

  llvm::Error finalize() override {
    for (const Node &X : In->Candidates) {
      NonEscapingParametersResult::Rejection Rej;

      auto FactIt = In->Facts.find(X);
      if (FactIt == In->Facts.end()) {
        // A candidate the extractor declared annotatable but recorded no fact
        // for. Absence is never read as "does not escape".
        Rej.Reason = EscapeReason::UnrecognizedUse;
        Rej.Detail = "no escape fact recorded for this parameter";
        getResult().Rejected[X] = std::move(Rej);
        continue;
      }
      const EscapeFact &F = FactIt->second;

      if (F.OtherSink) {
        Rej.Reason = F.OtherSink->Reason;
        Rej.Location = F.OtherSink->Location;
        Rej.Detail = F.OtherSink->Detail;
      } else if (F.returnsSelf()) {
        Rej.Reason = EscapeReason::Return;
        Rej.Location = *F.ReturnsSelfAt;
      } else if (const auto *Culprit = firstEscapingTarget(F)) {
        Node Q{Culprit->first.Callee, Culprit->first.ParamIndex};
        Rej.Reason = In->Facts.count(Q) ? EscapeReason::EscapesViaCallee
                                        : EscapeReason::UnanalyzedCallee;
        Rej.Location = Culprit->second;
        Rej.Blame = Q;
      } else {
        // ParameterEscapeSummary::CandidateParams is a set<unsigned>, so a
        // candidate is never a this node and the cast below is a real
        // parameter position.
        assert(X.ParamIndex >= 0 && "a this node cannot be a candidate");
        getResult().NonEscaping[X.Function].insert(
            static_cast<unsigned>(X.ParamIndex));
        continue;
      }

      getResult().Rejected[X] = std::move(Rej);
    }

    return llvm::Error::success();
  }
};

AnalysisRegistry::Add<NonEscapingParametersAnalysis>
    RegisterNonEscapingParametersAnalysis(
        "Whole-program escape fixpoint: parameters that provably do not "
        "escape");

} // namespace

namespace clang::ssaf {
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int ParameterEscapeAnalysisAnchorSource = 0;
} // namespace clang::ssaf
