// Resolving `noescape` across redeclarations also suppresses the
// `lifetimebound` suggestion for that parameter.
//
// `checkAnnotations` decides `noescape` first and returns, so the suggestion
// path is never reached for a parameter that is already annotated. Before the
// redeclaration walk, a `noescape` written only on a declaration was invisible
// there too, and this parameter was told to add `lifetimebound` -- the one
// annotation it is mutually exclusive with. Measured against the previous
// binary, the declaration on line 20 used to carry
// `warning: parameter in intra-TU function should be marked [[clang::lifetimebound]]`.

// RUN: %clang_cc1 -fsyntax-only -std=c17 \
// RUN:   -Wlifetime-safety-suggestions -Wlifetime-safety-noescape -verify %s

// Reporting control: the suggestion machinery is live in this file.
int *suggests(int *p) { // expected-warning {{parameter in intra-TU function should be marked [[clang::lifetimebound]]}}
  return p;             // expected-note {{param returned here}}
}

int *declared_noescape(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *declared_noescape(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;                      // expected-note {{param returned here}}
}
