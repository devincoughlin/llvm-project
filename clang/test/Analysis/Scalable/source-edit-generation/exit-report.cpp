// The milestone exit report, `clang/utils/ssaf/noescape_exit_report.py`, over
// a real SARIF file produced by the transformation itself.
//
// The script is the one artifact of this milestone that nothing else runs.
// There is no CI here; without this file it would be exercised only by whoever
// happened to type it, which is the same "invisible coverage" the unsupported
// tests in this directory are. So this test drives every counting path it has
// and asserts the numbers.
//
// The input suite is a fixture rather than a linker run, because what is under
// test is the aggregation and a fixture is the only way to control both the
// counts and the order they are emitted in.
//
// Both of those are load-bearing, and the order is the one that is easy to get
// wrong. Rejections are emitted in the order the visitor reaches the function
// *definitions*, so the definitions below are ordered `f`, `n`, `k`, `m`, `h`,
// `g` -- one UnanalyzedCallee, then two EscapesViaCallee, then three
// StoreToField, then four StoreToGlobal.
// That is the exact reverse of frequency order, so the ranking assertion fails
// if the script stops sorting. With the definitions in their natural order the
// emission order would already equal frequency order and the assertion would
// hold whether or not `most_common()` were called at all -- measured.
//
// `g` therefore needs a declaration before `k` and `m` can call it, and its
// rejections are still reported at its definition at the bottom.
//
// Two callers rather than one, both blamed on the same callee parameter, for
// the --blame assertion below. With a single blamed rejection the correct
// ranking prints `... param 0: 1`, and so does the defect that ranks the
// rejected *callers* instead -- the row is byte-identical and the test cannot
// tell them apart. Two collapse into one row with `: 2`, which no per-caller
// count can produce.

// =============================================================================
// 1. All three rule ids, the reason ranking, and the blame ranking.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t/reports
// RUN: %clang -fsyntax-only %s \
// RUN:   --ssaf-source-transformation=infer-noescape \
// RUN:   --ssaf-global-scope-analysis-result=%S/Inputs/infer-noescape-exit-report-suite.json \
// RUN:   --ssaf-compilation-unit-id=cu --ssaf-link-unit-id=lu \
// RUN:   --ssaf-src-edit-file=%t/edits.yaml \
// RUN:   --ssaf-transformation-report-file=%t/reports/report.sarif

// `f`'s parameter is written through a macro in its first declaration and
// spelled out in its definition, which is what makes one site inserted and the
// other skipped in a single translation unit.
//
// --blame is given the suite because the blamed callee's identity is not in
// the SARIF message at all; the same file serves as a whole-program result
// here because it carries an id table and a verdict, which is all the ranking
// reads.
// RUN: %python %S/../../../../utils/ssaf/noescape_exit_report.py %t/reports \
// RUN:   --blame %S/Inputs/infer-noescape-exit-report-suite.json \
// RUN:   | FileCheck %s --check-prefix=REPORT --match-full-lines
// REPORT:      1 SARIF file(s)
// REPORT-NEXT: noescape-inserted: 1
// REPORT-NEXT: noescape-skipped: 1
// REPORT-NEXT: noescape-rejected: 10
// REPORT-NEXT: rejection reasons (10 of 10 parsed):
// REPORT-NEXT:   StoreToGlobal: 4 (40.0%)
// REPORT-NEXT:   StoreToField: 3 (30.0%)
// REPORT-NEXT:   EscapesViaCallee: 2 (20.0%)
// REPORT-NEXT:   UnanalyzedCallee: 1 (10.0%)
// REPORT-NEXT: blamed callee parameters:
// REPORT-NEXT:   c:@F@g#*I#S0_#S0_#S0_# param 0: 2
// `n` is blamed on a callee too, for UnanalyzedCallee rather than
// EscapesViaCallee -- a callee that is simply not in the link unit, which
// calls for a different action. It is counted and named, not ranked, and not
// silently folded into the row above.
// REPORT-NEXT: blamed for UnanalyzedCallee (not ranked): 1
// The blame block has to be terminated: rows printed after the last
// REPORT-NEXT are asserted by nothing, and the caller-ranking defect prints
// nine of them. --match-full-lines makes `{{.+}}` mean "any non-empty line".
// REPORT-NOT: {{.+}}

// =============================================================================
// 2. A message the script cannot parse is counted and named, never dropped.
// =============================================================================

// The fixture holds one well-formed rejection and one whose text has been
// reshaped, which is what a change to InferNoescape.cpp's message would look
// like here. Without this the counter is pinned by nothing: deleting both the
// increment and the print leaves every other assertion in this file green.
// RUN: %python %S/../../../../utils/ssaf/noescape_exit_report.py \
// RUN:   %S/Inputs/noescape-exit-report-malformed.sarif \
// RUN:   | FileCheck %s --check-prefix=MALFORMED --match-full-lines
// MALFORMED:      noescape-rejected: 2
// MALFORMED-NEXT: rejection reasons (1 of 2 parsed):
// MALFORMED-NEXT:   StoreToGlobal: 1 (100.0%)
// MALFORMED-NEXT:   <unparsed message>: 1

// =============================================================================
// 3. Nothing to aggregate is an error, not three zeroes.
// =============================================================================

// Three zeroes read exactly like a run in which everything was accepted, which
// is the failure mode this milestone can least afford to report. Both shapes
// are checked: a directory that exists and holds no SARIF, and a path that
// does not exist at all -- the second is what a typo produces, and it used to
// print `1 SARIF file(s)` and three zeroes.
// RUN: mkdir -p %t/nothing
// RUN: not %python %S/../../../../utils/ssaf/noescape_exit_report.py \
// RUN:   %t/nothing 2>&1 | FileCheck %s --check-prefix=EMPTY
// EMPTY: error: no .sarif file found under {{.*}}nothing

// RUN: not %python %S/../../../../utils/ssaf/noescape_exit_report.py \
// RUN:   %t/no-such-dir 2>&1 | FileCheck %s --check-prefix=MISSING
// MISSING: error: no such path: {{.*}}no-such-dir

#define PTR int *
void f(PTR p, int n);
void f(int *p, int n) {
  (void)p;
  (void)n;
}

void g(int *a, int *b, int *c, int *d);

void n(int *a) { g(a, a, a, a); }

void k(int *a) { g(a, a, a, a); }

void m(int *a) { g(a, a, a, a); }

void h(int *a, int *b, int *c);
void h(int *a, int *b, int *c) { (void)a, (void)b, (void)c; }

int *ga;
int *gb;
int *gc;
int *gd;
void g(int *a, int *b, int *c, int *d) {
  ga = a;
  gb = b;
  gc = c;
  gd = d;
}
