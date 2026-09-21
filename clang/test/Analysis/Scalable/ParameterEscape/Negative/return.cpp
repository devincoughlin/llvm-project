// A parameter returned to the caller outlives the call. The fixpoint, not the
// classifier, raises this one: the extractor records `returns_self_at` and
// finalize() turns it into Return.

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
// CHECK-NEXT:       "column": 23,
// CHECK-NEXT:       "file": "{{.*}}return.cpp",
// CHECK-NEXT:       "line": 34
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "Return"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

int *escape(int *p) { return p; }

// Runtime half; see store-to-global.cpp for what the driver is for.
//
// `k` is `volatile` here and nowhere else in the corpus, and it has to be:
// with a plain `int *k` this driver produces *no* ASan report at -O2, measured.
// Nothing in it touches memory the optimizer cannot see through -- `escape` is
// the identity on a pointer to a local -- so the load is forwarded to `local`
// and sunk back inside its lifetime, and the dangling read stops existing. It
// does report at -O0. An escape the optimizer can delete was never observable,
// so the driver stores through a volatile slot to keep it.
// DRIVER: int main() {
// DRIVER:   int *volatile k;
// DRIVER:   { int local = 1; k = escape(&local); }
// DRIVER:   return *k;
// DRIVER: }
