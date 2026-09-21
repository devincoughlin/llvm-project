// Assignment into a variable with static storage. The global's name rides
// along in `detail`, which is what a diagnostic would show the user.

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
// CHECK-NEXT:     "detail": "g",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 27,
// CHECK-NEXT:       "file": "{{.*}}store-to-global.cpp",
// CHECK-NEXT:       "line": 35
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "StoreToGlobal"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

int *g;

void escape(int *p) { g = p; }

// The runtime half of this file. `validate_escape_corpus.py` compiles the code
// above together with the `main` below at -O2 under AddressSanitizer and
// requires a report: that is what makes this file a *true* escape rather than
// merely a program this analysis rejects. The driver lives at the end so that
// adding it shifts none of the line numbers the CHECK lines pin.
// DRIVER: int main() {
// DRIVER:   int *k;
// DRIVER:   { int local = 1; escape(&local); k = g; }
// DRIVER:   return *k;
// DRIVER: }
