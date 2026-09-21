// --ssaf-noescape-spelling= is a driver and a cc1 option. The value is free
// text -- it has to be, since the spelling may be a project macro such as
// NS_NOESCAPE -- so there is no value the option can reject. What must not
// happen is the value being dropped on the way to cc1 and the transformation
// silently falling back to the default spelling, and an option name that is
// not this one being accepted and ignored.

// DEFINE: %{filecheck} = FileCheck %s --check-prefix

// The driver forwards the value verbatim to cc1.
// RUN: %clang -### -fsyntax-only %s --ssaf-noescape-spelling=NS_NOESCAPE 2>&1 | %{filecheck}=FORWARDED
// FORWARDED: "-cc1"{{.+}}"--ssaf-noescape-spelling=NS_NOESCAPE"

// A spelling containing spaces and brackets survives argument round-tripping.
// RUN: %clang -### -fsyntax-only %s "--ssaf-noescape-spelling=[[clang::noescape]]" 2>&1 | %{filecheck}=FORWARDED-ATTR
// FORWARDED-ATTR: "-cc1"{{.+}}"--ssaf-noescape-spelling={{\[\[}}clang::noescape]]"

// An empty value is forwarded too, and means the default spelling rather than
// inserting nothing.
// RUN: %clang -### -fsyntax-only %s --ssaf-noescape-spelling= 2>&1 | %{filecheck}=EMPTY-VALUE
// EMPTY-VALUE: "-cc1"{{.+}}"--ssaf-noescape-spelling="

// The option is accepted by both the driver and cc1.
// RUN: %clang     -fsyntax-only %s --ssaf-noescape-spelling=NS_NOESCAPE
// RUN: %clang_cc1 -fsyntax-only %s --ssaf-noescape-spelling=NS_NOESCAPE

// A misspelled option name is rejected, not silently ignored and defaulted.
// RUN: not %clang     -fsyntax-only %s --ssaf-noescape-speling=X 2>&1 | %{filecheck}=UNKNOWN
// RUN: not %clang_cc1 -fsyntax-only %s --ssaf-noescape-speling=X 2>&1 | %{filecheck}=UNKNOWN
// UNKNOWN: error: {{unknown argument|unknown argument:}} '--ssaf-noescape-speling=X'

// The option is joined: the '=' is not optional, so a separate-argument
// spelling is not silently accepted either.
// RUN: not %clang     -fsyntax-only %s --ssaf-noescape-spelling X 2>&1 | %{filecheck}=UNKNOWN-SEPARATE
// UNKNOWN-SEPARATE: error: {{unknown argument|unknown argument:}} '--ssaf-noescape-spelling'

void empty(void) {}
