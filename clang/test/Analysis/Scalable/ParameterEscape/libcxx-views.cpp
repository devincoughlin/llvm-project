// What the analysis makes of real libc++ views, measured rather than assumed.
//
// The issue this file answers predicted that `std::span<int>(p, n).size()` and
// `*std::span<int>(p, n).data()` would be accepted and that a `push_back` and
// a stored `std::string_view` would be rejected. Measured against this SDK's
// libc++, *all four* are rejected, and for one reason: constructing a view
// from a pointer stores that pointer into the view's data member, which is an
// ordinary StoreToField in the constructor. The caller then sees
// EscapesViaCallee.
//
// That is the deferral of tracked views (#12, restored by #34) showing through
// at the only place a lit test can see it. Treating a view's construction as
// benign is exactly what the tracked-view model did, so when #34 lands this
// file is expected to change -- and it is here so that the change is visible
// rather than silent.
//
// Two consequences worth stating, because both were surprising:
//
//  * `span_size` is rejected although `.size()` never touches the pointer. The
//    rejection is the *construction*, not the use;
//  * `push` rejects its `std::vector &` parameter as well as the pointer,
//    blamed on `push_back`'s implicit object parameter (`"param": -1`).
//
// REQUIRES: system-darwin

// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -fsyntax-only -std=c++20 %s \
// RUN:   --ssaf-extract-summaries=ParameterEscape \
// RUN:   --ssaf-compilation-unit-id=cu --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json
// RUN: FileCheck %s --input-file=%t/wpa.json

// Exactly five rejections and no annotations, over a link unit of roughly a
// thousand entities -- nothing libc++ declares is provably non-escaping today
// either. If an SDK upgrade changes that, this line is the one that will say
// so; re-measure before relaxing it. `blame` is the discriminator because it
// is a key only a NonEscapingParametersResult rejection has: `reason` also
// appears in every ParameterEscapeResult sink in the same suite, 391 of them
// here, so counting those would say nothing.
// RUN: grep '"blame":' %t/wpa.json | count 5

// Entity ids are left as patterns because they index a libc++-sized table that
// an SDK upgrade renumbers. What identifies each rejection instead is its
// location, which is a call site in this file: distinct line and column for
// every one, and line != column throughout, so neither a transposition nor a
// swap between two rejections can pass. The order is the link unit's, which is
// USR order: push, span_data, span_size, store_sv.

// CHECK:      "non_escaping": [],
// CHECK-NEXT: "rejected": [

// `push`'s vector reference, blamed on push_back's implicit object parameter.
// CHECK-NEXT:   {
// CHECK-NEXT:     "blame": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": {{[0-9]+}}
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": -1
// CHECK-NEXT:     },
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 44,
// CHECK-NEXT:       "file": "{{.*}}libcxx-views.cpp",
// CHECK-NEXT:       "line": 176
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": [[PUSH:[0-9]+]]
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "EscapesViaCallee"
// CHECK-NEXT:   },

// `push`'s pointer, same node, next parameter.
// CHECK-NEXT:   {
// CHECK-NEXT:     "blame": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": {{[0-9]+}}
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 56,
// CHECK-NEXT:       "file": "{{.*}}libcxx-views.cpp",
// CHECK-NEXT:       "line": 176
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": [[PUSH]]
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 1
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "EscapesViaCallee"
// CHECK-NEXT:   },

// `span_data`: the span constructor, not `.data()`.
// CHECK-NEXT:   {
// CHECK-NEXT:     "blame": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": [[SPANCTOR:[0-9]+]]
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 55,
// CHECK-NEXT:       "file": "{{.*}}libcxx-views.cpp",
// CHECK-NEXT:       "line": 175
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": {{[0-9]+}}
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "EscapesViaCallee"
// CHECK-NEXT:   },

// `span_size`: the same constructor is to blame, which is what shows that the
// accessor is not what decides this.
// CHECK-NEXT:   {
// CHECK-NEXT:     "blame": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": [[SPANCTOR]]
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 59,
// CHECK-NEXT:       "file": "{{.*}}libcxx-views.cpp",
// CHECK-NEXT:       "line": 174
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": {{[0-9]+}}
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "EscapesViaCallee"
// CHECK-NEXT:   },

// `store_sv`: the string_view constructor.
// CHECK-NEXT:   {
// CHECK-NEXT:     "blame": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": {{[0-9]+}}
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "detail": "",
// CHECK-NEXT:     "location": {
// CHECK-NEXT:       "column": 67,
// CHECK-NEXT:       "file": "{{.*}}libcxx-views.cpp",
// CHECK-NEXT:       "line": 178
// CHECK-NEXT:     },
// CHECK-NEXT:     "node": {
// CHECK-NEXT:       "function": {
// CHECK-NEXT:         "@": {{[0-9]+}}
// CHECK-NEXT:       },
// CHECK-NEXT:       "param": 0
// CHECK-NEXT:     },
// CHECK-NEXT:     "reason": "EscapesViaCallee"
// CHECK-NEXT:   }
// CHECK-NEXT: ]

#include <span>
#include <string_view>
#include <vector>

int span_size(int *p, int n) { return (int)std::span<int>(p, n).size(); }
int span_data(int *p, int n) { return *std::span<int>(p, n).data(); }
void push(std::vector<int *> &v, int *p) { v.push_back(p); }
std::string_view gsv;
void store_sv(const char *p, unsigned n) { gsv = std::string_view(p, n); }
