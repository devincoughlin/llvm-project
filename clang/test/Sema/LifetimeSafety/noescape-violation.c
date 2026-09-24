// RUN: %clang_cc1 -fsyntax-only -std=c17 -fblocks -Wlifetime-safety-noescape -verify %s
// RUN: %clang_cc1 -fsyntax-only -std=c23 -fblocks -DCHECK_C23 -Wlifetime-safety-noescape -verify %s

// The C sibling of noescape-violation.cpp. Neither RUN line passes
// -fexperimental-lifetime-safety-c, because the analysis is on by default in C
// now; nor -flifetime-safety-inference, which the .cpp RUN line passes but
// which explicit-noescape violations do not need.

#define NOESCAPE __attribute__((noescape))

// The C23 RUN line is pinned from both sides. This half catches -DCHECK_C23
// being dropped, which would otherwise take the C23 block and its expected-
// directives out of the file silently; the half inside the block below
// catches -std=c23 being dropped.
#if __STDC_VERSION__ >= 202311L && !defined(CHECK_C23)
#error "the C23 RUN line must define CHECK_C23"
#endif

//===----------------------------------------------------------------------===//
// Positives.
//===----------------------------------------------------------------------===//

// Direct return, attribute written before the declarator.
int *return_directly(NOESCAPE int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p; // expected-note {{returned here}}
}

// GNU post-declarator attribute placement, the spelling K&R-era C code uses.
int *return_post_declarator(int *p NOESCAPE) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p; // expected-note {{returned here}}
}

int *global_ptr; // expected-note {{escapes to this global storage}}

void escape_to_global(NOESCAPE int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  global_ptr = p;
}

// A cast to void * does not break the flow.
void *return_through_void_cast(NOESCAPE int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return (void *)p; // expected-note {{returned here}}
}

// A GNU statement expression does not break the flow either.
int *return_through_statement_expr(NOESCAPE int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return ({ p; }); // expected-note {{returned here}}
}

_Atomic(int *) atomic_global; // expected-note {{escapes to this global storage}}

void escape_through_atomic_store(NOESCAPE int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  atomic_global = p;
}

// restrict is a C-only qualifier on the escaping parameter.
int *return_restrict(int *restrict p NOESCAPE) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p; // expected-note {{returned here}}
}

#ifdef CHECK_C23
// The [[]] spelling needs C23. Guard the RUN line rather than letting a
// mutation of -std=c23 silently skip this block: a skipped block would take
// its expected- directives with it and still pass.
#if __STDC_VERSION__ < 202311L
#error "the CHECK_C23 RUN line must be compiled with -std=c23"
#endif

int *return_c23_bracket_spelling(int *p [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p; // expected-note {{returned here}}
}

int *c23_global; // expected-note {{escapes to this global storage}}

void escape_to_global_c23_spelling(int *p [[clang::noescape]]) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  c23_global = p;
}
#endif

//===----------------------------------------------------------------------===//
// Negatives. Pinned so that a later change which starts diagnosing them is
// visible here rather than silent.
//===----------------------------------------------------------------------===//

struct S {
  int *f;
};

struct S struct_global;

// Escape through a field of a global. Not diagnosed today.
void escape_to_global_field(NOESCAPE int *p) { // no-warning
  struct_global.f = p;
}

// Escape through an out-parameter. Not diagnosed today; the .cpp sibling
// records the same gap as "escaping through another param is not detected".
void escape_through_out_param(NOESCAPE int *p, int **out) { // no-warning
  *out = p;
}

int *global_array[4];

// Escape into an element of a global array. Not diagnosed today.
void escape_to_global_array(NOESCAPE int *p) { // no-warning
  global_array[0] = p;
}

long global_as_integer;

// Laundering the pointer through an integer. Not diagnosed today.
void escape_as_integer(NOESCAPE int *p) { // no-warning
  global_as_integer = (long)p;
}

void (^global_block)(void);

// Capture by a block that outlives the call. Not diagnosed today.
void escape_through_block_capture(NOESCAPE int *p) { // no-warning
  global_block = ^{ (void)p; };
}

int *compound_literal_global; // no-warning

// A real escape, laundered through a C compound literal. Compound literals are
// not modelled yet (that is issue #52), so this is silent -- while the same
// assignment written directly, in escape_to_global above, warns. That pairing
// is what makes this negative track the modelling gap rather than a use that is
// simply safe.
void escape_through_compound_literal(NOESCAPE int *p) { // no-warning
  compound_literal_global = (int *){p};
}

// The plain parameter, with no annotation, is never diagnosed.
int *return_unannotated(int *p) { // no-warning
  return p;
}

// Local use only.
void use_locally(NOESCAPE int *p) { // no-warning
  *p = 42;
}
