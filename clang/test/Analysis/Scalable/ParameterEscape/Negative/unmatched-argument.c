// An argument that matches no parameter of a *non-variadic* callee: here an
// unprototyped C declaration. Nothing in the callee's summary can carry the
// flow, so the argument sinks rather than becoming an edge.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} -Wno-deprecated-non-prototype %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 30,
// CHECK-NEXT:       "file": "{{.*}}unmatched-argument.c",
// CHECK-NEXT:       "line": 36
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "UnmatchedArgument"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void legacy();

void escape(int *p) { legacy(p); }

// Runtime half; see store-to-global.cpp for what a driver is for. The
// unprototyped declaration is what stops the argument being matched to a
// parameter; the definition the driver supplies shows there was a parameter
// all along, and that it retains.
// DRIVER: int *leak;
// DRIVER: void legacy(int *q) { leak = q; }
// DRIVER: int main() {
// DRIVER:   { int local = 1; escape(&local); }
// DRIVER:   return *leak;
// DRIVER: }
