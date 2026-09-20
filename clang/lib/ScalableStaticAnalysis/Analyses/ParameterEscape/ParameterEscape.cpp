//===- ParameterEscape.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"

#include <cassert>
#include <cstddef>
#include <iterator>

using namespace clang;
using namespace ssaf;

/// Serialized names, indexed by EscapeReason. Generated from the same list as
/// the enumerators, so a name can never be attached to the wrong reason.
static constexpr llvm::StringLiteral ReasonNames[] = {
#define REASON(NAME) #NAME,
#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/EscapeReasons.def"
};

// Checks the "UnrecognizedUse must stay last" invariant that callers iterating
// the range [0, UnrecognizedUse] depend on.
static_assert(std::size(ReasonNames) ==
                  static_cast<size_t>(EscapeReason::UnrecognizedUse) + 1,
              "UnrecognizedUse must be the last EscapeReason");

llvm::StringRef clang::ssaf::escapeReasonName(EscapeReason R) {
  assert(static_cast<size_t>(R) < std::size(ReasonNames) &&
         "EscapeReason out of range");
  return ReasonNames[static_cast<size_t>(R)];
}

std::optional<EscapeReason>
clang::ssaf::parseEscapeReason(llvm::StringRef Name) {
  for (size_t I = 0; I < std::size(ReasonNames); ++I)
    if (ReasonNames[I] == Name)
      return static_cast<EscapeReason>(I);
  return std::nullopt;
}
