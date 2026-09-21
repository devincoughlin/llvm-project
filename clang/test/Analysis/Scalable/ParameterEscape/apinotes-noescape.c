// A `noescape` that exists only in API Notes is still trusted.
//
// `trusted` and `untrusted` are declared identically in the header; the only
// difference between them is a NoEscape entry in the sidecar .apinotes file,
// which gives parameter 0 a NoEscapeAttr in the AST without the attribute
// appearing in the header's bytes. The caller of the annotated one is
// annotatable and the caller of the other is not.
//
// The second run is the control: with -fno-apinotes-modules the attribute is
// nowhere, and the two callers become indistinguishable. Without it this file
// could pass with the API Notes never being read at all.

// DEFINE: %{mod} = -fmodules -fimplicit-module-maps -fapinotes-modules \
// DEFINE:   -I%S/Inputs/apinotes
// DEFINE: %{extract} = --ssaf-extract-summaries=ParameterEscape \
// DEFINE:   --ssaf-compilation-unit-id=cu

// RUN: rm -rf %t && mkdir -p %t

// =============================================================================
// 1. API Notes on: `trusted`'s caller is annotated.
// =============================================================================

// RUN: %clang -fsyntax-only %s %{mod} -fmodules-cache-path=%t/mc-on \
// RUN:   %{extract} --ssaf-tu-summary-file=%t/on.tu.json
// RUN: %{pe-link} %t/on.tu.json -o %t/on.lu.json
// RUN: %{pe-analyze} %t/on.lu.json -o %t/on.wpa.json
// `trusted` has no node at all: a parameter already known to be `noescape`
// records no edge, which is the mechanism by which the declaration is
// believed. That is asserted with --implicit-check-not, which is
// position-independent -- a CHECK-NOT would only guard the region between its
// neighbours, and `trusted` sorts *before* them in the id table.
// RUN: FileCheck %s --check-prefix=ON --input-file=%t/on.wpa.json \
// RUN:   --implicit-check-not='"usr": "c:@F@trusted"'

// ON:      "id": 0,
// ON:        "usr": "c:@F@untrusted"
// ON:      "id": 1,
// ON:        "usr": "c:@F@via_trusted"
// ON:      "id": 2,
// ON:        "usr": "c:@F@via_untrusted"

// ON:      "non_escaping": [
// ON-NEXT:   {
// ON-NEXT:     "function": {
// ON-NEXT:       "@": 1
// ON-NEXT:     },
// ON-NEXT:     "params": [
// ON-NEXT:       0
// ON-NEXT:     ]
// ON-NEXT:   }
// ON-NEXT: ],
// ON-NEXT: "rejected": [
// ON-NEXT:   {
// ON-NEXT:     "blame": {
// ON-NEXT:       "function": {
// ON-NEXT:         "@": 0
// ON-NEXT:       },
// ON-NEXT:       "param": 0
// ON-NEXT:     },
// ON-NEXT:     "detail": "",
// ON-NEXT:     "location": {
// ON-NEXT:       "column": 40,
// ON-NEXT:       "file": "{{.*}}apinotes-noescape.c",
// ON-NEXT:       "line": 97
// ON-NEXT:     },
// ON-NEXT:     "node": {
// ON-NEXT:       "function": {
// ON-NEXT:         "@": 2
// ON-NEXT:       },
// ON-NEXT:       "param": 0
// ON-NEXT:     },
// ON-NEXT:     "reason": "UnanalyzedCallee"
// ON-NEXT:   }
// ON-NEXT: ]

// =============================================================================
// 2. API Notes off: neither caller is annotated, and `trusted` now has a node.
// =============================================================================

// RUN: %clang -fsyntax-only %s %{mod} -fmodules-cache-path=%t/mc-off \
// RUN:   -fno-apinotes-modules \
// RUN:   %{extract} --ssaf-tu-summary-file=%t/off.tu.json
// RUN: %{pe-link} %t/off.tu.json -o %t/off.lu.json
// RUN: %{pe-analyze} %t/off.lu.json -o %t/off.wpa.json
// RUN: FileCheck %s --check-prefix=OFF --input-file=%t/off.wpa.json

// OFF:      "usr": "c:@F@trusted"
// OFF:      "non_escaping": [],
// OFF-NEXT: "rejected": [
// OFF:        "reason": "UnanalyzedCallee"
// OFF:        "reason": "UnanalyzedCallee"

#include "NoescapeLib.h"

void via_trusted(int *p) { trusted(p, 1); }
void via_untrusted(int *p) { untrusted(p, 1); }
