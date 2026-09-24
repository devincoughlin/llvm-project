// A `noescape` on a redeclaration that comes *after* the definition is seen in
// translation-unit mode and not in per-function mode.
//
// This is not a choice made by the audit. Per-function mode runs the analysis
// at the end of each function body, so a redeclaration parsed later does not
// exist yet and `redecls()` cannot return it. TU mode analyses at end of TU,
// by which time it does. The cross-TU inference trusts the annotation either
// way, so per-function mode under-reports here.
//
// This file pins the current behaviour of both modes rather than asserting that
// either is right. The divergence is filed as a follow-up.

// RUN: %clang_cc1 -fsyntax-only -std=c17 -fexperimental-lifetime-safety-c \
// RUN:   -Wlifetime-safety-noescape -Wlifetime-safety-suggestions \
// RUN:   -verify=common,perfunc %s
// RUN: %clang_cc1 -fsyntax-only -std=c17 -fexperimental-lifetime-safety-c \
// RUN:   -fexperimental-lifetime-safety-tu-analysis \
// RUN:   -Wlifetime-safety-noescape -Wlifetime-safety-suggestions \
// RUN:   -verify=common,tu %s

// Reporting control: a declaration that *precedes* the definition is seen in
// both modes, so this fires in both and neither run can be silently empty.
int *before(int *p __attribute__((noescape))); // common-note {{'noescape' attribute appears here}}
int *before(int *p) { // common-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;           // common-note {{param returned here}}
}

// The case under test. In per-function mode the annotation is invisible and the
// parameter instead gets the lifetimebound suggestion -- the very diagnostic
// this change stops emitting when the annotation *is* visible.
int *after(int *p) { // perfunc-warning {{parameter in intra-TU function should be marked [[clang::lifetimebound]]}}
                     // tu-warning@-1 {{parameter is marked [[clang::noescape]] but escapes}}
  return p;          // common-note {{param returned here}}
}
int *after(int *p __attribute__((noescape))); // tu-note {{'noescape' attribute appears here}}
