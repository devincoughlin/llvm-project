// The pointer is handed to a constructor running in heap storage, whose
// lifetime the callee's frame does not bound.

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
// CHECK-NEXT:       "column": 40,
// CHECK-NEXT:       "file": "{{.*}}heap-allocation.cpp",
// CHECK-NEXT:       "line": 37
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "HeapAllocation"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

struct Node {
  Node(int *);
};

Node *escape(int *p) { return new Node(p); }

// Runtime half; see store-to-global.cpp for what a driver is for. `Node`'s
// constructor is left bodiless by this file; the driver defines it so that the
// heap object really does retain the pointer it was handed.
// DRIVER: int *leak;
// DRIVER: Node::Node(int *p) { leak = p; }
// DRIVER: int main() {
// DRIVER:   { int local = 1; (void)escape(&local); }
// DRIVER:   return *leak;
// DRIVER: }
