// A byte-level snapshot of the ParameterEscape TU summary: every key the
// writer emits, asserted against a literal.
//
// A round trip through the reader cannot see a writer that swapped two
// same-typed fields, because it is closed under any transform applied to both
// sides. So this file spells the expected bytes out instead. Three rules make
// that closed, and all three are load-bearing here:
//
//  * every leaf gets its own assertion, and the leaf list comes from the
//    writer (ParameterEscapeFormat.cpp's key constants), not from a fixture;
//  * each leaf is enumerated by *path*, so the three `location` occurrences
//    and the two `flows_to` occurrences each get their own;
//  * the literals differ across paths -- four distinct source locations, with
//    line != column in each, so that a transposition cannot hide.
//
// The TU summary's id table is in extraction order, not USR order, so nothing
// here hardcodes an id. `entity_id` and each `callee` are captured into
// FileCheck variables and resolved against the id table further down the same
// file, which is what makes `"@": 0` mean `external` rather than a number.
//
// One TU-summary key is absent by construction: `is_candidate` is only ever
// false for a shape the fixpoint must not see, which is what
// Negative/coroutine.cpp and Negative/template-instantiation.cpp pin.

// RUN: rm -rf %t && mkdir -p %t
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/tu.json
// RUN: FileCheck %s --input-file=%t/tu.json

// `record` is the only definition in the file, so the summary array holds
// exactly one entry and the trailing `]` below pins that.
// RUN: grep '"entity_id":' %t/tu.json | count 1

// CHECK:      "summary_data": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "entity_id": {{[0-9]+}},
// CHECK-NEXT:     "entity_summary": {
// CHECK-NEXT:       "candidate_params": [
// CHECK-NEXT:         0,
// CHECK-NEXT:         1,
// CHECK-NEXT:         2
// CHECK-NEXT:       ],
// CHECK-NEXT:       "is_candidate": true,
// CHECK-NEXT:       "params": [

// Parameter 0 is returned. `returns_self_at` is the return statement, and the
// parameter has no flow edges of its own.
// CHECK-NEXT:         {
// CHECK-NEXT:           "flows_to": [],
// CHECK-NEXT:           "index": 0,
// CHECK-NEXT:           "returns_self_at": {
// CHECK-NEXT:             "column": 5,
// CHECK-NEXT:             "file": "{{.*}}extraction.cpp",
// CHECK-NEXT:             "line": 153
// CHECK-NEXT:           }
// CHECK-NEXT:         },

// Parameter 1 flows into parameter 0 of `external` at the call site. No sink:
// what happens to it is the callee's summary to say.
// CHECK-NEXT:         {
// CHECK-NEXT:           "flows_to": [
// CHECK-NEXT:             {
// CHECK-NEXT:               "callee": {
// CHECK-NEXT:                 "@": {{[0-9]+}}
// CHECK-NEXT:               },
// CHECK-NEXT:               "location": {
// CHECK-NEXT:                 "column": 14,
// CHECK-NEXT:                 "file": "{{.*}}extraction.cpp",
// CHECK-NEXT:                 "line": 150
// CHECK-NEXT:               },
// CHECK-NEXT:               "param": 0
// CHECK-NEXT:             }
// CHECK-NEXT:           ],
// CHECK-NEXT:           "index": 1
// CHECK-NEXT:         },

// Parameter 2 sinks. `detail` carries the global's name.
// CHECK-NEXT:         {
// CHECK-NEXT:           "flows_to": [],
// CHECK-NEXT:           "index": 2,
// CHECK-NEXT:           "sink": {
// CHECK-NEXT:             "detail": "g",
// CHECK-NEXT:             "location": {
// CHECK-NEXT:               "column": 9,
// CHECK-NEXT:               "file": "{{.*}}extraction.cpp",
// CHECK-NEXT:               "line": 152
// CHECK-NEXT:             },
// CHECK-NEXT:             "reason": "StoreToGlobal"
// CHECK-NEXT:           }
// CHECK-NEXT:         }
// CHECK-NEXT:       ],

// The implicit object parameter gets a node of its own, with the same shape a
// named parameter's has.
// CHECK-NEXT:       "this": {
// CHECK-NEXT:         "flows_to": [
// CHECK-NEXT:           {
// CHECK-NEXT:             "callee": {
// CHECK-NEXT:               "@": {{[0-9]+}}
// CHECK-NEXT:             },
// CHECK-NEXT:             "location": {
// CHECK-NEXT:               "column": 13,
// CHECK-NEXT:               "file": "{{.*}}extraction.cpp",
// CHECK-NEXT:               "line": 151
// CHECK-NEXT:             },
// CHECK-NEXT:             "param": 0
// CHECK-NEXT:           }
// CHECK-NEXT:         ]
// CHECK-NEXT:       }
// CHECK-NEXT:     }
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK-NEXT: "summary_name": "ParameterEscape"

// The three captured ids resolve against the id table. Each resolution is its
// own FileCheck pass, because the id table is in extraction order and a
// CHECK-DAG pair would match an id from one entry against a usr from another
// -- exactly the swap this is here to catch.

// RUN: FileCheck %s --check-prefix=ID-RECORD --input-file=%t/tu.json
// ID-RECORD:      "entity_id": [[R:[0-9]+]],
// ID-RECORD:      "id": [[R]],
// ID-RECORD-NEXT: "name": {
// ID-RECORD-NEXT:   "suffix": "",
// ID-RECORD-NEXT:   "usr": "c:@S@Snapshot@F@record#*I#S0_#S0_#"

// The first "@" in the summary is parameter 1's callee.
// RUN: FileCheck %s --check-prefix=ID-EXTERNAL --input-file=%t/tu.json
// ID-EXTERNAL:      "@": [[E:[0-9]+]]
// ID-EXTERNAL:      "id": [[E]],
// ID-EXTERNAL-NEXT: "name": {
// ID-EXTERNAL-NEXT:   "suffix": "",
// ID-EXTERNAL-NEXT:   "usr": "c:@F@external#*I#"

// The one under "this" is the second.
// RUN: FileCheck %s --check-prefix=ID-OBSERVE --input-file=%t/tu.json
// ID-OBSERVE:      "this": {
// ID-OBSERVE:      "@": [[O:[0-9]+]]
// ID-OBSERVE:      "id": [[O]],
// ID-OBSERVE-NEXT: "name": {
// ID-OBSERVE-NEXT:   "suffix": "",
// ID-OBSERVE-NEXT:   "usr": "c:@F@observe#*$@S@Snapshot#"

int *g;
struct Snapshot;
void external(int *q);
void observe(Snapshot *s);

struct Snapshot {
  int *record(int *returned, int *flowing, int *sunk) {
    external(flowing);
    observe(this);
    g = sunk;
    return returned;
  }
};
