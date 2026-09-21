// A thrown pointer propagates out of the frame exactly as a returned one
// does, but through a path the return edge does not model.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} -fcxx-exceptions -fexceptions %s --ssaf-tu-summary-file=%t/tu.json
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
// CHECK-NEXT:       "column": 29,
// CHECK-NEXT:       "file": "{{.*}}throw.cpp",
// CHECK-NEXT:       "line": 33
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "Throw"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void escape(int *p) { throw p; }

// Runtime half; see store-to-global.cpp for what the driver is for. The
// exception object carries the pointer out of the scope that owns the pointee,
// which is the escape this file asserts; the catch handler then reads it.
// DRIVER: int main() {
// DRIVER:   int *k = nullptr;
// DRIVER:   try { int local = 1; escape(&local); } catch (int *c) { k = c; }
// DRIVER:   return *k;
// DRIVER: }
//
// The one driver in the corpus whose report cannot name the object. Measured:
// AddressSanitizer says `is located in stack of thread T0` and stops, with no
// frame description and no `'local'`, because the frame carrying that
// description is clobbered by the unwind before the read happens. Three
// rearrangements were tried -- ending the scope by normal flow after the
// catch, moving the whole thing into a noinline callee, and forcing the
// address to be taken -- and none restores the description.
//
// So the attribution here is weaker than every other file's, and deliberately
// says so. `is located in stack of thread` would read like attribution, but it
// appears in *every* stack-use-after-scope report, and the floor on
// DRIVER-EVIDENCE refuses it for exactly that reason. What is left is the file
// name, which establishes only that the faulting read is in code compiled from
// this file; the kind is doing the rest of the work.
// DRIVER-EVIDENCE: throw.cpp
