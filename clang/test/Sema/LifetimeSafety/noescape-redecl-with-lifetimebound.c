// A parameter carrying both `noescape` and `lifetimebound` must not also be
// told that its return value could not be lifetime bound.
//
// `checkAnnotations` reports the noescape violation and returns early, which
// skips the bookkeeping that records the parameter as having escaped through the
// return. `reportLifetimeboundViolations` then concludes it never escapes and
// emits `could not verify that the return value can be lifetime bound`, about a
// function that demonstrably returns it. The early return records the escape
// first so that does not happen.
//
// The root cause predates the redeclaration walk -- both attributes on the
// definition already reached it, which `both_on_definition` below pins -- but
// resolving `noescape` across redeclarations made it reachable for a new class
// of input, where the `noescape` half comes from a header or from API Notes and
// nothing in the file being compiled mentions it.

// RUN: %clang_cc1 -fsyntax-only -std=c17 \
// RUN:   -Wlifetime-safety-noescape -Wlifetime-safety-lifetimebound-violation \
// RUN:   -verify %s

// Reporting control: the lifetimebound-violation diagnostic is live in this
// file. This parameter is marked lifetimebound and does not escape, so the
// diagnostic is correct here.
int *lb_never_escapes(int *p __attribute__((lifetimebound)), int *q) {
  // expected-warning@-1 {{could not verify that the return value can be lifetime bound to 'p'}}
  return q;
}

// `noescape` on the declaration, `lifetimebound` on the definition. The
// noescape violation is wanted; the lifetimebound violation is not, because the
// function does return the parameter.
int *both_across_redecl(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *both_across_redecl(int *p __attribute__((lifetimebound))) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p; // expected-note {{param returned here}}
}

// The pre-existing shape: both attributes written on the definition. Same
// requirement, and this is what shows the fix is not specific to redeclarations.
int *both_on_definition(int *p __attribute__((noescape)) // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
                        __attribute__((lifetimebound))) {
  return p; // expected-note {{param returned here}}
}

// A noescape parameter that does *not* escape gets neither diagnostic.
void neither(int *p __attribute__((noescape)));
void neither(int *p) { *p = 1; }
