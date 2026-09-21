// The pointer value is converted to something that is no longer a pointer, so
// the analysis loses track of it. The cast kind rides along in `detail`.

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
// CHECK-NEXT:     "detail": "PointerToIntegral",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 36,
// CHECK-NEXT:       "file": "{{.*}}cast-to-non-pointer.cpp",
// CHECK-NEXT:       "line": 33
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "CastToNonPointer"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

long escape(int *p) { return (long)p; }

// Runtime half; see store-to-global.cpp for what a driver is for. The integer
// the cast produces is carried out of the scope and cast back, which is
// exactly why an integer round trip has to be treated as an escape.
// DRIVER: long leak;
// DRIVER: int main() {
// DRIVER:   { int local = 1; leak = escape(&local); }
// DRIVER:   return *(int *)leak;
// DRIVER: }
