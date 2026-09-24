// The `noescape` audit sees an attribute that API Notes put on the header
// declaration.
//
// This is the mechanism the redeclaration walk matters most for: API Notes
// attach `NoEscapeAttr` to the *declaration* in the imported header
// (`SemaAPINotes.cpp`), never to the definition, and there are no bytes in this
// file that spell the attribute. Reading only the definition's `ParmVarDecl`
// leaves every API-Notes annotation unaudited.

// RUN: rm -rf %t && mkdir -p %t

// API Notes on.
// RUN: %clang_cc1 -fsyntax-only -std=c17 \
// RUN:   -Wlifetime-safety-noescape -fmodules -fimplicit-module-maps \
// RUN:   -fmodules-cache-path=%t/on -fapinotes-modules \
// RUN:   -I %S/Inputs/apinotes -verify=common,notes %s

// Control: the same compilation with API Notes off. The attribute is now
// nowhere, so `audited` becomes indistinguishable from `unaudited` and only the
// in-file control still fires. Without this run the test could pass with the
// .apinotes file never being read.
// RUN: %clang_cc1 -fsyntax-only -std=c17 \
// RUN:   -Wlifetime-safety-noescape -fmodules -fimplicit-module-maps \
// RUN:   -fmodules-cache-path=%t/off \
// RUN:   -I %S/Inputs/apinotes -verify=common %s

#include "NoescapeAudit.h"

// Reporting control: annotated in this file, so it fires in both runs. The
// attribute is on this very parameter, so the warning carries no note pointing
// elsewhere.
int *control(int *p __attribute__((noescape))) { // common-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;                                     // common-note {{param returned here}}
}

// `audited`'s only annotation is the NoEscape entry in NoescapeAudit.apinotes.
// API Notes build the attribute with no source location, so the note names the
// header declaration it was applied to rather than pointing at an attribute.
// Without that note the warning would land on a line that mentions nothing.
int *audited(int *p) { // notes-warning {{parameter is marked [[clang::noescape]] but escapes}}
  return p;            // notes-note {{param returned here}}
}
// notes-note@Inputs/apinotes/NoescapeAudit.h:1 {{'noescape' applies to this declaration without appearing in its source}}

// Declared identically in the same header, with no API Notes entry.
int *unaudited(int *p) { return p; }
