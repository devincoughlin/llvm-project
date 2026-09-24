extern int *sink;

// Annotated only by NoescapeDefn.apinotes / APINotes.apinotes. Nothing on this
// line says `noescape`, which is the whole point: without the note the warning
// would be unexplainable. API Notes attach the attribute to the definition's
// own ParmVarDecl here, because the definition is inside the header they
// annotate -- so a presence test on the definition's parameter suppresses the
// note, and only a written-ness test keeps it.
static inline int *defined_in_header(int *p) { // notes-warning {{parameter is marked [[clang::noescape]] but escapes}}
  sink = p;
  return p; // notes-note {{param returned here}}
  // notes-note@-3 {{'noescape' applies to this declaration without appearing in its source}}
}

// Reporting control: annotated in this header's own bytes, so it fires whether
// or not API Notes are read, and carries no origin note because the warning
// already points at the attribute.
static inline int *annotated_in_header(int *p __attribute__((noescape))) { // both-warning {{parameter is marked [[clang::noescape]] but escapes}}
  sink = p;
  return p; // both-note {{param returned here}}
}

// Negative control: no annotation from either source.
static inline int *unannotated_in_header(int *p) {
  sink = p;
  return p;
}
