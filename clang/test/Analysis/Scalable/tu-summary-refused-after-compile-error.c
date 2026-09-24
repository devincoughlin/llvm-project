// A translation unit that failed to compile must not produce a TU summary.
//
// The facts come from an error-recovery AST, where a use the analysis relies
// on seeing may simply be absent, and a missing use is a missing escape. The
// guard is in the shared writing path, so it covers every extractor, not just
// ParameterEscape.
//
// Every section resets %t. --ssaf-tu-summary-file= refuses to overwrite an
// existing file, so without the reset a 'not test -e' could pass against a
// stale file left by an earlier section rather than against a refusal.
//
// %{filecheck} carries --match-full-lines, so every pattern below -- the
// CHECK-NOT ones included -- is wrapped in {{.*}}. A bare CHECK-NOT under
// --match-full-lines can never match and would be silently vacuous.

// DEFINE: %{filecheck} = FileCheck %s --match-full-lines --check-prefix
// DEFINE: %{extract} = --ssaf-compilation-unit-id=cu-45 --ssaf-extract-summaries=ParameterEscape
// DEFINE: %{bad-header} = echo 'void broken_header_fn(void) { undeclared_identifier_45; }'

// =============================================================================
// 0. Reporting control: the same source, no errors, does write a summary.
//    If this section ever fails, no 'not test -e' below is evidence of
//    anything -- the harness itself stopped extracting.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang     -fsyntax-only %s %{extract} --ssaf-tu-summary-file=%t/control-driver.json 2>&1 | count 0
// RUN: test -e %t/control-driver.json
// RUN: %clang_cc1 -fsyntax-only %s %{extract} --ssaf-tu-summary-file=%t/control-cc1.json 2>&1 | count 0
// RUN: test -e %t/control-cc1.json
//
// And it is a summary with content: 'test -e' would pass against an empty file.
// RUN: %{filecheck}=CONTROL-BODY --input-file=%t/control-driver.json
// CONTROL-BODY: {{.*}}takes_pointer{{.*}}

// =============================================================================
// 1. A use of an undeclared identifier in a header -- the Libnotify shape,
//    where the main file is fine and an included header is not.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{bad-header} > %t/bad.h
// RUN: not %clang     -fsyntax-only %s -include %t/bad.h %{extract} --ssaf-tu-summary-file=%t/hdr-driver.json 2>&1 | %{filecheck}=HDR-DRIVER
// RUN: not test -e %t/hdr-driver.json
// HDR-DRIVER: {{.*}}error: use of undeclared identifier 'undeclared_identifier_45'{{.*}}
// HDR-DRIVER: {{.*}}error: not writing TU summary to '{{.*}}/hdr-driver.json': this translation unit failed to compile, so its summary would describe an error-recovery AST [-Wscalable-static-analysis-framework]
//
// RUN: not %clang_cc1 -fsyntax-only %s -include %t/bad.h %{extract} --ssaf-tu-summary-file=%t/hdr-cc1.json 2>&1 | %{filecheck}=HDR-CC1
// RUN: not test -e %t/hdr-cc1.json
// HDR-CC1: {{.*}}error: not writing TU summary to '{{.*}}/hdr-cc1.json': this translation unit failed to compile, so its summary would describe an error-recovery AST [-Wscalable-static-analysis-framework]

// =============================================================================
// 2. A header that is not there at all. This is a *fatal* diagnostic, and it
//    reaches the guard through the same flag: a fatal diagnostic is
//    error-mapped by default, so it sets UncompilableErrorOccurred. No
//    separate hasFatalErrorOccurred() disjunct is needed, and this is what
//    pins that.
//
//    Note what the refusal looks like here: the file is absent, but the
//    refusal *diagnostic* is not printed. DiagnosticsEngine::ProcessDiag
//    silences every non-note diagnostic that follows a fatal one -- and the
//    refusal is the diagnostic that both trips FatalErrorOccurred and is then
//    swallowed by it. So after a fatal error the refusal is silent.
//
//    That is acceptable and is why both halves are checked here: a fatal error
//    is already the loudest possible failure and the exit status is non-zero,
//    so the consumer is not relying on the refusal to learn anything. The
//    refusal earns its keep in the recoverable-error cases above, where a
//    build system would otherwise see errors alongside a summary that was
//    written anyway. Pinning the silence means a future change that starts
//    printing it -- or, far worse, starts writing the file -- is caught.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: not %clang -fsyntax-only %s -DMISSING_INCLUDE %{extract} --ssaf-tu-summary-file=%t/nohdr.json 2>&1 | %{filecheck}=NOHDR
// RUN: not test -e %t/nohdr.json
// NOHDR: {{.*}}fatal error: 'this-header-does-not-exist-45.h' file not found{{.*}}
// NOHDR-NOT: {{.*}}not writing TU summary{{.*}}

// =============================================================================
// 3. The guard sits in the shared writing path, so PointerFlow is refused too.
//    The issue was filed from ParameterEscape evidence, but nothing about the
//    guard is ParameterEscape-specific and its generality needs pinning.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{bad-header} > %t/bad.h
// Control first, with the same extractor and no errors.
// RUN: %clang -fsyntax-only %s --ssaf-compilation-unit-id=cu-45 --ssaf-extract-summaries=PointerFlow --ssaf-tu-summary-file=%t/pf-control.json 2>&1 | count 0
// RUN: test -e %t/pf-control.json
// RUN: not %clang -fsyntax-only %s -include %t/bad.h --ssaf-compilation-unit-id=cu-45 --ssaf-extract-summaries=PointerFlow --ssaf-tu-summary-file=%t/pf.json 2>&1 | %{filecheck}=PF
// RUN: not test -e %t/pf.json
// PF: {{.*}}error: not writing TU summary to '{{.*}}/pf.json': this translation unit failed to compile, so its summary would describe an error-recovery AST [-Wscalable-static-analysis-framework]

// =============================================================================
// 4. A warning promoted to an error by -Werror is NOT a compile failure: the
//    AST is well-formed and the summary is still written.
//
//    This is what picks hasUncompilableErrorOccurred() over
//    hasErrorOccurred(), which is true here and would cost a summary to every
//    -Werror build that tripped a benign warning.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: not %clang -fsyntax-only %s -DUNUSED_LOCAL -Werror -Wunused-variable %{extract} --ssaf-tu-summary-file=%t/werror.json 2>&1 | %{filecheck}=WERROR
// RUN: test -e %t/werror.json
// WERROR: {{.*}}error: unused variable 'unused_local' [-Werror,-Wunused-variable]{{.*}}
// WERROR-NOT: {{.*}}not writing TU summary{{.*}}

// =============================================================================
// 5. An error clang classifies as recoverable-for-codegen -- here
//    "is unavailable" -- IS a compile failure for this purpose: the call was
//    still dropped from the AST.
//
//    This is what picks hasUncompilableErrorOccurred() over
//    hasUnrecoverableErrorOccurred(), which is false here, and which is also
//    false for every ARC diagnostic -- and the field evidence in the issue is
//    Objective-C.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: not %clang -fsyntax-only %s -DUNAVAILABLE_USE %{extract} --ssaf-tu-summary-file=%t/unavail.json 2>&1 | %{filecheck}=UNAVAIL
// RUN: not test -e %t/unavail.json
// UNAVAIL: {{.*}}error: 'gone' is unavailable: gone{{.*}}
// UNAVAIL: {{.*}}error: not writing TU summary to '{{.*}}/unavail.json': this translation unit failed to compile, so its summary would describe an error-recovery AST [-Wscalable-static-analysis-framework]

// =============================================================================
// 6. The refusal is a diagnostic in -Wscalable-static-analysis-framework and
//    is downgradable like the rest of them. Downgrading changes the severity
//    of the report, never the refusal: no file appears either way.
// =============================================================================

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{bad-header} > %t/bad.h
// RUN: not %clang -fsyntax-only %s -include %t/bad.h -Wno-error=scalable-static-analysis-framework %{extract} --ssaf-tu-summary-file=%t/warn.json 2>&1 | %{filecheck}=DEMOTED
// RUN: not test -e %t/warn.json
// DEMOTED: {{.*}}warning: not writing TU summary to '{{.*}}/warn.json': this translation unit failed to compile, so its summary would describe an error-recovery AST [-Wscalable-static-analysis-framework]

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{bad-header} > %t/bad.h
// RUN: not %clang -fsyntax-only %s -include %t/bad.h -Wno-scalable-static-analysis-framework %{extract} --ssaf-tu-summary-file=%t/silent.json 2>&1 | %{filecheck}=SILENCED
// RUN: not test -e %t/silent.json
// The header's own error still prints, so this stream is not empty -- which is
// what makes the CHECK-NOT below meaningful rather than trivially true.
// SILENCED: {{.*}}error: use of undeclared identifier 'undeclared_identifier_45'{{.*}}
// SILENCED-NOT: {{.*}}not writing TU summary{{.*}}

// =============================================================================
// Source.
// =============================================================================

#ifdef MISSING_INCLUDE
#include "this-header-does-not-exist-45.h"
#endif

void takes_pointer(int *p) { (void)p; }

int *global_sink;
void leaks_pointer(int *p) { global_sink = p; }

#ifdef UNUSED_LOCAL
void has_unused_local(void) { int unused_local; }
#endif

#ifdef UNAVAILABLE_USE
__attribute__((unavailable("gone"))) void gone(void);
void calls_gone(int *p) {
  gone();
  (void)p;
}
#endif
