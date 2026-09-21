// The object expression of a *static* member function call. A static member
// called through an object is a plain CallExpr, not a CXXMemberCallExpr, so
// the object never becomes an implicit object argument and nothing consumes
// it: the member access is the use, and forming a callable out of an alias is
// not something the analysis models.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#*$@S@Table#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 25,
// CHECK-NEXT:       "file": "{{.*}}callable-use.cpp",
// CHECK-NEXT:       "line": 40
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "CallableUse"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

struct Table {
  static void run();
};

void escape(Table *t) { t->run(); }

// No runtime driver, and not because one is hard to write: this is **not a
// true escape**. `run` is a *static* member function, so `t->run()` evaluates
// `t`, discards it, and passes no object; nothing ever dereferences it. The
// rejection is a precision loss, and it is one that sits permanently outside
// this oracle -- no driver can demonstrate an escape that does not happen.
// Tracked by the M3 callables work (#4).
// NO-DRIVER: not a true escape -- a static member call passes no object
