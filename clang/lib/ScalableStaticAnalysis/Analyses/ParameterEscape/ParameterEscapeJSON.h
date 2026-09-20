//===- ParameterEscapeJSON.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// JSON encoding of the ParameterEscape leaf types, shared between the
// per-entity summary format and the whole-program result formats.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEJSON_H
#define LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEJSON_H

#include "clang/ScalableStaticAnalysis/Analyses/ParameterEscape/ParameterEscape.h"
#include "clang/ScalableStaticAnalysis/Core/Serialization/JSONFormat.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace clang::ssaf {

llvm::json::Object sourceLocationRecordToJSON(const SourceLocationRecord &R);

llvm::Expected<SourceLocationRecord>
sourceLocationRecordFromJSON(const llvm::json::Object &O);

llvm::json::Object escapeFactToJSON(const EscapeFact &F,
                                    JSONFormat::EntityIdToJSONFn IdToJSON);

llvm::Expected<EscapeFact>
escapeFactFromJSON(const llvm::json::Object &O,
                   JSONFormat::EntityIdFromJSONFn IdFromJSON);

} // namespace clang::ssaf

#endif // LLVM_CLANG_LIB_SCALABLESTATICANALYSIS_ANALYSES_PARAMETERESCAPE_PARAMETERESCAPEJSON_H
