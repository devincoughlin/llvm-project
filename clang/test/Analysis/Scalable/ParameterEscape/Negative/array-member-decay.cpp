// Storing into an element of an array member. The subscript decays the array
// to a pointer, so this lands on StoreThroughPointer rather than on
// StoreToField -- measured, not assumed.

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
// CHECK-NEXT:       "column": 38,
// CHECK-NEXT:       "file": "{{.*}}array-member-decay.cpp",
// CHECK-NEXT:       "line": 40
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

struct Box {
  int *slots[4];
};

Box box;

void escape(int *p) { box.slots[1] = p; }

// Runtime half; see store-to-global.cpp for what the driver is for. The read
// comes back out of the same array element the store went into.
// DRIVER: int main() {
// DRIVER:   int *k;
// DRIVER:   { int local = 1; escape(&local); k = box.slots[1]; }
// DRIVER:   return *k;
// DRIVER: }
