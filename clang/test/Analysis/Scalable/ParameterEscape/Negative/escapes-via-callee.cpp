// The flow edge resolves to a callee that *is* analyzed and that escapes.
// Both nodes are rejected, and the caller's blame names the callee's node --
// which is what separates this reason from UnanalyzedCallee.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#*I#"
// CHECK:      "id": 1,
// CHECK:        "usr": "c:@F@inner#*I#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "blame": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 1
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 29,
// CHECK-NEXT:       "file": "{{.*}}escapes-via-callee.cpp",
// CHECK-NEXT:       "line": 61
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "EscapesViaCallee"
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "g",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 26,
// CHECK-NEXT:       "file": "{{.*}}escapes-via-callee.cpp",
// CHECK-NEXT:       "line": 59
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 1
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "StoreToGlobal"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

int *g;

void inner(int *q) { g = q; }

void escape(int *p) { inner(p); }

// Runtime half; see store-to-global.cpp for what the driver is for. The
// dereference reads what `inner` stored, so the report is evidence the escape
// travels the whole caller-to-callee edge this file is about.
// DRIVER: int main() {
// DRIVER:   int *k;
// DRIVER:   { int local = 1; escape(&local); k = g; }
// DRIVER:   return *k;
// DRIVER: }
