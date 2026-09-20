//===- ParameterEscapeFormatTest.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests for the ParameterEscape summary's JSON format.
//
// THE INVARIANT THIS FILE EXISTS TO MAINTAIN
//
//   Every leaf of the wire format has one written-byte assertion against a
//   literal, at every path where that leaf can appear, and no two paths use
//   the same literal.
//
// Adding a field to the format means adding its assertion in the same change.
//
// Why. Two kinds of test here cannot see a whole class of defect. A round
// trip (build, write, read, compare objects) and the golden fixture in
// clang/test/Analysis/Scalable/ssaf-format/TUSummary/round-trip.test (read,
// write, diff) both check only that the writer and the reader agree with each
// other. Any transform applied symmetrically to both is the identity to them:
//
//   - transposing "line" and "column", at every path or just one
//   - renaming a reason
//   - renumbering ThisParamIndex
//   - inverting the "is_candidate" boolean
//   - swapping the contents of ANY two same-typed sibling fields
//
// That last one is why the rule is per-path and why the literals must differ:
// it applies to any two fields of the same type regardless of how many values
// either takes, so "this field is multi-valued" is not a reason to skip it.
// Each of the above was measured green against the full suite before the
// corresponding literal was added.
//
// The literals below are stated outside the writer/reader pair, which is the
// only thing that makes them asymmetric. Replacing one with the constant that
// produced it silently removes the check.
//
// THE LEAF SET
//
// Derived by walking serialize() -> escapeFactToJSON() ->
// sourceLocationRecordToJSON() in ParameterEscapeFormat.cpp. Re-derive it from
// those three functions when the format changes; do not read it off a fixture,
// which is only a sample. "$" is the entity_summary object.
//
//   29 scalar leaves, by path:
//
//     $.is_candidate                             1
//     $.candidate_params[]                       1
//     $.params[].index                           1
//     $.params[].flows_to[].callee.@             1
//     $.params[].flows_to[].param                1
//     $.params[].flows_to[].location.{file,line,column}   3
//     $.params[].returns_self_at.{file,line,column}       3
//     $.params[].sink.reason                     1
//     $.params[].sink.detail                     1
//     $.params[].sink.location.{file,line,column}         3
//     $.this.flows_to[].callee.@                 1
//     $.this.flows_to[].param                    1
//     $.this.flows_to[].location.{file,line,column}       3
//     $.this.returns_self_at.{file,line,column}           3
//     $.this.sink.reason                         1
//     $.this.sink.detail                         1
//     $.this.sink.location.{file,line,column}             3
//
// The "this" block repeats the EscapeFact leaves at a second path on purpose:
// they are 13 distinct leaves, not the same 13 seen twice, and a defect
// confined to either path is invisible from the other. RoundTripsAllFields
// asserts all 29.
//
// THE SHAPE SET
//
// Shapes are not scalar leaves and need their own derivation, by the same
// rule: read them off the writer, never off the tests that happen to exist.
//
//   Recipe: in ParameterEscapeFormat.cpp, take every `Array` the writer
//   constructs and every key it emits under an `if`. Each gives one entry.
//   Then multiply by the paths its enclosing function is called from --
//   escapeFactToJSON() runs for both $.params[] and $.this, so each of its
//   three shapes is two entries, at two distinct paths. Both sides of every
//   entry need an assertion.
//
//   3 arrays + 3 guarded keys, expanded over their call paths = 9 entries,
//   18 sides:
//
//     array  $.params                     empty / non-empty
//     array  $.candidate_params           empty / non-empty
//     array  $.params[].flows_to          empty / non-empty
//     array  $.this.flows_to              empty / non-empty
//     key    $.this                       absent / present
//     key    $.params[].returns_self_at   absent / present
//     key    $.params[].sink              absent / present
//     key    $.this.returns_self_at       absent / present
//     key    $.this.sink                  absent / present
//
// The non-empty/present side is in RoundTripsAllFields; the empty/absent side
// is in RoundTripsAbsentShapes, which needs two summaries to hold both "this"
// absent and "this" present with its own fields absent.
//
// Deriving this list from the tests instead of from the writer is how the
// empty side of $.params went unasserted for two rounds while the data that
// produced it was already there.
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "JSONFormatTest.h"

#include "clang/Frontend/SSAFOptions.h"
#include "clang/ScalableStaticAnalysis/Core/Model/BuildNamespace.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityLinkage.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummary.h"
#include "clang/ScalableStaticAnalysis/Core/TUSummary/TUSummaryBuilder.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace clang::ssaf {
namespace {

class ParameterEscapeFormatTest : public JSONFormatTest {
protected:
  // TUSummary entity names are serialized without their build namespace (see
  // JSONFormat::tuEntityNameToJSON), so names used for lookups after a round
  // trip must carry an empty namespace to compare equal.
  static EntityName nameOf(llvm::StringRef USR) {
    return EntityName(USR, /*Suffix=*/"", /*Namespace=*/{});
  }

  // Wraps one ParameterEscape entity_summary body in a minimal TUSummary
  // envelope, so a test can hand the reader exactly the bytes it wants.
  static std::string envelopeAround(llvm::StringRef SummaryBody) {
    return ("{\"data\":[{\"summary_data\":[{\"entity_id\":0,"
            "\"entity_summary\":" +
            SummaryBody +
            "}],\"summary_name\":\"ParameterEscape\"}],"
            "\"id_table\":[{\"id\":0,\"name\":{\"suffix\":\"\",\"usr\":"
            "\"c:@F@f\"}},{\"id\":1,\"name\":{\"suffix\":\"\",\"usr\":"
            "\"c:@F@g\"}}],"
            "\"linkage_table\":[{\"id\":0,\"linkage\":{\"type\":\"External\"}},"
            "{\"id\":1,\"linkage\":{\"type\":\"External\"}}],"
            "\"target_triple\":\"arm64-apple-macosx\","
            "\"tu_namespace\":{\"kind\":\"CompilationUnit\",\"name\":"
            "\"test.cpp\"},\"type\":\"TUSummary\"}")
        .str();
  }

  // Navigates written bytes down to the first entity's entity_summary object,
  // so a test can assert on the serialized form itself. Returns null rather
  // than asserting, so callers report the failure at their own line.
  static const llvm::json::Object *
  summaryObjectOf(const llvm::json::Value &Root, size_t Pos = 0) {
    const llvm::json::Object *O = Root.getAsObject();
    const llvm::json::Array *Data = O ? O->getArray("data") : nullptr;
    if (!Data || Data->empty())
      return nullptr;
    const llvm::json::Object *Entry = (*Data)[0].getAsObject();
    const llvm::json::Array *SummaryData =
        Entry ? Entry->getArray("summary_data") : nullptr;
    if (!SummaryData || Pos >= SummaryData->size())
      return nullptr;
    const llvm::json::Object *Nth = (*SummaryData)[Pos].getAsObject();
    return Nth ? Nth->getObject("entity_summary") : nullptr;
  }

  // The "@" below is JSONFormat's entity-id key, not this format's. Spelling
  // it as a literal here is not an oversight: it puts the key outside the
  // writer/reader pair, so a symmetric rename of JSONEntityIdKey fails these
  // tests instead of silently changing the format. Do not "clarify" it into
  // JSONEntityIdKey -- that deletes the check, for the same reason the
  // literal -1 must not become ThisParamIndex.
  static std::optional<int64_t> calleeIdOf(const llvm::json::Value &Target) {
    const llvm::json::Object *T = Target.getAsObject();
    const llvm::json::Object *Callee = T ? T->getObject("callee") : nullptr;
    return Callee ? Callee->getInteger("@") : std::nullopt;
  }

  static std::optional<int64_t> paramOf(const llvm::json::Value &Target) {
    const llvm::json::Object *T = Target.getAsObject();
    return T ? T->getInteger("param") : std::nullopt;
  }

  static const llvm::json::Object *locationOf(const llvm::json::Value &Target) {
    const llvm::json::Object *T = Target.getAsObject();
    return T ? T->getObject("location") : nullptr;
  }

  // The params array element at position \p Pos, so a test can pin the array's
  // order rather than only looking a parameter up by its index field.
  static const llvm::json::Object *paramAt(const llvm::json::Value &Root,
                                           size_t Pos) {
    const llvm::json::Object *Summary = summaryObjectOf(Root);
    const llvm::json::Array *Params =
        Summary ? Summary->getArray("params") : nullptr;
    if (!Params || Pos >= Params->size())
      return nullptr;
    return (*Params)[Pos].getAsObject();
  }

  // Asserts a serialized SourceLocationRecord in full. Every location in a
  // test must use values distinct from every other location's, or a swap
  // between two of them stays invisible even with all three fields asserted.
  static ::testing::AssertionResult locationIs(const llvm::json::Object *Loc,
                                               llvm::StringRef File,
                                               int64_t Line, int64_t Column) {
    if (!Loc)
      return ::testing::AssertionFailure() << "no location object";
    if (Loc->getString("file") != std::optional<llvm::StringRef>(File))
      return ::testing::AssertionFailure()
             << "file: expected '" << File.str() << "'";
    if (Loc->getInteger("line") != std::optional<int64_t>(Line))
      return ::testing::AssertionFailure() << "line: expected " << Line;
    if (Loc->getInteger("column") != std::optional<int64_t>(Column))
      return ::testing::AssertionFailure() << "column: expected " << Column;
    return ::testing::AssertionSuccess();
  }

  // Reads a TUSummary whose sole ParameterEscape body is \p SummaryBody.
  // \p CaseName is used to name the temporary file; the ".json" extension is
  // mandatory -- the reader rejects an extensionless path outright, which
  // would make a "does this input fail?" test pass for the wrong reason.
  llvm::Error readSummaryBody(llvm::StringRef SummaryBody,
                              llvm::StringRef CaseName) const {
    llvm::Expected<PathString> Path =
        writeJSON(envelopeAround(SummaryBody), CaseName.str() + ".json");
    if (!Path)
      return Path.takeError();
    llvm::Expected<TUSummary> Read = JSONFormat().readTUSummary(*Path);
    if (!Read)
      return Read.takeError();
    return llvm::Error::success();
  }

  // As readSummaryBody, but hands back what the reader built, for the cases
  // that need to assert the parsed value and not only that parsing succeeded.
  llvm::Expected<ParameterEscapeSummary>
  parseSummaryBody(llvm::StringRef SummaryBody,
                   llvm::StringRef CaseName) const {
    llvm::Expected<PathString> Path =
        writeJSON(envelopeAround(SummaryBody), CaseName.str() + ".json");
    if (!Path)
      return Path.takeError();
    llvm::Expected<TUSummary> Read = JSONFormat().readTUSummary(*Path);
    if (!Read)
      return Read.takeError();
    EntityId Id = getIdTable(*Read).getId(nameOf("c:@F@f"));
    auto &Data = getData(*Read);
    auto SummaryIt = Data.find(ParameterEscapeSummary::summaryName());
    if (SummaryIt == Data.end())
      return llvm::createStringError(std::errc::invalid_argument,
                                     "no ParameterEscape summaries");
    auto EntityIt = SummaryIt->second.find(Id);
    if (EntityIt == SummaryIt->second.end())
      return llvm::createStringError(std::errc::invalid_argument,
                                     "no summary for the test entity");
    return static_cast<const ParameterEscapeSummary &>(*EntityIt->second);
  }
};

TEST_F(ParameterEscapeFormatTest, RoundTripsAllFields) {
  SSAFOptions Opts;
  TUSummary TUSum(llvm::Triple("arm64-apple-macosx"),
                  BuildNamespace(BuildNamespaceKind::CompilationUnit, "cu"));
  TUSummaryBuilder Builder(TUSum, Opts);
  EntityId F =
      Builder.addEntity(nameOf("c:@F@f#*I#*I#"), EntityLinkageType::External);
  EntityId G =
      Builder.addEntity(nameOf("c:@F@g#*I#"), EntityLinkageType::External);
  // A second callee, so the serialized flows_to order exercises the major key
  // of FlowTarget::operator< and not just the parameter index.
  EntityId H =
      Builder.addEntity(nameOf("c:@F@h#*I#"), EntityLinkageType::External);

  // Every scalar below is distinct from every other scalar of its type in this
  // test. That is deliberate and load-bearing: asserting a leaf pins nothing
  // against a swap with a same-typed sibling if both hold the same literal.
  // Nine locations, twenty-seven distinct values, no file path repeated. Keep
  // this census accurate: it is what a future reader checks to decide whether
  // a location they are adding needs fresh literals.
  ParameterEscapeSummary S;
  S.IsCandidate = true;
  S.CandidateParams = {0, 1, 3};
  EscapeFact P0;
  P0.FlowsTo[FlowTarget{G, 0}] = SourceLocationRecord{"/src/flow-g0.cpp", 2, 3};
  P0.FlowsTo[FlowTarget{G, ThisParamIndex}] =
      SourceLocationRecord{"/src/flow-gthis.cpp", 11, 13};
  P0.FlowsTo[FlowTarget{H, ThisParamIndex}] =
      SourceLocationRecord{"/src/flow-hthis.cpp", 5, 7};
  S.Params[0] = P0;
  EscapeFact P1;
  P1.ReturnsSelfAt = SourceLocationRecord{"/src/ret.cpp", 17, 19};
  P1.OtherSink = Sink{EscapeReason::StoreToGlobal,
                      SourceLocationRecord{"/src/sink.cpp", 23, 29}, ""};
  S.Params[1] = P1;
  // The implicit object parameter is a first-class node: the classifier
  // records FlowsTo for the object argument of a member call, and
  // `return *this;` is routine. So the "this" fact must carry every shape a
  // parameter fact can, or the leaves under it are pinned at no path at all
  // and a defect confined to this path goes unseen. Two flow targets, so the
  // ordering here is pinned as well as the values.
  EscapeFact ThisFact;
  ThisFact.FlowsTo[FlowTarget{G, 8}] =
      SourceLocationRecord{"/src/this-flow-g.cpp", 41, 43};
  ThisFact.FlowsTo[FlowTarget{H, ThisParamIndex}] =
      SourceLocationRecord{"/src/this-flow-h.cpp", 47, 53};
  ThisFact.ReturnsSelfAt = SourceLocationRecord{"/src/this-ret.cpp", 59, 61};
  ThisFact.OtherSink = Sink{EscapeReason::UnrecognizedUse,
                            SourceLocationRecord{"/src/this-sink.cpp", 31, 37},
                            "AtomicExpr"};
  S.This = ThisFact;
  EXPECT_FALSE(S.empty());
  auto Owned = std::make_unique<ParameterEscapeSummary>(S);
  ASSERT_TRUE(Builder.addSummary(F, std::move(Owned)).second);

  PathString Path = makePath("parameter-escape-summary.json");
  JSONFormat Format;
  ASSERT_THAT_ERROR(Format.writeTUSummary(TUSum, Path), llvm::Succeeded());

  // ==========================================================================
  // Written-byte assertions.
  //
  // INVARIANT, which every future edit must maintain: *every leaf of the wire
  // format gets one written-byte assertion against a literal, at every path
  // where it can appear.* Adding a field to the format means adding its
  // assertion here in the same change.
  //
  // Why a literal, and why per-path. Reading back and comparing objects, and
  // the golden lit fixture's read/write/diff, both test only that the writer
  // and reader agree. Any transform applied symmetrically to both is the
  // identity to them: transposing line and column, renaming a reason,
  // renumbering ThisParamIndex, inverting a boolean, or -- for ANY two
  // same-typed sibling fields, however many values each takes -- swapping
  // their contents. Only a literal stated here, outside the writer/reader
  // pair, is asymmetric enough to catch those.
  //
  // This block asserts all 29 scalar leaves enumerated in the file header,
  // plus the present shapes. RoundTripsAbsentShapes carries the absent and
  // empty shapes.
  // ==========================================================================
  llvm::Expected<llvm::json::Value> Raw =
      readJSONFromFile("parameter-escape-summary.json");
  ASSERT_THAT_EXPECTED(Raw, llvm::Succeeded());

  const llvm::json::Object *Summary = summaryObjectOf(*Raw);
  ASSERT_NE(Summary, nullptr);
  EXPECT_EQ(Summary->getBoolean("is_candidate"), std::optional<bool>(true));

  const llvm::json::Array *Candidates = Summary->getArray("candidate_params");
  ASSERT_NE(Candidates, nullptr);
  ASSERT_EQ(Candidates->size(), 3u);
  EXPECT_EQ((*Candidates)[0].getAsInteger(), std::optional<int64_t>(0));
  EXPECT_EQ((*Candidates)[1].getAsInteger(), std::optional<int64_t>(1));
  // 3 is not a key of Params, so this also pins that candidate_params is
  // serialized from CandidateParams and not derived from the params array.
  EXPECT_EQ((*Candidates)[2].getAsInteger(), std::optional<int64_t>(3));

  const llvm::json::Array *ParamsArray = Summary->getArray("params");
  ASSERT_NE(ParamsArray, nullptr);
  ASSERT_EQ(ParamsArray->size(), 2u);

  // --- params[0]: flows only, no returns_self_at, no sink -------------------
  const llvm::json::Object *Param0 = paramAt(*Raw, 0);
  ASSERT_NE(Param0, nullptr);
  EXPECT_EQ(Param0->getInteger("index"), std::optional<int64_t>(0));

  const llvm::json::Array *Flows = Param0->getArray("flows_to");
  ASSERT_NE(Flows, nullptr);
  ASSERT_EQ(Flows->size(), 3u);

  // Ordered callee-major: both G targets precede the H target, even though
  // H's parameter index (-1) sorts below G's 0. The third callee is what makes
  // this sensitive to the major key; without it the ordering goes unpinned.
  //
  // The literal -1 here is also what pins ThisParamIndex's on-disk spelling.
  // Do not "clarify" it into `ThisParamIndex`: that compares the written byte
  // against the constant that produced it and cannot fail, leaving only the
  // header's static_assert to catch a renumbering.
  EXPECT_EQ(calleeIdOf((*Flows)[0]), 1);
  EXPECT_EQ(paramOf((*Flows)[0]), -1);
  EXPECT_TRUE(locationIs(locationOf((*Flows)[0]), "/src/flow-gthis.cpp", 11,
                         13));
  EXPECT_EQ(calleeIdOf((*Flows)[1]), 1);
  EXPECT_EQ(paramOf((*Flows)[1]), 0);
  EXPECT_TRUE(locationIs(locationOf((*Flows)[1]), "/src/flow-g0.cpp", 2, 3));
  EXPECT_EQ(calleeIdOf((*Flows)[2]), 2);
  EXPECT_EQ(paramOf((*Flows)[2]), -1);
  EXPECT_TRUE(locationIs(locationOf((*Flows)[2]), "/src/flow-hthis.cpp", 5, 7));

  // Absent shapes: params[0] has neither, so the keys must not be emitted.
  EXPECT_EQ(Param0->get("returns_self_at"), nullptr);
  EXPECT_EQ(Param0->get("sink"), nullptr);

  // --- params[1]: no flows, both returns_self_at and sink -------------------
  const llvm::json::Object *Param1 = paramAt(*Raw, 1);
  ASSERT_NE(Param1, nullptr);
  EXPECT_EQ(Param1->getInteger("index"), std::optional<int64_t>(1));
  const llvm::json::Array *Flows1 = Param1->getArray("flows_to");
  ASSERT_NE(Flows1, nullptr);
  EXPECT_TRUE(Flows1->empty());

  // returns_self_at and sink.location are two SourceLocationRecords in
  // adjacent statements of the writer -- the classic copy-paste swap. Their
  // literals differ in all three fields, so a swap cannot survive.
  EXPECT_TRUE(
      locationIs(Param1->getObject("returns_self_at"), "/src/ret.cpp", 17, 19));

  const llvm::json::Object *Sink1 = Param1->getObject("sink");
  ASSERT_NE(Sink1, nullptr);
  EXPECT_EQ(Sink1->getString("reason"),
            std::optional<llvm::StringRef>("StoreToGlobal"));
  // detail is a string sibling of location.file; pinning both, with different
  // values, is what stops the two being swapped.
  EXPECT_EQ(Sink1->getString("detail"), std::optional<llvm::StringRef>(""));
  EXPECT_TRUE(locationIs(Sink1->getObject("location"), "/src/sink.cpp", 23,
                         29));

  // --- this: flows, returns_self_at and sink all present --------------------
  // Every leaf under "this" is a distinct path from its params[] counterpart.
  // A defect confined to this subtree -- say a line/column transposition that
  // only the implicit object parameter reaches -- is invisible to every
  // assertion above, so all of them are restated here against fresh literals.
  const llvm::json::Object *This = Summary->getObject("this");
  ASSERT_NE(This, nullptr);
  const llvm::json::Array *ThisFlows = This->getArray("flows_to");
  ASSERT_NE(ThisFlows, nullptr);
  ASSERT_EQ(ThisFlows->size(), 2u);
  EXPECT_EQ(calleeIdOf((*ThisFlows)[0]), 1);
  EXPECT_EQ(paramOf((*ThisFlows)[0]), 8);
  EXPECT_TRUE(locationIs(locationOf((*ThisFlows)[0]), "/src/this-flow-g.cpp",
                         41, 43));
  EXPECT_EQ(calleeIdOf((*ThisFlows)[1]), 2);
  EXPECT_EQ(paramOf((*ThisFlows)[1]), -1);
  EXPECT_TRUE(locationIs(locationOf((*ThisFlows)[1]), "/src/this-flow-h.cpp",
                         47, 53));

  EXPECT_TRUE(locationIs(This->getObject("returns_self_at"),
                         "/src/this-ret.cpp", 59, 61));

  const llvm::json::Object *ThisSink = This->getObject("sink");
  ASSERT_NE(ThisSink, nullptr);
  // UnrecognizedUse is deliberately the default value of Sink::Reason: it is
  // the only assertion that catches a writer which omits "reason" when it
  // holds the default. Do not normalize it to some other reason.
  EXPECT_EQ(ThisSink->getString("reason"),
            std::optional<llvm::StringRef>("UnrecognizedUse"));
  EXPECT_EQ(ThisSink->getString("detail"),
            std::optional<llvm::StringRef>("AtomicExpr"));
  EXPECT_TRUE(locationIs(ThisSink->getObject("location"),
                         "/src/this-sink.cpp", 31, 37));

  llvm::Expected<TUSummary> Read = Format.readTUSummary(Path);
  ASSERT_THAT_EXPECTED(Read, llvm::Succeeded());

  ASSERT_TRUE(getIdTable(*Read).contains(nameOf("c:@F@f#*I#*I#")));
  ASSERT_TRUE(getIdTable(*Read).contains(nameOf("c:@F@g#*I#")));
  ASSERT_TRUE(getIdTable(*Read).contains(nameOf("c:@F@h#*I#")));
  EntityId ReadF = getIdTable(*Read).getId(nameOf("c:@F@f#*I#*I#"));
  EntityId ReadG = getIdTable(*Read).getId(nameOf("c:@F@g#*I#"));
  EntityId ReadH = getIdTable(*Read).getId(nameOf("c:@F@h#*I#"));

  auto &Data = getData(*Read);
  auto SummaryIt = Data.find(ParameterEscapeSummary::summaryName());
  ASSERT_NE(SummaryIt, Data.end());
  auto EntityIt = SummaryIt->second.find(ReadF);
  ASSERT_NE(EntityIt, SummaryIt->second.end());
  ASSERT_EQ(EntityIt->second->getSummaryName(),
            ParameterEscapeSummary::summaryName());
  const auto &RS =
      static_cast<const ParameterEscapeSummary &>(*EntityIt->second);

  // EntityIds are not stable across a round trip, so restate the flow targets
  // in terms of the ids the reader handed back.
  ParameterEscapeSummary Expected = S;
  Expected.Params[0].FlowsTo.clear();
  Expected.Params[0].FlowsTo[FlowTarget{ReadG, 0}] =
      SourceLocationRecord{"/src/flow-g0.cpp", 2, 3};
  Expected.Params[0].FlowsTo[FlowTarget{ReadG, ThisParamIndex}] =
      SourceLocationRecord{"/src/flow-gthis.cpp", 11, 13};
  Expected.Params[0].FlowsTo[FlowTarget{ReadH, ThisParamIndex}] =
      SourceLocationRecord{"/src/flow-hthis.cpp", 5, 7};
  ASSERT_TRUE(Expected.This.has_value());
  Expected.This->FlowsTo.clear();
  Expected.This->FlowsTo[FlowTarget{ReadG, 8}] =
      SourceLocationRecord{"/src/this-flow-g.cpp", 41, 43};
  Expected.This->FlowsTo[FlowTarget{ReadH, ThisParamIndex}] =
      SourceLocationRecord{"/src/this-flow-h.cpp", 47, 53};
  EXPECT_TRUE(RS == Expected);

  // Sanity: the round trip really did carry the individual fields, so an
  // all-defaults summary would not pass the comparison above.
  EXPECT_TRUE(RS.IsCandidate);
  EXPECT_EQ(RS.CandidateParams, (std::set<unsigned>{0, 1, 3}));
  ASSERT_EQ(RS.Params.count(1), 1u);
  EXPECT_TRUE(RS.Params.at(1).returnsSelf());
  ASSERT_TRUE(RS.This.has_value());
  ASSERT_TRUE(RS.This->OtherSink.has_value());
  EXPECT_EQ(RS.This->OtherSink->Detail, "AtomicExpr");
  EXPECT_FALSE(RS.Params.at(0).returnsSelf());
}

// The all-fields-populated case above cannot see a reader that hard-wires a
// present value, so pin the shapes where each optional field is absent.
TEST_F(ParameterEscapeFormatTest, RoundTripsAbsentShapes) {
  SSAFOptions Opts;
  TUSummary TUSum(llvm::Triple("arm64-apple-macosx"),
                  BuildNamespace(BuildNamespaceKind::CompilationUnit, "cu"));
  TUSummaryBuilder Builder(TUSum, Opts);
  EntityId F = Builder.addEntity(nameOf("c:@F@f#*I#"),
                                 EntityLinkageType::External);
  EntityId F2 = Builder.addEntity(nameOf("c:@F@f2#*I#"),
                                  EntityLinkageType::External);

  ParameterEscapeSummary S;
  S.IsCandidate = false;
  // A parameter with no flows, no return and no sink: the shape a
  // provably-non-escaping parameter produces.
  S.Params[0] = EscapeFact();
  ASSERT_TRUE(S.CandidateParams.empty());
  ASSERT_FALSE(S.This.has_value());
  EXPECT_FALSE(S.empty());
  auto Owned = std::make_unique<ParameterEscapeSummary>(S);
  ASSERT_TRUE(Builder.addSummary(F, std::move(Owned)).second);

  // A second summary where "this" is present but every field inside it is at
  // its absent shape. RoundTripsAllFields now populates "this" fully, so this
  // is the only place those absent shapes are written.
  ParameterEscapeSummary S2;
  S2.IsCandidate = true;
  S2.This = EscapeFact();
  ASSERT_TRUE(S2.Params.empty());
  auto Owned2 = std::make_unique<ParameterEscapeSummary>(S2);
  ASSERT_TRUE(Builder.addSummary(F2, std::move(Owned2)).second);

  PathString Path = makePath("parameter-escape-absent.json");
  JSONFormat Format;
  ASSERT_THAT_ERROR(Format.writeTUSummary(TUSum, Path), llvm::Succeeded());

  // Written-byte assertions here cover: is_candidate's false polarity (the
  // other half of RoundTripsAllFields' true, which no golden fixture can
  // carry), the empty candidate_params and flows_to arrays, and the absent
  // shapes -- "this" absent on the first summary, and returns_self_at and
  // sink absent inside a present "this" on the second. The object comparison
  // further down is a separate check and is not what pins these.
  llvm::Expected<llvm::json::Value> Raw =
      readJSONFromFile("parameter-escape-absent.json");
  ASSERT_THAT_EXPECTED(Raw, llvm::Succeeded());
  const llvm::json::Object *Summary = summaryObjectOf(*Raw);
  ASSERT_NE(Summary, nullptr);
  EXPECT_EQ(Summary->getBoolean("is_candidate"), std::optional<bool>(false));
  const llvm::json::Array *Candidates = Summary->getArray("candidate_params");
  ASSERT_NE(Candidates, nullptr);
  EXPECT_TRUE(Candidates->empty());
  EXPECT_EQ(Summary->get("this"), nullptr);
  const llvm::json::Object *Param0 = paramAt(*Raw, 0);
  ASSERT_NE(Param0, nullptr);
  const llvm::json::Array *Flows0 = Param0->getArray("flows_to");
  ASSERT_NE(Flows0, nullptr);
  EXPECT_TRUE(Flows0->empty());

  // Second summary: "this" present, everything inside it absent or empty.
  const llvm::json::Object *Summary2 = summaryObjectOf(*Raw, 1);
  ASSERT_NE(Summary2, nullptr);

  // The empty side of $.params. The writer omits three keys conditionally, so
  // "be consistent, skip the empty array too" is the natural cleanup against
  // that function -- and it would drop the key from every summary for a
  // function with no pointer-carrying parameters. The key must be present and
  // the array empty, not merely absent.
  const llvm::json::Array *Params2 = Summary2->getArray("params");
  ASSERT_NE(Params2, nullptr);
  EXPECT_TRUE(Params2->empty());

  const llvm::json::Object *This2 = Summary2->getObject("this");
  ASSERT_NE(This2, nullptr);
  const llvm::json::Array *ThisFlows2 = This2->getArray("flows_to");
  ASSERT_NE(ThisFlows2, nullptr);
  EXPECT_TRUE(ThisFlows2->empty());
  EXPECT_EQ(This2->get("returns_self_at"), nullptr);
  EXPECT_EQ(This2->get("sink"), nullptr);

  llvm::Expected<TUSummary> Read = Format.readTUSummary(Path);
  ASSERT_THAT_EXPECTED(Read, llvm::Succeeded());

  ASSERT_TRUE(getIdTable(*Read).contains(nameOf("c:@F@f#*I#")));
  EntityId ReadF = getIdTable(*Read).getId(nameOf("c:@F@f#*I#"));
  auto &Data = getData(*Read);
  auto SummaryIt = Data.find(ParameterEscapeSummary::summaryName());
  ASSERT_NE(SummaryIt, Data.end());
  auto EntityIt = SummaryIt->second.find(ReadF);
  ASSERT_NE(EntityIt, SummaryIt->second.end());
  const auto &RS =
      static_cast<const ParameterEscapeSummary &>(*EntityIt->second);

  EXPECT_TRUE(RS == S);
  EXPECT_FALSE(RS.IsCandidate);
  EXPECT_TRUE(RS.CandidateParams.empty());
  EXPECT_FALSE(RS.This.has_value());
  ASSERT_EQ(RS.Params.count(0), 1u);
  EXPECT_TRUE(RS.Params.at(0).FlowsTo.empty());
  EXPECT_FALSE(RS.Params.at(0).returnsSelf());
  EXPECT_FALSE(RS.Params.at(0).OtherSink.has_value());
}

// A well-formed body, to prove the rejections below are caused by the
// corruption under test and not by the envelope.
static constexpr llvm::StringLiteral WellFormedBody =
    R"({"candidate_params":[0],"is_candidate":true,"params":[{"flows_to":[)"
    R"({"callee":{"@":1},"location":{"column":3,"file":"a.cpp","line":2},)"
    R"("param":-1}],"index":0,"returns_self_at":{"column":1,"file":"a.cpp",)"
    R"("line":1},"sink":{"detail":"d","location":{"column":1,"file":"a.cpp",)"
    R"("line":4},"reason":"StoreToGlobal"}}],"this":{"flows_to":[]}})";

TEST_F(ParameterEscapeFormatTest, AcceptsWellFormedBody) {
  ASSERT_THAT_ERROR(readSummaryBody(WellFormedBody, "pe-well-formed"),
                    llvm::Succeeded());

  // "detail" is the one optional the reader defaults rather than rejects when
  // absent: getOptionalString returns "". The writer always emits the key, so
  // no round trip reaches this branch -- only a body that omits it does.
  // Present-but-malformed is pinned separately by detail-not-string.
  ASSERT_THAT_ERROR(
      readSummaryBody(
          R"({"candidate_params":[],"is_candidate":true,"params":[{)"
          R"("flows_to":[],"index":0,"sink":{"location":{"column":1,)"
          R"("file":"a.cpp","line":4},"reason":"StoreToGlobal"}}]})",
          "pe-sink-without-detail"),
      llvm::Succeeded());
}

// An optional field that is present but malformed must be an error, never
// silently treated as absent: dropping a sink would turn a parameter that
// escapes into one that appears not to, which is the unsound direction.
TEST_F(ParameterEscapeFormatTest, RejectsMalformedOptionalFields) {
  struct Case {
    llvm::StringLiteral Name;
    llvm::StringLiteral Body;
  };
  static constexpr Case Cases[] = {
      {"sink-not-object",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[],"index":0,"sink":5}]})"},
      {"returns-self-at-not-object",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[],"index":0,"returns_self_at":5}]})"},
      {"this-not-object",
       R"({"candidate_params":[],"is_candidate":true,"params":[],"this":5})"},
      {"detail-not-string",
       R"({"candidate_params":[],"is_candidate":true,"params":[{)"
       R"("flows_to":[],"index":0,"sink":{"detail":5,"location":{"column":1,)"
       R"("file":"a.cpp","line":4},"reason":"StoreToGlobal"}}]})"},
  };
  for (const Case &C : Cases) {
    EXPECT_THAT_ERROR(readSummaryBody(C.Body, C.Name), llvm::Failed())
        << "case: " << C.Name.str();
  }
}

// Parameter indices key std::map<unsigned, ...> / std::set<unsigned>, so an
// out-of-range or repeated index would collide with a legitimate parameter.
//
// checkedParamIndex is called from three sites and all three pass Max =
// INT_MAX, even though "index" and "candidate_params" key unsigned containers
// while only "flows_to[].param" is stored as an int here. The reason is that
// all three are narrowed to Node::ParamIndex -- an int whose -1 is
// ThisParamIndex -- on the whole-program side, where nothing can report an
// error any more. Measured before the "index"/"candidate_params" bound was
// tightened from UINT_MAX: a summary holding "index": 4294967295 and
// "candidate_params": [4294967295] made clang-ssaf-analyzer abort inside the
// fixpoint on an assertion, and with assertions off would have annotated
// parameter 4294967295 of a real function. UINT_MAX here was not a loose bound
// that happened to be harmless; it was the defect. Both bounds of all three
// sites are covered below, and the two "wraps-to-this" cases pin the exact
// value, with AcceptsLargestValidParameterIndices pinning the other side.
TEST_F(ParameterEscapeFormatTest, RejectsBadParameterIndices) {
  struct Case {
    llvm::StringLiteral Name;
    llvm::StringLiteral Body;
  };
  static constexpr Case Cases[] = {
      {"index-above-unsigned-max",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[],"index":4294967296}]})"},
      {"index-above-int-max",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[],"index":2147483648}]})"},
      // The value that narrows to ThisParamIndex on the whole-program side.
      {"index-wraps-to-this",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[],"index":4294967295}]})"},
      {"index-negative",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[],"index":-1}]})"},
      {"index-duplicated",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[],"index":1},{"flows_to":[],"index":1}]})"},
      {"candidate-param-above-unsigned-max",
       R"({"candidate_params":[4294967296],"is_candidate":true,"params":[]})"},
      {"candidate-param-above-int-max",
       R"({"candidate_params":[2147483648],"is_candidate":true,"params":[]})"},
      {"candidate-param-wraps-to-this",
       R"({"candidate_params":[4294967295],"is_candidate":true,"params":[]})"},
      {"candidate-param-negative",
       R"({"candidate_params":[-1],"is_candidate":true,"params":[]})"},
      {"flow-param-below-this",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"flows_to":[)"
       R"({"callee":{"@":1},"location":{"column":3,"file":"a.cpp","line":2},)"
       R"("param":-7}],"index":0}]})"},
      {"flow-param-above-int-max",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"flows_to":[)"
       R"({"callee":{"@":1},"location":{"column":3,"file":"a.cpp","line":2},)"
       R"("param":2147483648}],"index":0}]})"},
      {"flow-target-duplicated",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"flows_to":[)"
       R"({"callee":{"@":1},"location":{"column":3,"file":"a.cpp","line":2},)"
       R"("param":0},)"
       R"({"callee":{"@":1},"location":{"column":9,"file":"a.cpp","line":2},)"
       R"("param":0}],"index":0}]})"},
  };
  for (const Case &C : Cases) {
    EXPECT_THAT_ERROR(readSummaryBody(C.Body, C.Name), llvm::Failed())
        << "case: " << C.Name.str();
  }
}

// An array of bare scalars owes an element-type case that no missing-key or
// bound case can stand in for: its elements have no keys to enumerate, and
// every bound fixture parses as an integer and dies one line lower, at the
// bound, so the type check is never the guard that fires. Measured with it
// weakened to "not an integer means 0": the whole suite stayed green while
// candidate_params:["x"] read back as candidate parameter 0.
//
// candidate_params is the only bare-scalar array in this format. Two sibling
// element-type checks here -- flows_to[] and params[] destructured with
// getAsObject() -- are reported separately and left alone in this change.
TEST_F(ParameterEscapeFormatTest, RejectsBadCandidateParamElementTypes) {
  struct Case {
    llvm::StringLiteral Name;
    llvm::StringLiteral Body;
  };
  static constexpr Case Cases[] = {
      {"candidate-param-is-a-string",
       R"({"candidate_params":["x"],"is_candidate":true,"params":[]})"},
      {"candidate-param-is-null",
       R"({"candidate_params":[null],"is_candidate":true,"params":[]})"},
      {"candidate-param-is-an-array",
       R"({"candidate_params":[[0]],"is_candidate":true,"params":[]})"},
      {"candidate-param-is-a-bool",
       R"({"candidate_params":[true],"is_candidate":true,"params":[]})"},
  };
  for (const Case &C : Cases) {
    EXPECT_THAT_ERROR(readSummaryBody(C.Body, C.Name), llvm::Failed())
        << "case: " << C.Name.str();
  }
}

// The accepting side of the bound above. Without this, tightening the index
// bound too far -- to zero, or to anything below INT_MAX -- would reject
// legitimate input with every rejection case above still green.
TEST_F(ParameterEscapeFormatTest, AcceptsLargestValidParameterIndices) {
  llvm::Expected<ParameterEscapeSummary> Parsed = parseSummaryBody(
      R"({"candidate_params":[2147483647],"is_candidate":true,)"
      R"("params":[{"flows_to":[{"callee":{"@":1},)"
      R"("location":{"column":3,"file":"a.cpp","line":2},)"
      R"("param":2147483647}],"index":2147483647}]})",
      "largest-valid-indices");
  ASSERT_THAT_EXPECTED(Parsed, llvm::Succeeded());
  // 2147483647 spelled out rather than INT_MAX: the literal is the bound the
  // reader is held to, not a restatement of the reader's own constant.
  EXPECT_EQ(Parsed->CandidateParams, (std::set<unsigned>{2147483647u}));
  ASSERT_EQ(Parsed->Params.count(2147483647u), 1u);
  const EscapeFact &F = Parsed->Params.at(2147483647u);
  ASSERT_EQ(F.FlowsTo.size(), 1u);
  EXPECT_EQ(F.FlowsTo.begin()->first.ParamIndex, 2147483647);
}

// ThisParamIndex is a legitimate flow target and must survive the range check
// that rejects everything below it.
TEST_F(ParameterEscapeFormatTest, AcceptsThisParamIndexAsFlowTarget) {
  ASSERT_THAT_ERROR(
      readSummaryBody(
          R"({"candidate_params":[],"is_candidate":true,"params":[{)"
          R"("flows_to":[{"callee":{"@":1},"location":{"column":3,)"
          R"("file":"a.cpp","line":2},"param":-1}],"index":0}]})",
          "pe-this-flow-target"),
      llvm::Succeeded());
}

// A required key that is simply missing must be an error, never a default.
//
// Neither a round trip nor the golden fixture can reach these branches: the
// writer always emits the required keys, so nothing ever hands the reader a
// body without one. That makes them the reader's least-exercised code, and
// the "flows_to" case is the unsound direction -- if absent meant empty, a
// truncated or hand-edited summary reads back as EscapeFact{}, which is the
// shape a provably-non-escaping parameter produces, and an escaping parameter
// silently becomes a noescape annotation.
//
// One case per missing key, derived from the six required-field guards in
// ParameterEscapeFormat.cpp rather than from what seemed worth testing. Each
// guard was deleted on its own and the matching case confirmed to go red.
TEST_F(ParameterEscapeFormatTest, RejectsMissingRequiredFields) {
  struct Case {
    llvm::StringLiteral Name;
    llvm::StringLiteral Body;
  };
  static constexpr Case Cases[] = {
      // deserialize(): params / is_candidate / candidate_params
      {"summary-no-params",
       R"({"candidate_params":[],"is_candidate":true})"},
      {"summary-no-is-candidate",
       R"({"candidate_params":[],"params":[]})"},
      {"summary-no-candidate-params",
       R"({"is_candidate":true,"params":[]})"},
      // deserialize(), params element: index
      {"param-no-index",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"flows_to":[]}]})"},
      // escapeFactFromJSON(): flows_to, at both of its call paths
      {"param-no-flows-to",
       R"({"candidate_params":[],"is_candidate":true,)"
       R"("params":[{"index":0}]})"},
      {"this-no-flows-to",
       R"({"candidate_params":[],"is_candidate":true,"params":[],)"
       R"("this":{}})"},
      // escapeFactFromJSON(), flow target: callee / param / location
      {"flow-no-callee",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[{"param":0,"location":{"column":1,"file":"a.cpp",)"
       R"("line":2}}]}]})"},
      {"flow-no-param",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[{"callee":{"@":1},"location":{"column":1,)"
       R"("file":"a.cpp","line":2}}]}]})"},
      {"flow-no-location",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[{"callee":{"@":1},"param":0}]}]})"},
      // escapeFactFromJSON(), sink: reason / location
      {"sink-no-reason",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[],"sink":{"detail":"","location":{"column":1,)"
       R"("file":"a.cpp","line":4}}}]})"},
      {"sink-no-location",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[],"sink":{"detail":"","reason":"StoreToGlobal"}}]})"},
      // sourceLocationRecordFromJSON(): file / line / column
      {"location-no-file",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[],"returns_self_at":{"column":1,"line":2}}]})"},
      {"location-no-line",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[],"returns_self_at":{"column":1,"file":"a.cpp"}}]})"},
      {"location-no-column",
       R"({"candidate_params":[],"is_candidate":true,"params":[{"index":0,)"
       R"("flows_to":[],"returns_self_at":{"file":"a.cpp","line":2}}]})"},
  };
  for (const Case &C : Cases) {
    EXPECT_THAT_ERROR(readSummaryBody(C.Body, C.Name), llvm::Failed())
        << "case: " << C.Name.str();
  }
}

// A repeated candidate_params entry is absorbed, not rejected -- unlike a
// repeated params index or flow target, which are errors. The reader carries a
// comment explaining the difference, but the behaviour needs an assertion too,
// or "make these three consistent" meets no objection.
TEST_F(ParameterEscapeFormatTest, AbsorbsDuplicateCandidateParams) {
  llvm::Expected<ParameterEscapeSummary> S = parseSummaryBody(
      R"({"candidate_params":[1,1],"is_candidate":true,"params":[]})",
      "pe-duplicate-candidate-params");
  ASSERT_THAT_EXPECTED(S, llvm::Succeeded());
  EXPECT_EQ(S->CandidateParams, (std::set<unsigned>{1}));
}

// Locations are advisory, but the reader narrows them to unsigned, so give
// them the same range checking as the parameter indices.
TEST_F(ParameterEscapeFormatTest, RejectsOutOfRangeLocations) {
  struct Case {
    llvm::StringLiteral Name;
    llvm::StringLiteral Body;
  };
  static constexpr Case Cases[] = {
      {"line-negative",
       R"({"candidate_params":[],"is_candidate":true,"params":[{)"
       R"("flows_to":[],"index":0,"returns_self_at":{"column":1,)"
       R"("file":"a.cpp","line":-1}}]})"},
      {"line-above-unsigned-max",
       R"({"candidate_params":[],"is_candidate":true,"params":[{)"
       R"("flows_to":[],"index":0,"returns_self_at":{"column":1,)"
       R"("file":"a.cpp","line":4294967296}}]})"},
      {"column-negative",
       R"({"candidate_params":[],"is_candidate":true,"params":[{)"
       R"("flows_to":[],"index":0,"returns_self_at":{"column":-1,)"
       R"("file":"a.cpp","line":1}}]})"},
  };
  for (const Case &C : Cases) {
    EXPECT_THAT_ERROR(readSummaryBody(C.Body, C.Name), llvm::Failed())
        << "case: " << C.Name.str();
  }
}

// escapeReasonName and parseEscapeReason are inverses of each other by
// construction, so a round trip through them cannot notice a renamed reason.
// These literals are an independent statement of the wire spelling: the enum
// is generated from EscapeReasons.def, this list is not.
TEST_F(ParameterEscapeFormatTest, ReasonNamesHaveExpectedSpellings) {
  struct Expectation {
    EscapeReason Reason;
    llvm::StringLiteral Name;
  };
  static constexpr Expectation Expectations[] = {
      {EscapeReason::Return, "Return"},
      {EscapeReason::StoreToField, "StoreToField"},
      {EscapeReason::StoreToGlobal, "StoreToGlobal"},
      {EscapeReason::StoreThroughPointer, "StoreThroughPointer"},
      {EscapeReason::AddressTaken, "AddressTaken"},
      {EscapeReason::IndirectCall, "IndirectCall"},
      {EscapeReason::UnmatchedArgument, "UnmatchedArgument"},
      {EscapeReason::CastToNonPointer, "CastToNonPointer"},
      {EscapeReason::Throw, "Throw"},
      {EscapeReason::Coroutine, "Coroutine"},
      {EscapeReason::Deallocation, "Deallocation"},
      {EscapeReason::HeapAllocation, "HeapAllocation"},
      {EscapeReason::NonTrivialView, "NonTrivialView"},
      {EscapeReason::Asm, "Asm"},
      {EscapeReason::VarArgs, "VarArgs"},
      {EscapeReason::Capture, "Capture"},
      {EscapeReason::VirtualCall, "VirtualCall"},
      {EscapeReason::ObjCMessage, "ObjCMessage"},
      {EscapeReason::CallableUse, "CallableUse"},
      {EscapeReason::UnnamedCallee, "UnnamedCallee"},
      {EscapeReason::UnanalyzedCallee, "UnanalyzedCallee"},
      {EscapeReason::EscapesViaCallee, "EscapesViaCallee"},
      {EscapeReason::UnrecognizedUse, "UnrecognizedUse"},
  };
  static_assert(std::size(Expectations) ==
                    static_cast<size_t>(EscapeReason::UnrecognizedUse) + 1,
                "every EscapeReason needs its spelling pinned here");
  for (const Expectation &E : Expectations)
    EXPECT_EQ(escapeReasonName(E.Reason), E.Name);
}

TEST_F(ParameterEscapeFormatTest, ReasonNamesRoundTrip) {
  for (unsigned I = 0;
       I <= static_cast<unsigned>(EscapeReason::UnrecognizedUse); ++I) {
    auto R = static_cast<EscapeReason>(I);
    EXPECT_EQ(parseEscapeReason(escapeReasonName(R)), std::optional(R));
  }
  EXPECT_FALSE(parseEscapeReason("Bogus").has_value());
}

} // namespace
} // namespace clang::ssaf
