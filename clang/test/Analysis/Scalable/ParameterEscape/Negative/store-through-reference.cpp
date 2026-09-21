// Assignment through a reference. A reference is a pointer to the analysis, so
// this lands on StoreThroughPointer and names the reference in `detail` --
// which is the measured behaviour, not a separate StoreThroughReference reason.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#*I#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "out",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 29,
// CHECK-NEXT:       "file": "{{.*}}store-through-reference.cpp",
// CHECK-NEXT:       "line": 37
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "StoreThroughPointer"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

int *backing;
int *&out = backing;

void escape(int *p) { out = p; }
