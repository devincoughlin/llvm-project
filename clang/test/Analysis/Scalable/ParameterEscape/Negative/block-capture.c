// The block literal form of the same escape. A block that is copied outlives
// the frame, and nothing here proves it is not.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} -fblocks %s --ssaf-tu-summary-file=%t/tu.json
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
// CHECK-NEXT:       "column": 28,
// CHECK-NEXT:       "file": "{{.*}}block-capture.c",
// CHECK-NEXT:       "line": 35
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "Capture"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

void take(void (^)(void));

void escape(int *p) { take(^{ (void)*p; }); }

// No runtime driver, measured rather than assumed. A driver can define `take`
// to `Block_copy` the block and invoke it after the scope ends, but the
// block's only use of the pointer is `(void)*p`, a discarded load that -O2
// deletes, so the program reports nothing. Making it observable would mean
// changing the body this file exists to classify.
// NO-DRIVER: the block's only use of the pointer is a load -O2 deletes
