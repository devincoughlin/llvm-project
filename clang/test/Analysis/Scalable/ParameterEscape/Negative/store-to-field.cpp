// Assignment into a field of some object. The `this` node is not a candidate,
// so the sole rejection is the parameter's.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@S@Slot@F@keep#*I#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "held",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 30,
// CHECK-NEXT:       "file": "{{.*}}store-to-field.cpp",
// CHECK-NEXT:       "line": 35
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "StoreToField"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

struct Slot {
  int *held;
  void keep(int *p) { held = p; }
};

// Runtime half; see store-to-global.cpp for what the driver is for. The object
// is a global so that `held` outlives the scope `local` is declared in.
// DRIVER: Slot s;
// DRIVER: int main() {
// DRIVER:   int *k;
// DRIVER:   { int local = 1; s.keep(&local); k = s.held; }
// DRIVER:   return *k;
// DRIVER: }
