// Capture by a lambda stores the pointer into the closure object, whose
// lifetime is not the callee frame's in general.

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
// CHECK:        "usr": "c:lambda-capture.cpp@{{[0-9]+}}@{{[0-9]+}}@F@escape#*I#@Sa@F@operator()#1"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 16,
// CHECK-NEXT:       "file": "{{.*}}lambda-capture.cpp",
// CHECK-NEXT:       "line": 38
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "Capture"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void run(void (*)());

void escape(int *p) {
  auto keep = [p] { (void)*p; };
  keep();
}
