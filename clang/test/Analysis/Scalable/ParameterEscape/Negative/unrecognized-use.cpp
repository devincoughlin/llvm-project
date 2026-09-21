// The sound default, reached here through a pointer-to-member call: `->*` is
// a binary operator the analysis does not model, so its left operand sinks
// with the operator's spelling in `detail` rather than passing for a read.
//
// UnrecognizedUse is what every unmodelled shape falls to, so this file pins
// the shape of the fallback -- that it carries a detail identifying what was
// not understood -- rather than this one operator.

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
// CHECK-NEXT:     "detail": "->*",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 4,
// CHECK-NEXT:       "file": "{{.*}}unrecognized-use.cpp",
// CHECK-NEXT:       "line": 44
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "UnrecognizedUse"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

struct Table {
  void run();
};

void escape(Table *t) {
  auto member = &Table::run;
  (t->*member)();
}
