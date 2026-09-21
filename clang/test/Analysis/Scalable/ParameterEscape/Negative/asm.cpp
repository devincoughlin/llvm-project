// Inline assembly is opaque: nothing can say what the operand is used for.

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
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 45,
// CHECK-NEXT:       "file": "{{.*}}asm.cpp",
// CHECK-NEXT:       "line": 32
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "Asm"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void escape(int *p) { asm volatile("" ::"r"(p)); }

// No runtime driver. The asm template is empty and stores nothing, so the
// operand is consumed and never read back: there is no dereference of a dead
// object for AddressSanitizer to trap on. The refusal is because the analysis
// cannot see inside an asm block at all, not because this particular asm
// retains anything.
// NO-DRIVER: an empty asm template retains nothing to read back
