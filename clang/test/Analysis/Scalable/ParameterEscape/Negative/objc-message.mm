// An Objective-C message send: the receiver's implementation is chosen at run
// time. The receiver parameter is an Objective-C object pointer and so is not
// a candidate, leaving the object pointer as the only one.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} -x objective-c++ %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#*$objc(cs)Keeper#*I#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 42,
// CHECK-NEXT:       "file": "{{.*}}objc-message.mm",
// CHECK-NEXT:       "line": 38
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 1
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "ObjCMessage"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

@interface Keeper
- (void)keep:(int *)q;
@end

void escape(Keeper *k, int *p) { [k keep:p]; }
