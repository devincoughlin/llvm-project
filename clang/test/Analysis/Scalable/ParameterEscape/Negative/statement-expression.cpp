// A GNU statement expression is transparent: the escape is attributed to what
// is done with the statement expression's value, not to the statement
// expression. If it stopped being transparent these would become
// UnrecognizedUse instead of naming the global.
//
// `escape_labelled` is the case that carries the coverage, and it is here
// because of a measurement rather than a guess. computeKind()'s StmtExpr arm
// has two ways to reach the right answer for a labelled last statement: it
// finds the value with ValueStmt::getExprStmt() -- the query
// Sema::BuildStmtExpr() itself used, which looks through the LabelStmt and
// AttributedStmt wrappers a label puts in the way -- and, three lines below,
// it falls back on isPointerCarryingType() when no value is found. Probed
// three ways over twelve statement-expression shapes (labelled, attributed,
// nested, doubly labelled, in C and in C++, feeding a store, a local, an
// argument, a return, a dereference and a subscript):
//
//   * getExprStmt() weakened to body_back()-as-Expr, backstop kept: every
//     verdict unchanged;
//   * backstop weakened to AliasKind::None, getExprStmt() kept: every verdict
//     unchanged;
//   * both weakened: every labelled and attributed form flips from rejected to
//     *annotatable* -- b_label, c_attr, d_local, e_arg, f_return, i_addr,
//     j_nested and k_two all move into non_escaping, while the unlabelled
//     `escape` below stays rejected throughout.
//
// So the two are redundant with each other and neither is individually
// observable, which is why no test can pin one against the other. What is
// observable, and what only a *labelled* statement expression can see, is the
// pair going together -- and that direction is the soundness inversion, a
// silent false negative. That is what `escape_labelled` is for; `escape`
// alone cannot see it.
//
// Two globals rather than one so that a swap between the two rejections shows
// up in `detail` as well as in the location.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Asserted leaf by leaf against literals, ids bound to USRs first; see
// ../lit.local.cfg for why this directory is written that way.
// CHECK:      "id": 0,
// CHECK:        "usr": "c:@F@escape#*I#"
// CHECK:      "id": 1,
// CHECK:        "usr": "c:@F@escape_labelled#*I#"
// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "g",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 27,
// CHECK-NEXT:       "file": "{{.*}}statement-expression.cpp",
// CHECK-NEXT:       "line": 84
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 0
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "StoreToGlobal"
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "detail": "g_labelled",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 45,
// CHECK-NEXT:       "file": "{{.*}}statement-expression.cpp",
// CHECK-NEXT:       "line": 86
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": 1
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "StoreToGlobal"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

int *g;
int *g_labelled;

void escape(int *p) { g = ({ p; }); }

void escape_labelled(int *p) { g_labelled = ({ lbl : p; }); }

// Runtime half; see store-to-global.cpp for what the driver is for. It drives
// the unlabelled `escape`; `escape_labelled` exists for the classifier
// measurement described at the top and stores into a second global.
// DRIVER: int main() {
// DRIVER:   int *k;
// DRIVER:   { int local = 1; escape(&local); k = g; }
// DRIVER:   return *k;
// DRIVER: }
