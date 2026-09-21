// A coroutine's parameters are copied into the coroutine frame, which outlives
// the call. The classifier records the sink, and -- unlike every other file in
// this directory -- the extractor also clears `is_candidate`, so the fixpoint
// never sees the parameter at all.
//
// That is why this file asserts the TU summary as well as the verdict: an
// empty `non_escaping` is vacuous for a function with no candidates, so on its
// own it would be coverage this file does not have.
//
// The TU summary's id table is *not* sorted -- entity ids there follow
// extraction order, which moves with the driver flags (measured: `escape` is
// entity 0 under a bare `clang -cc1` and entity 2 under lit's `%clang_cc1`).
// So the TU assertions below are anchored on content and on exact counts
// rather than on an entity id. The link unit's ids, by contrast, are
// deterministic, which is why the WPA assertions elsewhere in this directory
// may name them -- and each of those binds its ids to USRs first.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} -std=c++20 -I %S/../../../../SemaCXX/Inputs %s \
// RUN:   --ssaf-tu-summary-file=%t/tu.json
// RUN: %{pe-link} %t/tu.json -o %t/lu.json
// RUN: %{pe-analyze} %t/lu.json -o %t/wpa.json

// `escape` is the only non-candidate and carries the only sink, so the two
// counts below pin the CHECK block to it without naming an entity id.
// RUN: grep '"is_candidate": false' %t/tu.json | count 1
// RUN: grep '"sink": {' %t/tu.json | count 1

// RUN: FileCheck %s --check-prefix=TU --input-file=%t/tu.json
// TU:      "sink": {
// TU-NEXT:   "detail": "",
// TU-NEXT:   "location": {
// TU-NEXT:     "column": 21,
// TU-NEXT:     "file": "{{.*}}coroutine.cpp",
// TU-NEXT:     "line": 55
// TU-NEXT:   },
// TU-NEXT:   "reason": "Coroutine"

// RUN: FileCheck %s --check-prefix=WPA --input-file=%t/wpa.json
// WPA:      "non_escaping": [],
// WPA-NEXT: "rejected": []

#include "std-coroutine.h"

struct Task {
  struct promise_type {
    Task get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
  };
};

Task escape(int *p) {
  (void)*p;
  co_return;
}

// No runtime driver. Both suspend points are `suspend_never`, so the coroutine
// runs to completion and destroys its frame before `escape` returns: as
// written, the parameter never outlives the call and there is nothing to
// observe. A coroutine that actually suspends would be a true escape, but it
// would be a different program from the one this file classifies.
// NO-DRIVER: suspend_never runs the frame to completion inside the call
