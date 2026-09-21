// A parameter that flows only into the same parameter of the same function.
// The fixpoint must reach a least solution here rather than blaming the edge
// on an un-annotatable callee: the cycle carries no sink, so it is clean.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@recurse#*I#I#"
// CHECK:      "non_escaping": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "function": {
// CHECK-NEXT:       "@": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "params": [
// CHECK-NEXT:       0
// CHECK-NEXT:     ]
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK-NEXT: "rejected": []

void recurse(int *p, int n) {
  if (n > 0)
    recurse(p, n - 1);
}
