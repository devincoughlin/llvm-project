// The body that runs at a virtual call site is the override's, which need not
// be in this translation unit. The receiver is a global so the file has a
// single candidate.

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
// CHECK-NEXT:       "column": 37,
// CHECK-NEXT:       "file": "{{.*}}virtual-call.cpp",
// CHECK-NEXT:       "line": 40
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "VirtualCall"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

struct Base {
  virtual void take(int *);
};

Base receiver;

void escape(int *p) { receiver.take(p); }

// Runtime half; see store-to-global.cpp for what a driver is for. `Base::take`
// is left bodiless by this file; the driver defines the override that runs and
// makes it retain, which is what the analysis cannot know at the call site.
// DRIVER: int *leak;
// DRIVER: void Base::take(int *q) { leak = q; }
// DRIVER: int main() {
// DRIVER:   { int local = 1; escape(&local); }
// DRIVER:   return *leak;
// DRIVER: }
