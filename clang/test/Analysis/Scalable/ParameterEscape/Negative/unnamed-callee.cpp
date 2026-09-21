// A flow into a callee the entity model cannot name.
//
// `Outer`'s copy constructor is implicit, and non-trivial because `Inner`'s is
// user-declared -- so it is a real callee with a real body, but one with no
// USR to key a summary on. The edge cannot be recorded, so the argument sinks
// instead: `detail` names the callee that could not be named, and the reason
// is what catches everything the entity model cannot reach.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#&$@S@Outer#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "Outer",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 30,
// CHECK-NEXT:       "file": "{{.*}}unnamed-callee.cpp",
// CHECK-NEXT:       "line": 49
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "UnnamedCallee"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

struct Inner {
  Inner();
  Inner(const Inner &);
};

struct Outer {
  Inner i;
};

void take(Outer);

void escape(Outer &o) { take(o); }

// Runtime half; see store-to-global.cpp for what a driver is for. The escape
// is through the copy constructor the analysis cannot name: the driver defines
// it to retain the address of the object it copied from, which is a subobject
// of the caller's `local`.
// DRIVER: int *leak;
// DRIVER: Inner::Inner() {}
// DRIVER: Inner::Inner(const Inner &other) { leak = (int *)&other; }
// DRIVER: void take(Outer) {}
// DRIVER: int main() {
// DRIVER:   { Outer local; escape(local); }
// DRIVER:   return *leak;
// DRIVER: }
