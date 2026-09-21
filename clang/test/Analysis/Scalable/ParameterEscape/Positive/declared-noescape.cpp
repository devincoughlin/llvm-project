// A parameter handed to an *external* declaration that is already spelled
// `__attribute__((noescape))`. The declaration is trusted, so the edge is not
// an UnanalyzedCallee: compare Negative/unanalyzed-callee.cpp, which is the
// same shape with the attribute removed.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#*I#"
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

void external(__attribute__((noescape)) int *q);

void escape(int *p) { external(p); }
