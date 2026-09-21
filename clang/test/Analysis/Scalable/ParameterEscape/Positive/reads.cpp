// The harness is not vacuous: a parameter that is only read through is
// annotated. Every benign row the classifier has -- a dereference, a
// comparison, a cast to bool, an increment through the pointee, a discarded
// value, an unevaluated operand -- is exercised here, so weakening any of them
// into a sink turns this file red.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@reads#*I#S0_#"
// CHECK:      "non_escaping": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "function": {
// CHECK-NEXT:       "@": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "params": [
// CHECK-NEXT:       0,
// CHECK-NEXT:       1
// CHECK-NEXT:     ]
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK-NEXT: "rejected": []

int reads(int *p, int *q) {
  if (!p)
    return 0;
  if (p == q)
    return 1;
  (void)sizeof(*p);
  (*p)++;
  (void)p;
  return *p;
}
