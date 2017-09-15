//===- CoverageSummaryInfo.cpp - Coverage summary for function/file -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// These structures are used to represent code coverage metrics
// for functions/files.
//
//===----------------------------------------------------------------------===//

#include "CoverageSummaryInfo.h"

using namespace llvm;
using namespace coverage;

FunctionCoverageSummary
FunctionCoverageSummary::get(const coverage::FunctionRecord &Function) {
  // Compute the region coverage.
  size_t NumCodeRegions = 0, CoveredRegions = 0;
  for (auto &CR : Function.CountedRegions) {
    if (CR.Kind != CounterMappingRegion::CodeRegion)
      continue;
    ++NumCodeRegions;
    if (CR.ExecutionCount != 0)
      ++CoveredRegions;
  }

  // TODO: This logic is incorrect and needs to be removed (PR34615). We need
  // to use the segment builder to get accurate line execution counts.
  //
  // Compute the line coverage
  size_t NumLines = 0, CoveredLines = 0;
  for (unsigned FileID = 0, E = Function.Filenames.size(); FileID < E;
       ++FileID) {
    // Find the line start and end of the function's source code
    // in that particular file
    unsigned LineStart = std::numeric_limits<unsigned>::max();
    unsigned LineEnd = 0;
    for (auto &CR : Function.CountedRegions) {
      if (CR.FileID != FileID)
        continue;
      LineStart = std::min(LineStart, CR.LineStart);
      LineEnd = std::max(LineEnd, CR.LineEnd);
    }
    assert(LineStart <= LineEnd && "Function contains spurious file");
    unsigned LineCount = LineEnd - LineStart + 1;

    // Get counters
    llvm::SmallVector<uint64_t, 16> ExecutionCounts;
    ExecutionCounts.resize(LineCount, 0);
    unsigned LinesNotSkipped = LineCount;
    for (auto &CR : Function.CountedRegions) {
      if (CR.FileID != FileID)
        continue;
      // Ignore the lines that were skipped by the preprocessor.
      auto ExecutionCount = CR.ExecutionCount;
      if (CR.Kind == CounterMappingRegion::SkippedRegion) {
        unsigned SkippedLines = CR.LineEnd - CR.LineStart + 1;
        assert((SkippedLines <= LinesNotSkipped) &&
               "Skipped region larger than file containing it");
        LinesNotSkipped -= SkippedLines;
        ExecutionCount = 1;
      }
      for (unsigned I = CR.LineStart; I <= CR.LineEnd; ++I)
        ExecutionCounts[I - LineStart] = ExecutionCount;
    }
    unsigned UncoveredLines = std::min(
        (unsigned)std::count(ExecutionCounts.begin(), ExecutionCounts.end(), 0),
        (unsigned)LinesNotSkipped);
    CoveredLines += LinesNotSkipped - UncoveredLines;
    NumLines += LinesNotSkipped;
  }
  return FunctionCoverageSummary(
      Function.Name, Function.ExecutionCount,
      RegionCoverageInfo(CoveredRegions, NumCodeRegions),
      LineCoverageInfo(CoveredLines, NumLines));
}

FunctionCoverageSummary
FunctionCoverageSummary::get(const InstantiationGroup &Group,
                             ArrayRef<FunctionCoverageSummary> Summaries) {
  std::string Name;
  if (Group.hasName()) {
    Name = Group.getName();
  } else {
    llvm::raw_string_ostream OS(Name);
    OS << "Definition at line " << Group.getLine() << ", column "
       << Group.getColumn();
  }

  FunctionCoverageSummary Summary(Name);
  Summary.ExecutionCount = Group.getTotalExecutionCount();
  Summary.RegionCoverage = Summaries[0].RegionCoverage;
  Summary.LineCoverage = Summaries[0].LineCoverage;
  for (const auto &FCS : Summaries.drop_front()) {
    Summary.RegionCoverage.merge(FCS.RegionCoverage);
    Summary.LineCoverage.merge(FCS.LineCoverage);
  }
  return Summary;
}
