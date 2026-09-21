// The variadic tail of a prototyped callee. Same absence of a callee
// parameter as unmatched-argument.c, but separated into its own reason
// because a variadic tail is the one shape a caller can see coming.

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
// CHECK-NEXT:       "column": 33,
// CHECK-NEXT:       "file": "{{.*}}var-args.cpp",
// CHECK-NEXT:       "line": 36
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "VarArgs"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void report(int, ...);

void escape(int *p) { report(1, p); }

// Runtime half; see store-to-global.cpp for what a driver is for. `report` is
// left bodiless by this file, so the driver supplies the definition -- and
// reading the pointer back out with `va_arg` is what makes the escape through
// the variadic tail observable.
// DRIVER: #include <cstdarg>
// DRIVER: int *leak;
// DRIVER: void report(int n, ...) {
// DRIVER:   va_list ap;
// DRIVER:   va_start(ap, n);
// DRIVER:   leak = va_arg(ap, int *);
// DRIVER:   va_end(ap);
// DRIVER: }
// DRIVER: int main() {
// DRIVER:   { int local = 1; escape(&local); }
// DRIVER:   return *leak;
// DRIVER: }
