//===- ParameterEscapeResultFormatTest.cpp --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests for the JSON format of the two whole-program noescape results,
// ParameterEscapeResult and NonEscapingParametersResult.
//
// THE INVARIANT THIS FILE EXISTS TO MAINTAIN
//
//   Every leaf of the wire format has one written-byte assertion against a
//   literal, at every path where that leaf can appear, and no two paths use
//   the same literal.
//
// Adding a field to either result means adding its assertion in the same
// change. See ParameterEscapeFormatTest.cpp for the argument in full: a round
// trip and a golden fixture are both closed under any transform applied
// symmetrically to the writer and the reader, so neither can see a
// transposition, a renaming, a polarity inversion, or a swap of two same-typed
// sibling fields. Only literals stated outside the writer/reader pair can.
//
// THE LEAF SET
//
// Derived by walking serializeParameterEscapeResult(),
// serializeNonEscapingParametersResult(), nodeToJSON(), and the shared
// escapeFactToJSON()/sourceLocationRecordToJSON() in
// ParameterEscapeAnalysis.cpp and ParameterEscapeFormat.cpp. "$" is the
// "result" object of one WPASuite entry.
//
//   ParameterEscapeResult -- 17 scalar leaves, by path:
//
//     $.facts[].node.function.@                            1
//     $.facts[].node.param                                 1
//     $.facts[].flows_to[].callee.@                        1
//     $.facts[].flows_to[].param                           1
//     $.facts[].flows_to[].location.{file,line,column}     3
//     $.facts[].returns_self_at.{file,line,column}         3
//     $.facts[].sink.reason                                1
//     $.facts[].sink.detail                                1
//     $.facts[].sink.location.{file,line,column}           3
//     $.candidates[].function.@                            1
//     $.candidates[].param                                 1
//
//   NonEscapingParametersResult -- 12 scalar leaves, by path:
//
//     $.non_escaping[].function.@                          1
//     $.non_escaping[].params[]                            1
//     $.rejected[].node.function.@                         1
//     $.rejected[].node.param                              1
//     $.rejected[].reason                                  1
//     $.rejected[].detail                                  1
//     $.rejected[].location.{file,line,column}             3
//     $.rejected[].blame.function.@                        1
//     $.rejected[].blame.param                             1
//
// Note that "node" and "blame" are the same Node type at two paths, and so
// are two distinct sets of leaves: a swap between them is invisible from
// either one alone, which is why every literal below is distinct.
//
// THE SHAPE SET
//
// Every Array the writers construct and every key they emit under an `if`,
// multiplied by the paths its enclosing function is called from. Both sides
// of every entry need an assertion.
//
//   ParameterEscapeResult -- 3 arrays + 2 guarded keys:
//     array  $.facts                      empty / non-empty
//     array  $.candidates                 empty / non-empty
//     array  $.facts[].flows_to           empty / non-empty
//     key    $.facts[].returns_self_at    absent / present
//     key    $.facts[].sink               absent / present
//
//   NonEscapingParametersResult -- 3 arrays + 1 guarded key:
//     array  $.non_escaping               empty / non-empty
//     array  $.non_escaping[].params      empty / non-empty
//     array  $.rejected                   empty / non-empty
//     key    $.rejected[].blame           absent / present
//
// escapeFactToJSON() is called from exactly one path here ($.facts[]), unlike
// in the summary format where it is called from two.
//
// THE READER'S FOUR CENSUSES
//
// The leaf and shape sets above are derived from the WRITER. They say nothing
// about the reader, which needs its own enumerations -- four of them, because
// each is reached by a different kind of malformed input and no one of them
// implies another. They live with the tests below; this is the index.
//
//   1. Required keys. One case per field whose absence must be an error, per
//      path. Keys read with getObject/getArray/getString conflate "absent"
//      with "present but wrong type", so one case covers both; keys read with
//      get() do not, and those get a separate wrong-type case (see 3).
//   2. checkedParamIndex bounds. Both sides of both bounds at every call site,
//      with the site's own Min.
//   3. Element types. One case per array element the reader destructures. The
//      bound census does NOT reach these: every fixture it uses parses as the
//      expected type and dies one line lower, at the bound, so the type check
//      is never the guard that fires. Measured: replacing
//      non_escaping[].params[]'s check with "absent means 0" left the whole
//      suite green while clang-ssaf-format accepted "params":["x"] and wrote
//      it back as [0] -- a corrupt field turned into the shape that means
//      "parameter 0 provably does not escape".
//   4. Duplicate map keys. One case per emplace(...).second rejection. These
//      are not inversions, but each is a guard written on purpose to stop a
//      repeat silently discarding a fact, and an untested one is not evidence.
//
// An array of bare scalars owes census 3 a case even though it owes census 1
// nothing: its elements have no keys to enumerate.
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"

#include "JSONFormatTest.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/EntityLinker/LUSummary.h"
#include "clang/ScalableStaticAnalysis/Core/Model/BuildNamespace.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityLinkage.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisDriver.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/WPASuite.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace clang::ssaf {
namespace {

class ParameterEscapeResultFormatTest : public JSONFormatTest {
protected:
  NestedBuildNamespace NS{{BuildNamespace(BuildNamespaceKind::LinkUnit, "LU")}};

  // Entity ids are the integers the "@" leaves carry, so the fixtures below
  // intern e0..e5 up front and then index them positionally.
  std::vector<EntityId> Es;
  WPASuite Suite = makeWPASuite();

  void SetUp() override {
    JSONFormatTest::SetUp();
    for (unsigned I = 0; I < 6; ++I)
      Es.push_back(getIdTable(Suite).getId(
          EntityName(("c:@F@e" + std::to_string(I)), "", NS)));
  }

  void put(std::unique_ptr<AnalysisResult> R, AnalysisName Name) {
    getData(Suite)[std::move(Name)] = std::move(R);
  }

  // Writes the suite and returns the "result" object of the entry named
  // \p Name, as re-read from the file. Returns null rather than asserting, so
  // callers report the failure at their own line.
  llvm::Expected<llvm::json::Value> writeAndRead(llvm::StringRef FileName) {
    JSONFormat Format;
    PathString Path = makePath(FileName);
    if (auto Err = Format.writeWPASuite(Suite, Path))
      return std::move(Err);
    return readJSONFromFile(FileName);
  }

  static const llvm::json::Object *resultOf(const llvm::json::Value &Root,
                                            llvm::StringRef Name) {
    const llvm::json::Object *O = Root.getAsObject();
    const llvm::json::Array *Results = O ? O->getArray("results") : nullptr;
    if (!Results)
      return nullptr;
    for (const llvm::json::Value &V : *Results) {
      const llvm::json::Object *Entry = V.getAsObject();
      if (!Entry)
        continue;
      if (Entry->getString("analysis_name") == Name)
        return Entry->getObject("result");
    }
    return nullptr;
  }

  // The "@" below is JSONFormat's entity-id key, not this format's. It is
  // spelled as a literal on purpose: that puts the key outside the
  // writer/reader pair, so a symmetric rename of JSONEntityIdKey fails these
  // tests instead of silently changing the format. Do not "clarify" it into
  // JSONEntityIdKey, for the same reason the literal -1 below must not become
  // ThisParamIndex.
  static std::optional<int64_t> functionOf(const llvm::json::Object *O,
                                           llvm::StringRef Key) {
    const llvm::json::Object *N = O ? O->getObject(Key) : nullptr;
    const llvm::json::Object *F = N ? N->getObject("function") : nullptr;
    return F ? F->getInteger("@") : std::nullopt;
  }

  static std::optional<int64_t> paramOf(const llvm::json::Object *O,
                                        llvm::StringRef Key) {
    const llvm::json::Object *N = O ? O->getObject(Key) : nullptr;
    return N ? N->getInteger("param") : std::nullopt;
  }

  static const llvm::json::Object *nth(const llvm::json::Array *A, size_t I) {
    return A && I < A->size() ? (*A)[I].getAsObject() : nullptr;
  }

  // Asserts a location object leaf by leaf. Line and column are always
  // different within a fixture location so a transposition cannot hide.
  static void expectLocation(const llvm::json::Object *Loc,
                             llvm::StringRef File, int64_t Line,
                             int64_t Column) {
    ASSERT_NE(Loc, nullptr);
    EXPECT_EQ(Loc->getString("file"), File);
    EXPECT_EQ(Loc->getInteger("line"), std::optional<int64_t>(Line));
    EXPECT_EQ(Loc->getInteger("column"), std::optional<int64_t>(Column));
  }

  // A fully populated ParameterEscapeResult: one fact with every optional
  // present and a non-empty flows_to, one fact with all of them absent, and
  // two candidates that share no literal with either fact's node.
  std::unique_ptr<ParameterEscapeResult> fullEscapeResult() const {
    auto R = std::make_unique<ParameterEscapeResult>();

    EscapeFact Loud;
    Loud.FlowsTo[FlowTarget{Es[1], 4}] =
        SourceLocationRecord{"flow-a.cpp", 11, 12};
    Loud.ReturnsSelfAt = SourceLocationRecord{"ret-a.cpp", 21, 22};
    Loud.OtherSink =
        Sink{EscapeReason::StoreToGlobal,
             SourceLocationRecord{"sink-a.cpp", 31, 32}, "detail-a"};
    R->Facts[Node{Es[0], 3}] = std::move(Loud);
    R->Facts[Node{Es[2], ThisParamIndex}] = EscapeFact();

    R->Candidates.insert(Node{Es[1], 6});
    R->Candidates.insert(Node{Es[3], 5});
    return R;
  }

  // A fully populated NonEscapingParametersResult: one function with two
  // indices and one with none, one rejection with a blame and one without.
  std::unique_ptr<NonEscapingParametersResult> fullVerdict() const {
    auto R = std::make_unique<NonEscapingParametersResult>();

    R->NonEscaping[Es[0]] = {7, 8};
    R->NonEscaping[Es[1]] = {};

    NonEscapingParametersResult::Rejection Blamed;
    Blamed.Reason = EscapeReason::Asm;
    Blamed.Location = SourceLocationRecord{"rej-a.cpp", 41, 42};
    Blamed.Detail = "detail-r";
    Blamed.Blame = Node{Es[3], 10};
    R->Rejected[Node{Es[2], 9}] = std::move(Blamed);

    NonEscapingParametersResult::Rejection Bare;
    Bare.Reason = EscapeReason::Return;
    Bare.Location = SourceLocationRecord{"rej-b.cpp", 51, 52};
    R->Rejected[Node{Es[4], 11}] = std::move(Bare);
    return R;
  }
};

//===----------------------------------------------------------------------===//
// ParameterEscapeResult: written bytes
//===----------------------------------------------------------------------===//

TEST_F(ParameterEscapeResultFormatTest, EscapeResultWritesEveryLeaf) {
  put(fullEscapeResult(), ParameterEscapeResult::analysisName());

  llvm::Expected<llvm::json::Value> Root = writeAndRead("escape-full.json");
  ASSERT_THAT_EXPECTED(Root, llvm::Succeeded());

  const llvm::json::Object *Result = resultOf(*Root, "ParameterEscapeResult");
  ASSERT_NE(Result, nullptr);
  const llvm::json::Object *Content =
      Result->getObject("ParameterEscapeResult");
  ASSERT_NE(Content, nullptr);

  const llvm::json::Array *Facts = Content->getArray("facts");
  ASSERT_NE(Facts, nullptr);
  ASSERT_EQ(Facts->size(), 2u);

  // $.facts[0] -- every optional present.
  const llvm::json::Object *Loud = nth(Facts, 0);
  ASSERT_NE(Loud, nullptr);
  EXPECT_EQ(functionOf(Loud, "node"), std::optional<int64_t>(0));
  EXPECT_EQ(paramOf(Loud, "node"), std::optional<int64_t>(3));

  const llvm::json::Array *Flows = Loud->getArray("flows_to");
  ASSERT_NE(Flows, nullptr);
  ASSERT_EQ(Flows->size(), 1u);
  const llvm::json::Object *Flow = nth(Flows, 0);
  ASSERT_NE(Flow, nullptr);
  const llvm::json::Object *Callee = Flow->getObject("callee");
  ASSERT_NE(Callee, nullptr);
  EXPECT_EQ(Callee->getInteger("@"), std::optional<int64_t>(1));
  EXPECT_EQ(Flow->getInteger("param"), std::optional<int64_t>(4));
  expectLocation(Flow->getObject("location"), "flow-a.cpp", 11, 12);

  expectLocation(Loud->getObject("returns_self_at"), "ret-a.cpp", 21, 22);

  const llvm::json::Object *SinkObj = Loud->getObject("sink");
  ASSERT_NE(SinkObj, nullptr);
  EXPECT_EQ(SinkObj->getString("reason"), "StoreToGlobal");
  EXPECT_EQ(SinkObj->getString("detail"), "detail-a");
  expectLocation(SinkObj->getObject("location"), "sink-a.cpp", 31, 32);

  // $.facts[1] -- the absent/empty side of all three shapes.
  const llvm::json::Object *Quiet = nth(Facts, 1);
  ASSERT_NE(Quiet, nullptr);
  EXPECT_EQ(functionOf(Quiet, "node"), std::optional<int64_t>(2));
  // The literal -1 is deliberate: writing ThisParamIndex here would make the
  // assertion agree with the writer by construction.
  EXPECT_EQ(paramOf(Quiet, "node"), std::optional<int64_t>(-1));
  const llvm::json::Array *NoFlows = Quiet->getArray("flows_to");
  ASSERT_NE(NoFlows, nullptr);
  EXPECT_TRUE(NoFlows->empty());
  EXPECT_EQ(Quiet->get("returns_self_at"), nullptr);
  EXPECT_EQ(Quiet->get("sink"), nullptr);

  // $.candidates -- a second Node path, with literals shared by nothing.
  const llvm::json::Array *Candidates = Content->getArray("candidates");
  ASSERT_NE(Candidates, nullptr);
  ASSERT_EQ(Candidates->size(), 2u);
  const llvm::json::Object *C0 = nth(Candidates, 0);
  const llvm::json::Object *C1 = nth(Candidates, 1);
  ASSERT_NE(C0, nullptr);
  ASSERT_NE(C1, nullptr);
  const llvm::json::Object *C0Fn = C0->getObject("function");
  const llvm::json::Object *C1Fn = C1->getObject("function");
  ASSERT_NE(C0Fn, nullptr);
  ASSERT_NE(C1Fn, nullptr);
  EXPECT_EQ(C0Fn->getInteger("@"), std::optional<int64_t>(1));
  EXPECT_EQ(C0->getInteger("param"), std::optional<int64_t>(6));
  EXPECT_EQ(C1Fn->getInteger("@"), std::optional<int64_t>(3));
  EXPECT_EQ(C1->getInteger("param"), std::optional<int64_t>(5));
}

TEST_F(ParameterEscapeResultFormatTest, EscapeResultWritesEmptyArrays) {
  put(std::make_unique<ParameterEscapeResult>(),
      ParameterEscapeResult::analysisName());

  llvm::Expected<llvm::json::Value> Root = writeAndRead("escape-empty.json");
  ASSERT_THAT_EXPECTED(Root, llvm::Succeeded());

  const llvm::json::Object *Result = resultOf(*Root, "ParameterEscapeResult");
  ASSERT_NE(Result, nullptr);
  const llvm::json::Object *Content =
      Result->getObject("ParameterEscapeResult");
  ASSERT_NE(Content, nullptr);

  const llvm::json::Array *Facts = Content->getArray("facts");
  const llvm::json::Array *Candidates = Content->getArray("candidates");
  ASSERT_NE(Facts, nullptr);
  ASSERT_NE(Candidates, nullptr);
  EXPECT_TRUE(Facts->empty());
  EXPECT_TRUE(Candidates->empty());
}

//===----------------------------------------------------------------------===//
// NonEscapingParametersResult: written bytes
//===----------------------------------------------------------------------===//

TEST_F(ParameterEscapeResultFormatTest, VerdictWritesEveryLeaf) {
  put(fullVerdict(), NonEscapingParametersResult::analysisName());

  llvm::Expected<llvm::json::Value> Root = writeAndRead("verdict-full.json");
  ASSERT_THAT_EXPECTED(Root, llvm::Succeeded());

  const llvm::json::Object *Result =
      resultOf(*Root, "NonEscapingParametersResult");
  ASSERT_NE(Result, nullptr);
  const llvm::json::Object *Content =
      Result->getObject("NonEscapingParametersResult");
  ASSERT_NE(Content, nullptr);

  const llvm::json::Array *NonEscaping = Content->getArray("non_escaping");
  ASSERT_NE(NonEscaping, nullptr);
  ASSERT_EQ(NonEscaping->size(), 2u);

  const llvm::json::Object *N0 = nth(NonEscaping, 0);
  ASSERT_NE(N0, nullptr);
  const llvm::json::Object *N0Fn = N0->getObject("function");
  ASSERT_NE(N0Fn, nullptr);
  EXPECT_EQ(N0Fn->getInteger("@"), std::optional<int64_t>(0));
  const llvm::json::Array *N0Params = N0->getArray("params");
  ASSERT_NE(N0Params, nullptr);
  ASSERT_EQ(N0Params->size(), 2u);
  EXPECT_EQ((*N0Params)[0].getAsInteger(), std::optional<int64_t>(7));
  EXPECT_EQ((*N0Params)[1].getAsInteger(), std::optional<int64_t>(8));

  // The empty side of $.non_escaping[].params.
  const llvm::json::Object *N1 = nth(NonEscaping, 1);
  ASSERT_NE(N1, nullptr);
  const llvm::json::Object *N1Fn = N1->getObject("function");
  ASSERT_NE(N1Fn, nullptr);
  EXPECT_EQ(N1Fn->getInteger("@"), std::optional<int64_t>(1));
  const llvm::json::Array *N1Params = N1->getArray("params");
  ASSERT_NE(N1Params, nullptr);
  EXPECT_TRUE(N1Params->empty());

  const llvm::json::Array *Rejected = Content->getArray("rejected");
  ASSERT_NE(Rejected, nullptr);
  ASSERT_EQ(Rejected->size(), 2u);

  // $.rejected[0] -- blame present.
  const llvm::json::Object *R0 = nth(Rejected, 0);
  ASSERT_NE(R0, nullptr);
  EXPECT_EQ(functionOf(R0, "node"), std::optional<int64_t>(2));
  EXPECT_EQ(paramOf(R0, "node"), std::optional<int64_t>(9));
  EXPECT_EQ(R0->getString("reason"), "Asm");
  EXPECT_EQ(R0->getString("detail"), "detail-r");
  expectLocation(R0->getObject("location"), "rej-a.cpp", 41, 42);
  EXPECT_EQ(functionOf(R0, "blame"), std::optional<int64_t>(3));
  EXPECT_EQ(paramOf(R0, "blame"), std::optional<int64_t>(10));

  // $.rejected[1] -- blame absent, detail empty but still written.
  const llvm::json::Object *R1 = nth(Rejected, 1);
  ASSERT_NE(R1, nullptr);
  EXPECT_EQ(functionOf(R1, "node"), std::optional<int64_t>(4));
  EXPECT_EQ(paramOf(R1, "node"), std::optional<int64_t>(11));
  EXPECT_EQ(R1->getString("reason"), "Return");
  EXPECT_EQ(R1->getString("detail"), "");
  expectLocation(R1->getObject("location"), "rej-b.cpp", 51, 52);
  EXPECT_EQ(R1->get("blame"), nullptr);
}

TEST_F(ParameterEscapeResultFormatTest, VerdictWritesEmptyArrays) {
  put(std::make_unique<NonEscapingParametersResult>(),
      NonEscapingParametersResult::analysisName());

  llvm::Expected<llvm::json::Value> Root = writeAndRead("verdict-empty.json");
  ASSERT_THAT_EXPECTED(Root, llvm::Succeeded());

  const llvm::json::Object *Result =
      resultOf(*Root, "NonEscapingParametersResult");
  ASSERT_NE(Result, nullptr);
  const llvm::json::Object *Content =
      Result->getObject("NonEscapingParametersResult");
  ASSERT_NE(Content, nullptr);

  const llvm::json::Array *NonEscaping = Content->getArray("non_escaping");
  const llvm::json::Array *Rejected = Content->getArray("rejected");
  ASSERT_NE(NonEscaping, nullptr);
  ASSERT_NE(Rejected, nullptr);
  EXPECT_TRUE(NonEscaping->empty());
  EXPECT_TRUE(Rejected->empty());
}

//===----------------------------------------------------------------------===//
// Round trips
//===----------------------------------------------------------------------===//

TEST_F(ParameterEscapeResultFormatTest, RoundTripsBothResults) {
  std::unique_ptr<ParameterEscapeResult> Escape = fullEscapeResult();
  std::unique_ptr<NonEscapingParametersResult> Verdict = fullVerdict();
  const ParameterEscapeResult ExpectedEscape = *Escape;
  const NonEscapingParametersResult ExpectedVerdict = *Verdict;

  put(std::move(Escape), ParameterEscapeResult::analysisName());
  put(std::move(Verdict), NonEscapingParametersResult::analysisName());

  JSONFormat Format;
  PathString Path = makePath("round-trip.json");
  ASSERT_THAT_ERROR(Format.writeWPASuite(Suite, Path), llvm::Succeeded());

  llvm::Expected<WPASuite> Read = Format.readWPASuite(Path);
  ASSERT_THAT_EXPECTED(Read, llvm::Succeeded());

  llvm::Expected<const ParameterEscapeResult &> GotEscape =
      Read->get<ParameterEscapeResult>();
  ASSERT_THAT_EXPECTED(GotEscape, llvm::Succeeded());
  EXPECT_EQ(GotEscape->Facts, ExpectedEscape.Facts);
  EXPECT_EQ(GotEscape->Candidates, ExpectedEscape.Candidates);

  llvm::Expected<const NonEscapingParametersResult &> GotVerdict =
      Read->get<NonEscapingParametersResult>();
  ASSERT_THAT_EXPECTED(GotVerdict, llvm::Succeeded());
  EXPECT_EQ(GotVerdict->NonEscaping, ExpectedVerdict.NonEscaping);
  EXPECT_EQ(GotVerdict->Rejected, ExpectedVerdict.Rejected);
}

//===----------------------------------------------------------------------===//
// Reader rejections
//===----------------------------------------------------------------------===//

class ParameterEscapeResultReaderTest : public JSONFormatTest {
protected:
  struct Case {
    llvm::StringLiteral Name;
    llvm::StringLiteral Body;
  };

  // Wraps one result body in a minimal WPASuite envelope, so a test can hand
  // the reader exactly the bytes it wants.
  llvm::Error read(llvm::StringRef Analysis, llvm::StringRef Body,
                   llvm::StringRef FileName) {
    std::string JSON =
        ("{\"id_table\":[],\"results\":[{\"analysis_name\":\"" + Analysis +
         "\",\"result\":" + Body + "}],\"type\":\"WPASuite\"}")
            .str();
    llvm::Expected<PathString> Path = writeJSON(JSON, FileName);
    if (!Path)
      return Path.takeError();
    JSONFormat Format;
    llvm::Expected<WPASuite> Suite = Format.readWPASuite(*Path);
    if (!Suite)
      return Suite.takeError();
    return llvm::Error::success();
  }

  void expectAllRejected(llvm::StringRef Analysis, llvm::ArrayRef<Case> Cases) {
    for (const Case &C : Cases)
      EXPECT_THAT_ERROR(read(Analysis, C.Body, C.Name.str() + ".json"),
                        llvm::Failed())
          << "case: " << C.Name.str();
  }

  void expectAllAccepted(llvm::StringRef Analysis, llvm::ArrayRef<Case> Cases) {
    for (const Case &C : Cases)
      EXPECT_THAT_ERROR(read(Analysis, C.Body, C.Name.str() + "-ok.json"),
                        llvm::Succeeded())
          << "case: " << C.Name.str();
  }
};

// CENSUS 1: REQUIRED KEYS
//
// One case per required key per path, derived by reading
// deserializeParameterEscapeResult() and
// deserializeNonEscapingParametersResult() and listing every field whose
// absence the reader must refuse -- including the ones inside nodeFromJSON()
// and the flows_to that escapeFactFromJSON() demands, because each call site
// is its own path.
//
// These are not a variation on the top-level array cases. Each guard here
// only ever widens the accepted set when it is weakened, so no other test in
// this file can fail in its place. Measured before these existed: a
// non_escaping entry spelled {"params":[0]} with the function key absent was
// accepted and attributed to entity 0, producing a verdict that annotates
// parameter 0 of a function the fixpoint never cleared -- with the whole
// suite green.

TEST_F(ParameterEscapeResultReaderTest, EscapeResultRejectsMissingKeys) {
  static constexpr Case Cases[] = {
      {"outer-key",
       R"({"NotParameterEscapeResult":{"candidates":[],"facts":[]}})"},
      {"facts", R"({"ParameterEscapeResult":{"candidates":[]}})"},
      {"candidates", R"({"ParameterEscapeResult":{"facts":[]}})"},
      {"fact-node",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[)"
       R"(]}]}})"},
      {"fact-node-function",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[],)"
       R"("node":{"param":0}}]}})"},
      {"fact-node-param",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[],)"
       R"("node":{"function":{"@":0}}}]}})"},
      {"fact-flows-to",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"node":{)"
       R"("function":{"@":0},"param":0}}]}})"},
      {"candidate-function",
       R"({"ParameterEscapeResult":{"candidates":[{"param":0}],"facts":[{)"
       R"("flows_to":[],"node":{"function":{"@":0},"param":0}}]}})"},
      {"candidate-param",
       R"({"ParameterEscapeResult":{"candidates":[{"function":{"@":0}}],)"
       R"("facts":[{"flows_to":[],"node":{"function":{"@":0},"param":0}}]}})"},
  };
  expectAllRejected("ParameterEscapeResult", Cases);
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsMissingKeys) {
  static constexpr Case Cases[] = {
      {"outer-key",
       R"({"NotNonEscapingParametersResult":{"non_escaping":[],"rejected":[]}})"},
      {"non-escaping", R"({"NonEscapingParametersResult":{"rejected":[]}})"},
      {"rejected", R"({"NonEscapingParametersResult":{"non_escaping":[]}})"},
      {"non-escaping-function",
       R"({"NonEscapingParametersResult":{"non_escaping":[{"params":[0]}],)"
       R"("rejected":[]}})"},
      {"non-escaping-params",
       R"({"NonEscapingParametersResult":{"non_escaping":[{"function":{)"
       R"("@":0}}],"rejected":[]}})"},
      {"rejection-node",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"reason":"Asm"}]}})"},
      {"rejection-reason",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"function":{)"
       R"("@":0},"param":0}}]}})"},
      {"rejection-location",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("node":{"function":{"@":0},"param":0},"reason":"Asm"}]}})"},
      {"rejection-node-function",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"param":0},)"
       R"("reason":"Asm"}]}})"},
      {"rejection-node-param",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"function":{)"
       R"("@":0}},"reason":"Asm"}]}})"},
      {"blame-function",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("blame":{"param":0},"location":{"column":2,"file":"a.cpp","line":1},)"
       R"("node":{"function":{"@":0},"param":0},"reason":"Asm"}]}})"},
      {"blame-param",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("blame":{"function":{"@":0}},"location":{"column":2,"file":"a.cpp",)"
       R"("line":1},"node":{"function":{"@":0},"param":0},"reason":"Asm"}]}})"},
  };
  expectAllRejected("NonEscapingParametersResult", Cases);
}

// CENSUS 2: THE checkedParamIndex BOUNDS
//
// checkedParamIndex is called from five sites in these two readers: the four
// inside nodeFromJSON, plus non_escaping[].params[] directly. Both sides of
// both bounds are covered at all five.
//
// nodeFromJSON() is called from four sites and the lower bounds are NOT
// uniform: facts[].node and rejected[].blame admit ThisParamIndex, because
// Facts is keyed by every summarized parameter and a caller can flow into a
// callee's implicit object parameter; candidates[] and rejected[].node do
// not, because both hold parameters that must be spellable as noescape.
// Passing Min per site is what makes ParameterEscapeResult::Candidates'
// documented "never holds a this node" unrepresentable rather than argued --
// measured before it did: a result holding
// "candidates":[{"function":{"@":0},"param":-1}] round-tripped through
// clang-ssaf-format with exit 0.
//
// The upper bound is INT_MAX at all four, for the reason the summary reader's
// is: 4294967295 fits the JSON integer but narrows to ThisParamIndex, and
// 2147483648 to some other wrapped value. Measured before these cases
// existed: widening Max to the int64_t maximum left the whole suite green
// while clang-ssaf-format wrote "param": 4294967295 back as -1.
//
// Both sides of both bounds are covered at all four sites.

TEST_F(ParameterEscapeResultReaderTest, EscapeResultRejectsBadNodeIndices) {
  static constexpr Case Cases[] = {
      {"fact-node-below-this",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[],)"
       R"("node":{"function":{"@":0},"param":-2}}]}})"},
      {"fact-node-above-int-max",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[],)"
       R"("node":{"function":{"@":0},"param":2147483648}}]}})"},
      {"fact-node-wraps-to-this",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[],)"
       R"("node":{"function":{"@":0},"param":4294967295}}]}})"},
      {"candidate-is-this",
       R"({"ParameterEscapeResult":{"candidates":[{"function":{"@":0},)"
       R"("param":-1}],"facts":[{"flows_to":[],"node":{"function":{"@":0},)"
       R"("param":0}}]}})"},
      {"candidate-below-this",
       R"({"ParameterEscapeResult":{"candidates":[{"function":{"@":0},)"
       R"("param":-2}],"facts":[{"flows_to":[],"node":{"function":{"@":0},)"
       R"("param":0}}]}})"},
      {"candidate-above-int-max",
       R"({"ParameterEscapeResult":{"candidates":[{"function":{"@":0},)"
       R"("param":2147483648}],"facts":[{"flows_to":[],"node":{"function":{)"
       R"("@":0},"param":0}}]}})"},
      {"candidate-wraps-to-this",
       R"({"ParameterEscapeResult":{"candidates":[{"function":{"@":0},)"
       R"("param":4294967295}],"facts":[{"flows_to":[],"node":{"function":{)"
       R"("@":0},"param":0}}]}})"},
  };
  expectAllRejected("ParameterEscapeResult", Cases);
}

TEST_F(ParameterEscapeResultReaderTest, EscapeResultAcceptsValidNodeIndices) {
  static constexpr Case Cases[] = {
      {"fact-node-is-this",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[],)"
       R"("node":{"function":{"@":0},"param":-1}}]}})"},
      {"fact-node-at-int-max",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{"flows_to":[],)"
       R"("node":{"function":{"@":0},"param":2147483647}}]}})"},
      {"candidate-at-zero",
       R"({"ParameterEscapeResult":{"candidates":[{"function":{"@":0},)"
       R"("param":0}],"facts":[{"flows_to":[],"node":{"function":{"@":0},)"
       R"("param":0}}]}})"},
      {"candidate-at-int-max",
       R"({"ParameterEscapeResult":{"candidates":[{"function":{"@":0},)"
       R"("param":2147483647}],"facts":[{"flows_to":[],"node":{"function":{)"
       R"("@":0},"param":0}}]}})"},
  };
  expectAllAccepted("ParameterEscapeResult", Cases);
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsBadNodeIndices) {
  static constexpr Case Cases[] = {
      {"rejection-node-below-this",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{)"
       R"("function":{"@":0},"param":-2},"reason":"Asm"}]}})"},
      {"rejection-node-is-this",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"function":{)"
       R"("@":0},"param":-1},"reason":"Asm"}]}})"},
      {"rejection-node-above-int-max",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"function":{)"
       R"("@":0},"param":2147483648},"reason":"Asm"}]}})"},
      {"rejection-node-wraps-to-this",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"function":{)"
       R"("@":0},"param":4294967295},"reason":"Asm"}]}})"},
      {"blame-below-this",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("blame":{"function":{"@":0},"param":-2},"location":{"column":2,)"
       R"("file":"a.cpp","line":1},"node":{"function":{"@":0},"param":0},)"
       R"("reason":"Asm"}]}})"},
      {"blame-above-int-max",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("blame":{"function":{"@":0},"param":2147483648},"location":{)"
       R"("column":2,"file":"a.cpp","line":1},"node":{"function":{"@":0},)"
       R"("param":0},"reason":"Asm"}]}})"},
      {"blame-wraps-to-this",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("blame":{"function":{"@":0},"param":4294967295},"location":{)"
       R"("column":2,"file":"a.cpp","line":1},"node":{"function":{"@":0},)"
       R"("param":0},"reason":"Asm"}]}})"},
  };
  expectAllRejected("NonEscapingParametersResult", Cases);
}

TEST_F(ParameterEscapeResultReaderTest, VerdictAcceptsValidNodeIndices) {
  static constexpr Case Cases[] = {
      {"rejection-node-at-zero",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"function":{)"
       R"("@":0},"param":0},"reason":"Asm"}]}})"},
      {"rejection-node-at-int-max",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":{"function":{)"
       R"("@":0},"param":2147483647},"reason":"Asm"}]}})"},
      {"blame-is-this",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("blame":{"function":{"@":0},"param":-1},"location":{"column":2,)"
       R"("file":"a.cpp","line":1},"node":{"function":{"@":0},"param":0},)"
       R"("reason":"Asm"}]}})"},
      {"blame-at-int-max",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("blame":{"function":{"@":0},"param":2147483647},"location":{)"
       R"("column":2,"file":"a.cpp","line":1},"node":{"function":{"@":0},)"
       R"("param":0},"reason":"Asm"}]}})"},
  };
  expectAllAccepted("NonEscapingParametersResult", Cases);
}

// CENSUS 4: DUPLICATE MAP KEYS
//
// One case per emplace(...).second rejection, of which these readers have
// three: facts[], non_escaping[] and rejected[]. None is an inversion -- a
// repeat cannot make something escape that did not -- but each stops a repeat
// silently discarding the losing entry's facts, and the rejected[] one went
// untested for two rounds while its two siblings were pinned.

TEST_F(ParameterEscapeResultReaderTest, EscapeResultRejectsDuplicateFactNode) {
  EXPECT_THAT_ERROR(
      read("ParameterEscapeResult",
           R"({"ParameterEscapeResult":{"candidates":[],"facts":[)"
           R"({"flows_to":[],"node":{"function":{"@":0},"param":0}},)"
           R"({"flows_to":[],"node":{"function":{"@":0},"param":0}}]}})",
           "dup-fact.json"),
      llvm::Failed());
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsDuplicateFunction) {
  EXPECT_THAT_ERROR(
      read("NonEscapingParametersResult",
           R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[)"
           R"({"function":{"@":0},"params":[0]},)"
           R"({"function":{"@":0},"params":[1]}]}})",
           "dup-function.json"),
      llvm::Failed());
}

// The fifth checkedParamIndex site. Every index that lands here is spelled as
// __attribute__((noescape)) on a real parameter, so a value the writer cannot
// produce is a wrong annotation rather than a cosmetic defect.
TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsOutOfRangeAnnotation) {
  static constexpr Case Cases[] = {
      {"annotation-negative",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[-1]}]}})"},
      {"annotation-above-int-max",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[2147483648]}]}})"},
      {"annotation-wraps-to-this",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[4294967295]}]}})"},
      {"annotation-above-unsigned-max",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[4294967296]}]}})"},
  };
  expectAllRejected("NonEscapingParametersResult", Cases);
}

TEST_F(ParameterEscapeResultReaderTest, VerdictAcceptsValidAnnotations) {
  static constexpr Case Cases[] = {
      {"annotation-at-zero",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[0]}]}})"},
      {"annotation-at-int-max",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[2147483647]}]}})"},
  };
  expectAllAccepted("NonEscapingParametersResult", Cases);
}

// CENSUS 3: ELEMENT TYPES
//
// One case per array element the reader destructures, derived by listing every
// getAsObject() and getAsInteger() the two deserializers perform on an array
// element. The bound census cannot stand in for this: its fixtures all parse
// as the expected type and die one line lower.
//
// params is the dangerous one and the reason this census exists. Measured with
// its check weakened to "not an integer means 0": the whole suite stayed green
// and clang-ssaf-format accepted "params":["x"], writing it back as
// "params":[0] -- a corrupt field become the shape that means "parameter 0
// provably does not escape", which is an annotation on real source.

TEST_F(ParameterEscapeResultReaderTest, EscapeResultRejectsBadElementTypes) {
  static constexpr Case Cases[] = {
      // facts[] and candidates[] are destructured with getAsObject().
      {"fact-not-an-object",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":["x"]}})"},
      {"fact-is-a-number",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[7]}})"},
      {"candidate-not-an-object",
       R"({"ParameterEscapeResult":{"candidates":["x"],"facts":[]}})"},
      {"candidate-is-null",
       R"({"ParameterEscapeResult":{"candidates":[null],"facts":[]}})"},
      // node is read with get(), so "present but not an object" is a path of
      // its own rather than a synonym for absent.
      {"fact-node-not-an-object",
       R"({"ParameterEscapeResult":{"candidates":[],"facts":[{)"
       R"("flows_to":[],"node":"x"}]}})"},
  };
  expectAllRejected("ParameterEscapeResult", Cases);
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsBadElementTypes) {
  static constexpr Case Cases[] = {
      {"non-escaping-not-an-object",
       R"({"NonEscapingParametersResult":{"non_escaping":["x"],)"
       R"("rejected":[]}})"},
      {"rejection-not-an-object",
       R"({"NonEscapingParametersResult":{"non_escaping":[],)"
       R"("rejected":["x"]}})"},
      {"rejection-node-not-an-object",
       R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[{)"
       R"("location":{"column":2,"file":"a.cpp","line":1},"node":"x",)"
       R"("reason":"Asm"}]}})"},
      // params holds bare scalars: no keys to enumerate, so census 1 has
      // nothing to say about it and only these cases reach its type check.
      {"annotation-is-a-string",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":["x"]}]}})"},
      {"annotation-is-null",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[null]}]}})"},
      {"annotation-is-an-array",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[[0]]}]}})"},
      {"annotation-is-a-bool",
       R"({"NonEscapingParametersResult":{"rejected":[],"non_escaping":[{)"
       R"("function":{"@":0},"params":[true]}]}})"},
  };
  expectAllRejected("NonEscapingParametersResult", Cases);
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsDuplicateRejectedNode) {
  // Last-wins here would drop one node's reason, location and blame and exit
  // 0, leaving a report that silently disagrees with the file it came from.
  EXPECT_THAT_ERROR(
      read("NonEscapingParametersResult",
           R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[)"
           R"({"location":{"column":2,"file":"a.cpp","line":1},)"
           R"("node":{"function":{"@":0},"param":0},"reason":"Asm"},)"
           R"({"location":{"column":9,"file":"b.cpp","line":8},)"
           R"("node":{"function":{"@":0},"param":0},"reason":"Throw"}]}})",
           "dup-rejection.json"),
      llvm::Failed());
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsUnknownReason) {
  EXPECT_THAT_ERROR(
      read("NonEscapingParametersResult",
           R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[)"
           R"({"node":{"function":{"@":0},"param":0},"reason":"NotAReason",)"
           R"("location":{"file":"a.cpp","line":1,"column":2}}]}})",
           "bad-reason.json"),
      llvm::Failed());
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsCorruptOptionalDetail) {
  // A detail that is present but not a string is a corrupt record; reading it
  // as an absent one would be the "absent means default" defect.
  EXPECT_THAT_ERROR(
      read("NonEscapingParametersResult",
           R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[)"
           R"({"node":{"function":{"@":0},"param":0},"reason":"Asm",)"
           R"("detail":7,)"
           R"("location":{"file":"a.cpp","line":1,"column":2}}]}})",
           "bad-detail.json"),
      llvm::Failed());
}

TEST_F(ParameterEscapeResultReaderTest, VerdictRejectsCorruptOptionalBlame) {
  EXPECT_THAT_ERROR(
      read("NonEscapingParametersResult",
           R"({"NonEscapingParametersResult":{"non_escaping":[],"rejected":[)"
           R"({"node":{"function":{"@":0},"param":0},"reason":"Asm",)"
           R"("blame":"not-a-node",)"
           R"("location":{"file":"a.cpp","line":1,"column":2}}]}})",
           "bad-blame.json"),
      llvm::Failed());
}

//===----------------------------------------------------------------------===//
// Byte stability of a real analysis run
//===----------------------------------------------------------------------===//

// Determinism is a claim about bytes, so it is measured as bytes: the same
// program is built and analysed twice, from two independently constructed
// LUSummaries, and the two written WPASuite files are compared verbatim. A
// hash-ordered container anywhere between the summaries and the writer would
// show up here.
class NonEscapingParametersDeterminismTest : public JSONFormatTest {
protected:
  NestedBuildNamespace NS{{BuildNamespace(BuildNamespaceKind::LinkUnit, "LU")}};

  /// Builds a link unit exercising every branch of the fixpoint: a clean
  /// parameter, a sink, a chain, a cycle, an unanalyzed callee, a this node,
  /// a return, and a candidate with two escaping targets to choose blame
  /// from.
  static std::unique_ptr<LUSummary> build(const NestedBuildNamespace &NS) {
    auto LU =
        std::make_unique<LUSummary>(llvm::Triple("arm64-apple-macosx"), NS);
    std::map<std::string, EntityId> Ids;
    auto fn = [&](llvm::StringRef Name) {
      auto It = Ids.find(Name.str());
      if (It != Ids.end())
        return It->second;
      EntityId Id =
          getIdTable(*LU).getId(EntityName(("c:@F@" + Name).str(), "", NS));
      getLinkageTable(*LU).insert(
          {Id, EntityLinkage(EntityLinkageType::External)});
      Ids.emplace(Name.str(), Id);
      return Id;
    };
    auto summary = [&](llvm::StringRef Name) -> ParameterEscapeSummary & {
      auto &Slot =
          getData(*LU)[ParameterEscapeSummary::summaryName()][fn(Name)];
      if (!Slot) {
        auto S = std::make_unique<ParameterEscapeSummary>();
        S->IsCandidate = true;
        Slot = std::move(S);
      }
      return static_cast<ParameterEscapeSummary &>(*Slot);
    };
    auto param = [&](llvm::StringRef Name, unsigned I) -> EscapeFact & {
      ParameterEscapeSummary &S = summary(Name);
      S.CandidateParams.insert(I);
      return S.Params[I];
    };
    const SourceLocationRecord Site{"d.cpp", 3, 9};

    param("clean", 0) = EscapeFact();
    param("sunk", 0).OtherSink =
        Sink{EscapeReason::StoreToGlobal, SourceLocationRecord{"d.cpp", 4, 1},
             "global"};
    param("chain", 0).FlowsTo[FlowTarget{fn("sunk"), 0}] = Site;
    param("cycleA", 0).FlowsTo[FlowTarget{fn("cycleB"), 0}] = Site;
    param("cycleB", 0).FlowsTo[FlowTarget{fn("cycleA"), 0}] = Site;
    param("toExternal", 0).FlowsTo[FlowTarget{fn("external"), 0}] = Site;
    param("toThis", 0).FlowsTo[FlowTarget{fn("method"), ThisParamIndex}] = Site;
    summary("method").This = EscapeFact();
    param("ident", 0).ReturnsSelfAt = SourceLocationRecord{"d.cpp", 5, 2};
    param("twoBad", 0).FlowsTo[FlowTarget{fn("sunk"), 0}] = Site;
    param("twoBad", 0).FlowsTo[FlowTarget{fn("external"), 0}] = Site;
    return LU;
  }

  llvm::Expected<std::string> runAndWrite(llvm::StringRef FileName) {
    AnalysisDriver Driver(build(NS));
    llvm::Expected<WPASuite> Suite = Driver.run<NonEscapingParametersResult>();
    if (!Suite)
      return Suite.takeError();
    JSONFormat Format;
    PathString Path = makePath(FileName);
    if (auto Err = Format.writeWPASuite(*Suite, Path))
      return std::move(Err);
    auto Buffer = llvm::MemoryBuffer::getFile(Path, /*IsText=*/true);
    if (!Buffer)
      return llvm::createStringError(Buffer.getError(), "failed to read back");
    return Buffer.get()->getBuffer().str();
  }
};

TEST_F(NonEscapingParametersDeterminismTest, RunsAreByteIdentical) {
  llvm::Expected<std::string> First = runAndWrite("run-1.json");
  ASSERT_THAT_EXPECTED(First, llvm::Succeeded());
  llvm::Expected<std::string> Second = runAndWrite("run-2.json");
  ASSERT_THAT_EXPECTED(Second, llvm::Succeeded());

  // A non-trivial verdict, so the comparison is not vacuously true.
  EXPECT_NE(First->find("\"NonEscapingParametersResult\""), std::string::npos);
  EXPECT_NE(First->find("\"UnanalyzedCallee\""), std::string::npos);
  // "params" appears only inside a non_escaping entry, so its presence means
  // at least one parameter really was annotated.
  EXPECT_NE(First->find("\"params\""), std::string::npos);
  EXPECT_EQ(*First, *Second);
}

} // namespace
} // namespace clang::ssaf
