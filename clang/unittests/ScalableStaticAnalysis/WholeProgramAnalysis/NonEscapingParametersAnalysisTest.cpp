//===- NonEscapingParametersAnalysisTest.cpp ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Tests for the whole-program escape fixpoint. Summaries are hand-built into
// an LUSummary and run through AnalysisDriver, so these exercise
// ParameterEscapeAnalysis and NonEscapingParametersAnalysis together.
//
// The cases are organised around the fixpoint's shape: how a node enters the
// worklist (a sink, a missing callee, back-propagation), what the initial
// state is (nothing escapes until positive evidence says so, which is why a
// cycle with no sink annotates), and what happens at the edges of the node
// universe (a flow target with no summary, a this node, a candidate with no
// fact).
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscapeAnalysis.h"

#include "../TestFixture.h"
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/EntityLinker/LUSummary.h"
#include "clang/ScalableStaticAnalysis/Core/Model/BuildNamespace.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityId.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityLinkage.h"
#include "clang/ScalableStaticAnalysis/Core/Model/EntityName.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/AnalysisDriver.h"
#include "clang/ScalableStaticAnalysis/Core/WholeProgramAnalysis/WPASuite.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace clang::ssaf {
namespace {

// The call site every FlowsTo edge below is recorded at. Distinct from the
// sink and return locations so a rejection that quotes the wrong one is
// visible.
const SourceLocationRecord CallSite{"call.cpp", 7, 3};
const SourceLocationRecord SinkSite{"sink.cpp", 11, 5};
const SourceLocationRecord ReturnSite{"ret.cpp", 13, 2};

class NonEscapingParametersAnalysisTest : public TestFixture {
protected:
  NestedBuildNamespace NS{{BuildNamespace(BuildNamespaceKind::LinkUnit, "LU")}};
  std::unique_ptr<LUSummary> LU =
      std::make_unique<LUSummary>(llvm::Triple("arm64-apple-macosx"), NS);
  std::map<std::string, EntityId> Ids;

  /// Interns \p Name as an external entity, assigning EntityIds in first-use
  /// order. Node ordering follows EntityId, so the order these are first
  /// mentioned is what the determinism cases below vary.
  EntityId fn(llvm::StringRef Name) {
    auto It = Ids.find(Name.str());
    if (It != Ids.end())
      return It->second;
    EntityId Id =
        getIdTable(*LU).getId(EntityName(("c:@F@" + Name).str(), "", NS));
    getLinkageTable(*LU).insert(
        {Id, EntityLinkage(EntityLinkageType::External)});
    Ids.emplace(Name.str(), Id);
    return Id;
  }

  ParameterEscapeSummary &summary(llvm::StringRef Name) {
    auto &Slot = getData(*LU)[ParameterEscapeSummary::summaryName()][fn(Name)];
    if (!Slot) {
      auto S = std::make_unique<ParameterEscapeSummary>();
      S->IsCandidate = true;
      Slot = std::move(S);
    }
    return static_cast<ParameterEscapeSummary &>(*Slot);
  }

  /// Declares parameter \p I of \p Fn and returns its fact slot. \p
  /// CandidateType controls whether the parameter is of annotatable type.
  EscapeFact &param(llvm::StringRef Fn, unsigned I, bool CandidateType = true) {
    ParameterEscapeSummary &S = summary(Fn);
    if (CandidateType)
      S.CandidateParams.insert(I);
    return S.Params[I];
  }

  static EscapeFact flows(std::initializer_list<FlowTarget> Targets) {
    EscapeFact F;
    for (const FlowTarget &T : Targets)
      F.FlowsTo[T] = CallSite;
    return F;
  }

  static EscapeFact sinks(EscapeReason R, std::string Detail = "") {
    EscapeFact F;
    F.OtherSink = Sink{R, SinkSite, std::move(Detail)};
    return F;
  }

  /// Runs the driver and hands back a copy of the fixpoint's result. Consumes
  /// LU, so each test calls this once per link unit it builds.
  NonEscapingParametersResult run() {
    AnalysisDriver Driver(std::move(LU));
    llvm::Expected<WPASuite> Suite = Driver.run<NonEscapingParametersResult>();
    EXPECT_THAT_EXPECTED(Suite, llvm::Succeeded());
    if (!Suite) {
      llvm::consumeError(Suite.takeError());
      return {};
    }
    llvm::Expected<const NonEscapingParametersResult &> R =
        Suite->get<NonEscapingParametersResult>();
    EXPECT_THAT_EXPECTED(R, llvm::Succeeded());
    if (!R) {
      llvm::consumeError(R.takeError());
      return {};
    }
    return *R;
  }

  /// Starts a fresh link unit, so a test can build the same program twice with
  /// a different EntityId assignment.
  void reset() {
    LU = std::make_unique<LUSummary>(llvm::Triple("arm64-apple-macosx"), NS);
    Ids.clear();
  }

  /// \returns the rejection recorded for \p N, or null. Returning null rather
  /// than calling map::at keeps a missing rejection a test failure instead of
  /// an abort that takes the whole binary with it.
  static const NonEscapingParametersResult::Rejection *
  rejection(const NonEscapingParametersResult &R, const Node &N) {
    auto It = R.Rejected.find(N);
    return It == R.Rejected.end() ? nullptr : &It->second;
  }

  static bool annotated(const NonEscapingParametersResult &R, EntityId F,
                        unsigned I) {
    auto It = R.NonEscaping.find(F);
    return It != R.NonEscaping.end() && It->second.count(I) == 1;
  }
};

TEST_F(NonEscapingParametersAnalysisTest, CleanParameterIsAnnotated) {
  param("f", 0) = EscapeFact();

  NonEscapingParametersResult R = run();

  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_TRUE(R.Rejected.empty());
}

TEST_F(NonEscapingParametersAnalysisTest, SinkRejectsWithReasonAndLocation) {
  param("f", 0) = sinks(EscapeReason::StoreToGlobal, "g");

  NonEscapingParametersResult R = run();

  EXPECT_FALSE(annotated(R, fn("f"), 0));
  const auto *Rej = rejection(R, Node{fn("f"), 0});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::StoreToGlobal);
  EXPECT_EQ(Rej->Location, SinkSite);
  EXPECT_EQ(Rej->Detail, "g");
  EXPECT_FALSE(Rej->Blame.has_value());
}

TEST_F(NonEscapingParametersAnalysisTest, ChainPropagatesWithBlameAndCallSite) {
  param("f", 0) = flows({FlowTarget{fn("g"), 0}});
  param("g", 0) = flows({FlowTarget{fn("h"), 0}});
  param("h", 0) = sinks(EscapeReason::StoreToField);

  NonEscapingParametersResult R = run();

  EXPECT_FALSE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("g"), 0));
  EXPECT_FALSE(annotated(R, fn("h"), 0));

  // f is blamed on g, one hop, at the call site -- not on h, and not at h's
  // sink location.
  const auto *Rej = rejection(R, Node{fn("f"), 0});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::EscapesViaCallee);
  ASSERT_TRUE(Rej->Blame.has_value());
  EXPECT_EQ(*Rej->Blame, (Node{fn("g"), 0}));
  EXPECT_EQ(Rej->Location, CallSite);

  // h keeps its own reason rather than inheriting one.
  const auto *Sunk = rejection(R, Node{fn("h"), 0});
  ASSERT_NE(Sunk, nullptr);
  EXPECT_EQ(Sunk->Reason, EscapeReason::StoreToField);
}

TEST_F(NonEscapingParametersAnalysisTest, CycleWithoutSinkIsNonEscaping) {
  param("f", 0) = flows({FlowTarget{fn("g"), 0}});
  param("g", 0) = flows({FlowTarget{fn("f"), 0}});

  NonEscapingParametersResult R = run();

  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_TRUE(annotated(R, fn("g"), 0));
  EXPECT_TRUE(R.Rejected.empty());
}

TEST_F(NonEscapingParametersAnalysisTest, CycleWithSinkEscapes) {
  param("f", 0) = flows({FlowTarget{fn("g"), 0}});
  param("g", 0) = flows({FlowTarget{fn("f"), 0}});
  param("g", 0).OtherSink = Sink{EscapeReason::Throw, SinkSite, ""};

  NonEscapingParametersResult R = run();

  EXPECT_FALSE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("g"), 0));
  const auto *F = rejection(R, Node{fn("f"), 0});
  const auto *G = rejection(R, Node{fn("g"), 0});
  ASSERT_NE(F, nullptr);
  ASSERT_NE(G, nullptr);
  EXPECT_EQ(F->Reason, EscapeReason::EscapesViaCallee);
  EXPECT_EQ(G->Reason, EscapeReason::Throw);
}

// The long-chain version of the above: the sink is four hops away and reached
// only through the cycle, so a fixpoint that stopped propagating at the first
// repeated node would annotate every one of these.
TEST_F(NonEscapingParametersAnalysisTest, SinkBehindCyclePropagatesToAll) {
  param("a", 0) = flows({FlowTarget{fn("b"), 0}});
  param("b", 0) = flows({FlowTarget{fn("c"), 0}});
  param("c", 0) = flows({FlowTarget{fn("b"), 0}, FlowTarget{fn("d"), 0}});
  param("d", 0) = sinks(EscapeReason::Asm);

  NonEscapingParametersResult R = run();

  for (llvm::StringRef Name : {"a", "b", "c", "d"})
    EXPECT_FALSE(annotated(R, fn(Name), 0)) << Name;
}

TEST_F(NonEscapingParametersAnalysisTest, MissingNodeEscapes) {
  param("f", 0) = flows({FlowTarget{fn("external"), 0}});

  NonEscapingParametersResult R = run();

  EXPECT_FALSE(annotated(R, fn("f"), 0));
  const auto *Rej = rejection(R, Node{fn("f"), 0});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::UnanalyzedCallee);
  EXPECT_EQ(Rej->Location, CallSite);
  ASSERT_TRUE(Rej->Blame.has_value());
  EXPECT_EQ(*Rej->Blame, (Node{fn("external"), 0}));
}

// A callee that is summarized, but not at the index the edge names. "Absent
// from Facts" is per node, not per function, so this is a distinct entry into
// the unanalyzed seed from MissingNodeEscapes.
TEST_F(NonEscapingParametersAnalysisTest, MissingParameterIndexEscapes) {
  param("f", 0) = flows({FlowTarget{fn("g"), 1}});
  param("g", 0) = EscapeFact();

  NonEscapingParametersResult R = run();

  EXPECT_FALSE(annotated(R, fn("f"), 0));
  const auto *Rej = rejection(R, Node{fn("f"), 0});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::UnanalyzedCallee);
  // g's own parameter 0 is still clean.
  EXPECT_TRUE(annotated(R, fn("g"), 0));
}

// An unanalyzed callee two hops away still reaches the caller: the seed has to
// be propagated, not just reported at the edge that names it.
TEST_F(NonEscapingParametersAnalysisTest, MissingNodePropagatesThroughChain) {
  param("f", 0) = flows({FlowTarget{fn("g"), 0}});
  param("g", 0) = flows({FlowTarget{fn("external"), 0}});

  NonEscapingParametersResult R = run();

  EXPECT_FALSE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("g"), 0));
  // f is blamed on g, which is analyzed, so the reason is EscapesViaCallee;
  // only g names the unanalyzed node.
  const auto *F = rejection(R, Node{fn("f"), 0});
  const auto *G = rejection(R, Node{fn("g"), 0});
  ASSERT_NE(F, nullptr);
  ASSERT_NE(G, nullptr);
  EXPECT_EQ(F->Reason, EscapeReason::EscapesViaCallee);
  EXPECT_EQ(G->Reason, EscapeReason::UnanalyzedCallee);
}

TEST_F(NonEscapingParametersAnalysisTest, ReturnRejectsSelfButNotCallers) {
  param("f", 0) = flows({FlowTarget{fn("id"), 0}});
  param("id", 0).ReturnsSelfAt = ReturnSite;

  NonEscapingParametersResult R = run();

  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("id"), 0));
  const auto *Rej = rejection(R, Node{fn("id"), 0});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::Return);
  EXPECT_EQ(Rej->Location, ReturnSite);
  EXPECT_FALSE(Rej->Blame.has_value());
}

// A sink and a return on the same parameter: the sink wins, because it is the
// reason that also explains the callers.
TEST_F(NonEscapingParametersAnalysisTest, SinkOutranksReturn) {
  param("f", 0) = sinks(EscapeReason::AddressTaken);
  param("f", 0).ReturnsSelfAt = ReturnSite;

  NonEscapingParametersResult R = run();

  const auto *Rej = rejection(R, Node{fn("f"), 0});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::AddressTaken);
  EXPECT_EQ(Rej->Location, SinkSite);
}

TEST_F(NonEscapingParametersAnalysisTest,
       ThisNodesParticipateButAreNeverAnnotated) {
  param("f", 0) = flows({FlowTarget{fn("m"), ThisParamIndex}});
  summary("m").This = sinks(EscapeReason::StoreToGlobal);

  NonEscapingParametersResult R = run();

  EXPECT_FALSE(annotated(R, fn("f"), 0));
  const auto *Rej = rejection(R, Node{fn("f"), 0});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::EscapesViaCallee);
  ASSERT_TRUE(Rej->Blame.has_value());
  EXPECT_EQ(*Rej->Blame, (Node{fn("m"), ThisParamIndex}));
  // The this node is neither annotated nor reported as a rejected candidate.
  EXPECT_EQ(R.Rejected.count(Node{fn("m"), ThisParamIndex}), 0u);
  EXPECT_EQ(R.NonEscaping.count(fn("m")), 0u);
}

// A clean this node is an ordinary non-escaping flow target for its callers,
// but still never annotated itself.
TEST_F(NonEscapingParametersAnalysisTest, CleanThisNodeDoesNotBlockCallers) {
  param("f", 0) = flows({FlowTarget{fn("m"), ThisParamIndex}});
  summary("m").This = EscapeFact();

  NonEscapingParametersResult R = run();

  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_EQ(R.NonEscaping.count(fn("m")), 0u);
  EXPECT_TRUE(R.Rejected.empty());
}

TEST_F(NonEscapingParametersAnalysisTest, OnlyCandidatesAreReported) {
  // A non-candidate function: its parameters are neither annotated nor
  // rejected, even though the facts are clean.
  param("f", 0) = EscapeFact();
  summary("f").IsCandidate = false;
  // A candidate function with a parameter of non-candidate type.
  param("g", 0, /*CandidateType=*/false) = EscapeFact();

  NonEscapingParametersResult R = run();

  EXPECT_TRUE(R.NonEscaping.empty());
  EXPECT_TRUE(R.Rejected.empty());
}

// A candidate parameter the extractor declared annotatable but recorded no
// fact for. Absence must not read as "does not escape".
TEST_F(NonEscapingParametersAnalysisTest, CandidateWithNoFactIsRejected) {
  param("f", 0) = EscapeFact();
  summary("f").CandidateParams.insert(1); // declared, never recorded

  NonEscapingParametersResult R = run();

  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("f"), 1));
  const auto *Rej = rejection(R, Node{fn("f"), 1});
  ASSERT_NE(Rej, nullptr);
  EXPECT_EQ(Rej->Reason, EscapeReason::UnrecognizedUse);
  EXPECT_FALSE(Rej->Blame.has_value());
}

// Two parameters of the same function decided independently.
TEST_F(NonEscapingParametersAnalysisTest,
       ParametersOfOneFunctionAreIndependent) {
  param("f", 0) = EscapeFact();
  param("f", 1) = sinks(EscapeReason::StoreToGlobal);
  param("f", 2) = EscapeFact();

  NonEscapingParametersResult R = run();

  EXPECT_TRUE(annotated(R, fn("f"), 0));
  EXPECT_FALSE(annotated(R, fn("f"), 1));
  EXPECT_TRUE(annotated(R, fn("f"), 2));
  EXPECT_EQ(R.Rejected.size(), 1u);
}

// Blame is the smallest escaping flow target in Node order, not the first one
// inserted. The two halves build the same program with the entity names first
// mentioned in opposite orders, so "b" and "c" swap EntityIds: a blame that
// followed insertion order would name the same function twice, and a blame
// that followed node order names a different one each time.
TEST_F(NonEscapingParametersAnalysisTest, BlameIsTheSmallestEscapingTarget) {
  param("a", 0) = flows({FlowTarget{fn("b"), 0}, FlowTarget{fn("c"), 0}});
  param("b", 0) = sinks(EscapeReason::Throw);
  param("c", 0) = sinks(EscapeReason::Asm);
  ASSERT_TRUE(fn("b") < fn("c"));

  NonEscapingParametersResult First = run();
  const auto *FirstRej = rejection(First, Node{fn("a"), 0});
  ASSERT_NE(FirstRej, nullptr);
  ASSERT_TRUE(FirstRej->Blame.has_value());
  EXPECT_EQ(*FirstRej->Blame, (Node{fn("b"), 0}));

  reset();

  param("a", 0) = flows({FlowTarget{fn("c"), 0}, FlowTarget{fn("b"), 0}});
  param("b", 0) = sinks(EscapeReason::Throw);
  param("c", 0) = sinks(EscapeReason::Asm);
  ASSERT_TRUE(fn("c") < fn("b"));

  NonEscapingParametersResult Second = run();
  const auto *SecondRej = rejection(Second, Node{fn("a"), 0});
  ASSERT_NE(SecondRej, nullptr);
  ASSERT_TRUE(SecondRej->Blame.has_value());
  EXPECT_EQ(*SecondRej->Blame, (Node{fn("c"), 0}));
}

} // namespace
} // namespace clang::ssaf
