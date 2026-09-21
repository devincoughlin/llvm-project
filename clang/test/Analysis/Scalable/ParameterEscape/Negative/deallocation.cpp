// `noescape` forbids deallocating through the parameter, so a deallocator is
// refused even though it captures nothing.

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
// CHECK-NEXT:       "column": 30,
// CHECK-NEXT:       "file": "{{.*}}deallocation.cpp",
// CHECK-NEXT:       "line": 33
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "Deallocation"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void escape(int *p) { delete p; }

// Runtime half; see store-to-global.cpp for what a driver is for. The only
// driver in the corpus that is not a stack escape: `noescape` forbids freeing
// through the parameter as well as retaining it, and the observable
// consequence of freeing the caller's object is a use after free, not a use
// after scope.
//
// The attribution here is weaker than every other file's, and deliberately
// says so. A heap report names no variable, and the evidence that would read
// best -- `freed by thread` -- appears in *every* heap-use-after-free, so the
// floor on DRIVER-EVIDENCE refuses it: it would attribute nothing while
// reading like attribution. The free is attributed to the driver's call site
// rather than to `delete p` (measured: `main deallocation.cpp:51`, the
// `escape(heap_block)` line), so there is no line in this file's own code to
// anchor on either. What is left is the file name, which establishes only
// that the faulting read is in code compiled from this file. The `DRIVER-KIND`
// above is doing the real work: it is what stops a stack bug in the driver
// standing in for the free this file is about.
// DRIVER-KIND: heap-use-after-free
// DRIVER-EVIDENCE: deallocation.cpp
// DRIVER: int main() {
// DRIVER:   int *heap_block = new int(1);
// DRIVER:   escape(heap_block);
// DRIVER:   return *heap_block;
// DRIVER: }
