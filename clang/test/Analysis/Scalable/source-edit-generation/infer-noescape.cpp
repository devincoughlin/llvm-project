// End-to-end test of the `infer-noescape` transformation driven by the
// clang driver and by cc1. Proves the transformation is registered, that its
// anchor keeps it linked into `clang`, that every redeclaration of a proven
// parameter is edited, and that --ssaf-noescape-spelling= reaches it.
//
// The input suite names `f`'s parameter 0 as provably non-escaping. `g` is
// absent from the suite and must not be touched.

// DEFINE: %{run} = --ssaf-source-transformation=infer-noescape \
// DEFINE:   --ssaf-global-scope-analysis-result=%S/Inputs/infer-noescape-suite.json \
// DEFINE:   --ssaf-compilation-unit-id=cu --ssaf-link-unit-id=lu

// =============================================================================
// 1. Default spelling, through the driver. Both redeclarations are edited.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -c %s -o %t/test.o %{run} \
// RUN:   --ssaf-src-edit-file=%t/edits.yaml \
// RUN:   --ssaf-transformation-report-file=%t/report.sarif

// RUN: FileCheck --check-prefix=EDITS --input-file=%t/edits.yaml %s
// EDITS:      MainSourceFile: {{.*}}infer-noescape.cpp
// EDITS:      Replacements:
// EDITS:        - FilePath: {{.*}}infer-noescape.cpp
// EDITS-NEXT:     Offset: {{[0-9]+}}
// EDITS-NEXT:     Length: 0
// EDITS-NEXT:     ReplacementText: '__attribute__((noescape)) '
// EDITS-NEXT:   - FilePath: {{.*}}infer-noescape.cpp
// EDITS-NEXT:     Offset: {{[0-9]+}}
// EDITS-NEXT:     Length: 0
// EDITS-NEXT:     ReplacementText: '__attribute__((noescape)) '
// EDITS-NOT:      ReplacementText:

// CHECK-NOT after a CHECK-DAG group only guards the region after the last DAG
// match, so a skip emitted before the inserted result would slip through --
// and a system-header or typedef-declared redeclaration at the top of a file
// is reported before the definition. --implicit-check-not is
// position-independent.
// RUN: FileCheck --check-prefix=REPORT --input-file=%t/report.sarif %s \
// RUN:   --implicit-check-not='"ruleId": "noescape-skipped"' \
// RUN:   --implicit-check-not='"ruleId": "noescape-rejected"'
// REPORT-DAG: "fullName": {{.*}}infer-noescape
// REPORT-DAG: "ruleId": "noescape-inserted"
// REPORT-DAG: "text": "inserted __attribute__((noescape)) (entity=c:@F@f#*I#I# param=0)"

// =============================================================================
// 2. Spelling override, through cc1.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_cc1 %s %{run} \
// RUN:   --ssaf-noescape-spelling=NS_NOESCAPE \
// RUN:   --ssaf-src-edit-file=%t/edits.yaml \
// RUN:   --ssaf-transformation-report-file=%t/report.sarif \
// RUN:   -emit-obj -o %t/test.o

// RUN: FileCheck --check-prefix=SPELLED --input-file=%t/edits.yaml %s \
// RUN:   --implicit-check-not='__attribute__((noescape))'
// SPELLED:      ReplacementText: 'NS_NOESCAPE '
// SPELLED:      ReplacementText: 'NS_NOESCAPE '

void f(int *p, int n);

void f(int *p, int n) {
  (void)p;
  (void)n;
}

void g(int *q) { (void)q; }
