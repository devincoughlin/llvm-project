// API Notes give a parameter a NoEscapeAttr in the AST without the attribute
// existing in the header's bytes. Deciding idempotency on the AST would skip
// the header's declaration *silently* -- no edit and no report -- leaving the
// definition annotated, the header not, and design section 5.5's pipeline
// backstop with nothing to refuse on.
//
// Both halves of the same compilation are checked here, one flag apart:
// without API Notes the attribute is nowhere, so both sites are edited; with
// API Notes the header's site cannot be edited and must be *reported*.

// DEFINE: %{mod} = -fmodules -fimplicit-module-maps \
// DEFINE:   -I%S/Inputs/NoescapeAPINotes
// DEFINE: %{run} = --ssaf-source-transformation=infer-noescape \
// DEFINE:   --ssaf-global-scope-analysis-result=%S/Inputs/infer-noescape-c-suite.json \
// DEFINE:   --ssaf-compilation-unit-id=cu --ssaf-link-unit-id=lu

// =============================================================================
// 1. -fno-apinotes-modules: the attribute exists nowhere, so both the header
//    declaration and the definition are edited, and nothing is skipped.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -c %s -o %t/test.o %{mod} -fmodules-cache-path=%t/mc-off \
// RUN:   -fno-apinotes-modules %{run} \
// RUN:   --ssaf-src-edit-file=%t/edits.yaml \
// RUN:   --ssaf-transformation-report-file=%t/report.sarif

// RUN: FileCheck --check-prefix=OFF-EDITS --input-file=%t/edits.yaml %s
// OFF-EDITS-DAG: FilePath: {{.*}}noescape-mod.h
// OFF-EDITS-DAG: FilePath: {{.*}}infer-noescape-apinotes.c

// RUN: FileCheck --check-prefix=OFF-REPORT --input-file=%t/report.sarif %s \
// RUN:   --implicit-check-not='"ruleId": "noescape-skipped"'
// OFF-REPORT: "ruleId": "noescape-inserted"

// =============================================================================
// 2. -fapinotes-modules: the header's declaration carries the attribute in the
//    AST only. It must be reported as skipped, not dropped in silence, and the
//    definition is still edited.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -c %s -o %t/test.o %{mod} -fmodules-cache-path=%t/mc-on \
// RUN:   -fapinotes-modules %{run} \
// RUN:   --ssaf-src-edit-file=%t/edits.yaml \
// RUN:   --ssaf-transformation-report-file=%t/report.sarif

// The header is not edited -- it cannot be -- and the definition still is.
// RUN: FileCheck --check-prefix=ON-EDITS --input-file=%t/edits.yaml %s \
// RUN:   --implicit-check-not='noescape-mod.h'
// ON-EDITS: FilePath: {{.*}}infer-noescape-apinotes.c

// The skip is present, is a warning, and carries a location in the header, so
// the section 5.5 policy can see that a site was left behind.
// RUN: FileCheck --check-prefix=ON-REPORT --input-file=%t/report.sarif %s
// ON-REPORT-DAG: "ruleId": "noescape-inserted"
// ON-REPORT-DAG: "ruleId": "noescape-skipped"
// ON-REPORT-DAG: "level": "warning"
// ON-REPORT-DAG: "uri": "file://{{.*}}noescape-mod.h"
// ON-REPORT-DAG: "text": "cannot insert noescape here: the parameter is noescape in this compilation but the attribute is not written in this declaration{{.*}}"

#include "noescape-mod.h"

void f(int *p, int n) {
  (void)p;
  (void)n;
}
