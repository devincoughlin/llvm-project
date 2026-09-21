// Decomposing the object a parameter points at does not leak the parameter.
//
// This replaces the `view-accessor.cpp` the issue asked for: since #12 took
// tracked views out, a view parameter carries no fact and is not a candidate,
// so a view accessor can no longer show that the harness accepts anything.
// A structured binding can: `p` is a candidate, and the BindingDecl row in the
// classifier is what decides it.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@destructure#*$@S@Pair#"
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

struct Pair {
  int *first;
  int second;
};

int destructure(Pair *p) {
  auto &[a, b] = *p;
  return *a + b;
}
