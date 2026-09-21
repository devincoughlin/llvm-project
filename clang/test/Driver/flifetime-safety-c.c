/// -flifetime-safety-c is the default, so the driver forwards nothing to -cc1.
/// The ENABLED prefix opens with a positive match: on its own, the -NOT below
/// would pass against empty input and so would say nothing about forwarding.
// RUN: %clang -### -c %s 2>&1 | FileCheck --check-prefix=ENABLED %s
/// Spelling the default explicitly is still not forwarded.
// RUN: %clang -### -c %s -flifetime-safety-c 2>&1 | FileCheck --check-prefix=ENABLED %s
/// -fno-lifetime-safety-c loses to a later -flifetime-safety-c.
// RUN: %clang -### -c %s -fno-lifetime-safety-c -flifetime-safety-c 2>&1 | \
// RUN:   FileCheck --check-prefix=ENABLED %s
// ENABLED: "-cc1"
// ENABLED-NOT: "-fno-lifetime-safety-c"

/// The opt-out is forwarded to -cc1, on its own and as the last of a pair.
// RUN: %clang -### -c %s -fno-lifetime-safety-c 2>&1 | \
// RUN:   FileCheck --check-prefix=DISABLED %s
// RUN: %clang -### -c %s -flifetime-safety-c -fno-lifetime-safety-c 2>&1 | \
// RUN:   FileCheck --check-prefix=DISABLED %s
// DISABLED: "-cc1"
// DISABLED: "-fno-lifetime-safety-c"

/// Forwarding claims both spellings, so neither is reported unused. A lone
/// -NOT scans the whole output, including the driver diagnostics that precede
/// the job listing.
// RUN: %clang -### -c %s -flifetime-safety-c 2>&1 | FileCheck --check-prefix=CLAIMED %s
// RUN: %clang -### -c %s -fno-lifetime-safety-c 2>&1 | FileCheck --check-prefix=CLAIMED %s
// CLAIMED-NOT: argument unused during compilation
