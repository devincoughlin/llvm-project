// No instantiation of a function template is ever annotatable.
//
// An instantiation has no source of its own to edit -- the attribute would
// have to land on the pattern, where it would have to hold for every other
// instantiation too -- so the extractor clears `is_candidate` on all of them.
// The *caller* is still analyzed and still sees the escape through the
// instantiation's fact, which is the second half of what this file pins: the
// instantiation being un-annotatable must not also make it un-analyzable.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json

// Both instantiations are non-candidates and `use` is the only candidate.
// These are counts rather than a CHECK block because the TU summary's id table
// follows extraction order, not USR order, so an entity id means nothing here.
// RUN: grep '"is_candidate": false' %t/tu.json | count 2
// RUN: grep '"is_candidate": true' %t/tu.json | count 1

// The link unit's id assignment is deterministic, and each id is bound to a
// USR below, so the ids that follow name functions.
// `rejected` holding exactly `use`'s two parameters is the other half of the
// claim: were an instantiation a candidate it would be rejected here too,
// since it stores to a global.
// RUN: FileCheck %s --check-prefix=WPA --input-file=%t/wpa.json

// WPA:      "id": 0,
// WPA:        "usr": "c:@F@store<#C>#*C#"
// WPA:      "id": 1,
// WPA:        "usr": "c:@F@store<#I>#*I#"
// WPA:      "id": 2,
// WPA:        "usr": "c:@F@use#*I#*C#"
// WPA:      "non_escaping": [],
// WPA-NEXT: "rejected": [
// WPA-NEXT:   {
// WPA-NEXT:     "blame": {
// WPA-NEXT:       "function": {
// WPA-NEXT:         "@": 1
// WPA-NEXT:       },
// WPA-NEXT:       "param": 0
// WPA-NEXT:     },
// WPA-NEXT:     "detail": "",
// WPA-NEXT:     "location": {
// WPA-NEXT:       "column": 35,
// WPA-NEXT:       "file": "{{.*}}template-instantiation.cpp",
// WPA-NEXT:       "line": 84
// WPA-NEXT:     },
// WPA-NEXT:     "node": {
// WPA-NEXT:       "function": {
// WPA-NEXT:         "@": 2
// WPA-NEXT:       },
// WPA-NEXT:       "param": 0
// WPA-NEXT:     },
// WPA-NEXT:     "reason": "EscapesViaCallee"
// WPA-NEXT:   },
// WPA-NEXT:   {
// WPA-NEXT:     "blame": {
// WPA-NEXT:       "function": {
// WPA-NEXT:         "@": 0
// WPA-NEXT:       },
// WPA-NEXT:       "param": 0
// WPA-NEXT:     },
// WPA-NEXT:     "detail": "",
// WPA-NEXT:     "location": {
// WPA-NEXT:       "column": 45,
// WPA-NEXT:       "file": "{{.*}}template-instantiation.cpp",
// WPA-NEXT:       "line": 84
// WPA-NEXT:     },
// WPA-NEXT:     "node": {
// WPA-NEXT:       "function": {
// WPA-NEXT:         "@": 2
// WPA-NEXT:       },
// WPA-NEXT:       "param": 1
// WPA-NEXT:     },
// WPA-NEXT:     "reason": "EscapesViaCallee"
// WPA-NEXT:   }
// WPA-NEXT: ]

void *sink;

template <class T> void store(T *p) { sink = (void *)p; }

void use(int *a, char *b) { store(a); store(b); }

// Runtime half; see store-to-global.cpp for what a driver is for. `use` calls
// `store` twice and the second call wins, so the object the driver reads back
// is the `char` -- which is why it, and not the `int`, is the one named
// `local`.
//
// So the driver observes **one** of the two rejections asserted above: `b`,
// through the `store<char>` instantiation. `a`'s escape through `store<int>`
// is the same shape and is not separately demonstrated -- a single global sink
// can only hold one of them, and reordering the calls would just swap which.
// The ASan report is on a 1-byte object, which is how to tell which one it is.
// DRIVER: int main() {
// DRIVER:   { int first = 1; char local = 'x'; use(&first, &local); }
// DRIVER:   return *(char *)sink;
// DRIVER: }
