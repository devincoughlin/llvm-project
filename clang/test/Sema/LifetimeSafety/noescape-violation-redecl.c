// The `noescape` audit resolves the attribute across a function's
// redeclarations, not just on the definition.
//
// `NoEscape` is a plain `Attr`, not an `InheritableParamAttr`, so
// `mergeParamDeclAttributes` never copies it onto a later declaration, and
// `ASTContext::mergeExtParameterInfo` AND-merges the `FunctionProtoType`'s bit
// so the definition's merged type has lost it too. In C that combination is
// silent -- the annotation is written, the compiler discards it, and reading
// only the definition's `ParmVarDecl` audits nothing. C++ never reaches this
// state: a mismatched redeclaration is `error: conflicting types` in both
// directions. See `findNoEscapeAnnotation` in
// clang/lib/Analysis/LifetimeSafety/Checker.cpp.
//
// `-Wno-deprecated-non-prototype` is only to keep the unprototyped
// declarations below from adding diagnostics unrelated to what is under test.

// RUN: %clang_cc1 -fsyntax-only -std=c17 \
// RUN:   -Wlifetime-safety-noescape -Wno-deprecated-non-prototype -verify %s

// Reporting control. Annotated on the definition, which already warned before
// this change: if this stops firing, the file is measuring nothing.
int *control_definition_annotated(int *p __attribute__((noescape))) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p; // expected-note {{param returned here}}
}

// Negative control: nothing in the chain is annotated, so nothing is audited.
int *never_annotated(int *p);
int *never_annotated(int *p) { return p; }

// ---------------------------------------------------------------------------
// The case this change is about: the annotation is written only on a
// declaration. Before this change there was no diagnostic here at all.
// ---------------------------------------------------------------------------
int *declaration_annotated(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *declaration_annotated(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;                          // expected-note {{param returned here}}
}

// A three-declaration chain where *only the middle* declaration is annotated.
// This is what distinguishes "any redeclaration" from the alternatives: the
// canonical (first) declaration is unannotated, so a canonical-only lookup
// would stay silent, and so would a walk that stops at the first declaration
// it reaches from either end.
int *middle_of_three(int *p);
int *middle_of_three(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *middle_of_three(int *p);
int *middle_of_three(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;                    // expected-note {{param returned here}}
}

// The mirror image: only the last declaration before the definition carries it.
int *last_of_three(int *p);
int *last_of_three(int *p);
int *last_of_three(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *last_of_three(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;                  // expected-note {{param returned here}}
}

// ---------------------------------------------------------------------------
// Parameter identity is by index. Both directions are pinned, so neither an
// index-insensitive walk nor one that transposes two parameters passes.
// ---------------------------------------------------------------------------

// The attribute is on index 1 and index 1 escapes: diagnosed, on `b`.
int *annotated_index_one(int *a, int *b __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *annotated_index_one(int *a,
                         int *b) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  (void)a;
  return b; // expected-note {{param returned here}}
}

// The attribute is on index 0 and index 1 escapes: not diagnosed. An
// index-insensitive lookup would warn here.
int *annotated_index_zero(int *a __attribute__((noescape)), int *b);
int *annotated_index_zero(int *a, int *b) {
  (void)a;
  return b;
}

// ---------------------------------------------------------------------------
// Shapes where a redeclaration's parameter list does not line up with the
// definition's, which is why the index walk needs a bound check.
// ---------------------------------------------------------------------------

// An unprototyped declaration that comes *first* keeps the type `int *()` and
// is given no `ParmVarDecl` at all, so index 0 is out of range for it. The
// later prototype still carries the attribute and is still found.
int *knr_first();
int *knr_first(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *knr_first(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;              // expected-note {{param returned here}}
}

// The same shape with nothing annotated anywhere. This, and not the annotated
// case above, is what pins the bound check: measured by deleting the check and
// rebuilding, the annotated chain returns true before the walk ever reaches the
// zero-parameter declaration, while this one runs to the end and crashes with
// `Assertion failed: (i < getNumParams() && "Illegal param #")` in
// `FunctionDecl::getParamDecl`. An out-of-range declaration must also not make
// an unannotated parameter look annotated.
int *knr_first_unannotated();
int *knr_first_unannotated(int *p);
int *knr_first_unannotated(int *p) { return p; }

// An unprototyped declaration that comes *after* a prototype is a different
// shape: it composes with the prototype's type and is given a synthesized
// implicit `ParmVarDecl`, which carries no attribute of its own. The attribute
// still has to be found on the real prototype.
int *knr_after_prototype(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
int *knr_after_prototype();
int *knr_after_prototype(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;                        // expected-note {{param returned here}}
}

// A variadic tail: the named parameters still match by index.
int *variadic_annotated(int *p __attribute__((noescape)), ...); // expected-note {{'noescape' attribute appears here}}
int *variadic_annotated(int *p, ...) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;                            // expected-note {{param returned here}}
}

int *variadic_unannotated(int *p, ...);
int *variadic_unannotated(int *p, ...) { return p; }

// ---------------------------------------------------------------------------
// Escape targets other than `return`, to show the resolution happens in
// `checkAnnotations` and so covers every escape kind.
// ---------------------------------------------------------------------------
int *global_sink;
// One note per violation that escapes here: escapes_to_global, typedef_one
// and typedef_two.
// expected-note@-3 3 {{escapes to this global storage}}

void escapes_to_global(int *p __attribute__((noescape))); // expected-note {{'noescape' attribute appears here}}
void escapes_to_global(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  global_sink = p;
}

// Annotated on a declaration but never escaping: stays quiet.
void declared_noescape_used_locally(int *p __attribute__((noescape)));
void declared_noescape_used_locally(int *p) { *p = 42; }

// ---------------------------------------------------------------------------
// Declared through a typedef of function type. Such a declaration's
// `ParmVarDecl`s are synthesized and carry no attribute, so the annotation
// survives only as the `ExtParameterInfo` bit in the declaration's
// `FunctionProtoType`. Walking the redeclarations' attributes alone misses it,
// while `EscapeClassifier::paramIsDeclaredNoescape` consults the type and
// therefore trusts it -- which is the gap the type fallback closes.
//
// The note points at the declaration rather than at an attribute, because the
// declaration does not spell one.
// ---------------------------------------------------------------------------
typedef void OneParam(int *__attribute__((noescape)));
OneParam typedef_one; // expected-note {{'noescape' applies to this declaration without appearing in its source}}
void typedef_one(int *p) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  global_sink = p;
}

// Control: the same shape with no annotation in the typedef.
typedef void OneParamPlain(int *);
OneParamPlain typedef_one_plain;
void typedef_one_plain(int *p) { global_sink = p; }

// Two parameters, annotated on index 1 only, so the type fallback has to respect
// the index just as the attribute walk does.
typedef void TwoParams(int *, int *__attribute__((noescape)));
TwoParams typedef_two; // expected-note {{'noescape' applies to this declaration without appearing in its source}}
void typedef_two(int *a,
                 int *b) { // expected-warning {{parameter is marked [[clang::noescape]] but escapes}}
  (void)a;
  global_sink = b;
}

// The mirror: annotated on index 0 while index 1 escapes, so nothing is
// reported. An index-insensitive type fallback would warn here.
typedef void TwoParamsFirst(int *__attribute__((noescape)), int *);
TwoParamsFirst typedef_two_first;
void typedef_two_first(int *a, int *b) {
  (void)a;
  global_sink = b;
}
