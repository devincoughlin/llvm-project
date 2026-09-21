// The flow edge resolves to a callee with no summary in the link unit.
// Absence is never read as "does not escape": the blame names the callee
// parameter the edge pointed at.

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
// CHECK:        "usr": "c:@F@external#*I#"
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
// CHECK-NEXT:       "column": 32,
// CHECK-NEXT:       "file": "{{.*}}unanalyzed-callee.cpp",
// CHECK-NEXT:       "line": 44
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "UnanalyzedCallee"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void external(int *);

void escape(int *p) { external(p); }
