// API Notes can put `noescape` on a *definition's* parameter, and the note that
// explains the warning has to survive that.
//
// When the definition lives inside a header that API Notes annotate -- a
// `static inline` helper, the ordinary shape in C -- the attribute is attached
// to the definition's own `ParmVarDecl`, with no source location. Suppressing
// the origin note whenever the parameter merely *has* the attribute therefore
// withholds it exactly where it is needed: the warning lands on a line that
// spells no annotation. `findNoEscapeAnnotation` suppresses it only when the
// attribute is *written* there, which is what these runs pin.
//
// All expectations live in the header, because the second RUN line compiles the
// module map rather than this file. This file only provides a use of the header.

// 1. Non-modules API Notes, discovered as APINotes.apinotes in the header's own
//    directory. No module map and no module cache are involved.
// RUN: %clang_cc1 -fsyntax-only -std=c17 \
// RUN:   -Wlifetime-safety-noescape -fapinotes \
// RUN:   -I %S/Inputs/apinotes-definition -verify=both,notes %s

// 2. The module-building path, where the notes are named after the module and
//    the header is analyzed as the module is compiled.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang_cc1 -emit-module -std=c17 \
// RUN:   -Wlifetime-safety-noescape -fmodules -fmodule-name=NoescapeDefn \
// RUN:   -fapinotes-modules -fmodules-cache-path=%t/mc \
// RUN:   -I %S/Inputs/apinotes-definition -verify=both,notes \
// RUN:   -x c %S/Inputs/apinotes-definition/module.modulemap -o %t/out.pcm

// 3. Control: the same header with API Notes off. Only the annotation written in
//    the header's bytes is left, so `defined_in_header` becomes indistinguish-
//    able from `unannotated_in_header`. Without this run both runs above could
//    pass with the .apinotes file never being read.
// RUN: %clang_cc1 -fsyntax-only -std=c17 \
// RUN:   -Wlifetime-safety-noescape \
// RUN:   -I %S/Inputs/apinotes-definition -verify=both %s

#include "NoescapeDefn.h"

int *use(int *q) { return defined_in_header(q); }
