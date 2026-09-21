// A `cleanup` attribute runs a function over the address of the local it is
// attached to. Initializing that local from the parameter therefore hands the
// parameter to a callee the analysis cannot see through.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 63,
// CHECK-NEXT:       "file": "{{.*}}cleanup-attribute.c",
// CHECK-NEXT:       "line": 36
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "AddressTaken"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void dtor(int **);

void escape(int *p) { int *q __attribute__((cleanup(dtor))) = p; }

// Runtime half; see store-to-global.cpp for what a driver is for. The cleanup
// function runs at the end of `escape` and is handed the address of the local
// holding the parameter; the driver defines it to retain what it finds there.
// DRIVER: int *leak;
// DRIVER: void dtor(int **pq) { leak = *pq; }
// DRIVER: int main() {
// DRIVER:   { int local = 1; escape(&local); }
// DRIVER:   return *leak;
// DRIVER: }
