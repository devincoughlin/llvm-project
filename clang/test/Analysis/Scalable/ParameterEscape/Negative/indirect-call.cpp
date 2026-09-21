// A call through a function pointer has no callee declaration to attribute the
// argument to. The function-pointer parameter is neither analyzed nor a
// candidate, so the only rejection is the object pointer's.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#*I#*Fv(#S0_)#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 45,
// CHECK-NEXT:       "file": "{{.*}}indirect-call.cpp",
// CHECK-NEXT:       "line": 34
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "IndirectCall"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void escape(int *p, void (*fn)(int *)) { fn(p); }

// "Neither analyzed nor a candidate" is exact, and was wrong here until it was
// measured: isPointerCarryingType() excludes isFunctionPointerType(), so no
// fact is recorded for `fn` at all and it is absent from the summary's
// `params` array -- not merely excluded from `candidate_params`. A function
// *reference* is the other way round: analyzed, and not a candidate.
//
// Runtime half; see store-to-global.cpp for what a driver is for. The function
// the driver passes is one that retains -- which is the whole point of
// refusing an indirect call: nothing at the call site says which it will be.
// DRIVER: int *leak;
// DRIVER: static void keep(int *q) { leak = q; }
// DRIVER: int main() {
// DRIVER:   { int local = 1; escape(&local, keep); }
// DRIVER:   return *leak;
// DRIVER: }
