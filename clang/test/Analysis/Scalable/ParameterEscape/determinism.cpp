// Repeated runs of the pipeline agree, byte for byte, from the link unit
// onward.
//
// The issue this file answers asked for "two extractions and two analyses are
// byte-identical". Measured, the first half of that is false today and the
// second half is true:
//
//   * two `clang` extractions of this file produce *different* TU summaries --
//     7 distinct outputs over 8 runs. The id table is listed in USR order in
//     every run, but the id *assignment* is not: `forward_one` was measured as
//     id 4 in one run and id 6 in the next, and `data`'s `entity_id` and
//     `callee` references follow. It is not specific to ParameterEscape --
//     extracting `PointerFlow` and `CallGraph` from this same file is equally
//     unstable -- so it is a property of the extraction framework's entity
//     numbering, not of this analysis;
//   * `clang-ssaf-linker` renumbers by USR, so the link unit is stable: 8
//     identical outputs over the same 8 runs, and so is every WPA suite
//     computed from it.
//
// So what is asserted here is the determinism the pipeline actually has, and
// it is asserted end to end: two independent extractions, linked separately,
// must reach the same link unit and the same verdict. That is the property a
// consumer depends on -- the annotation a build produces must not depend on
// which run extracted it -- and it holds *through* the unstable stage rather
// than around it.
//
// The extractor's instability is tracked separately; do not "fix" this file by
// diffing %t/tu-1.json against %t/tu-2.json. That comparison fails, and it
// fails intermittently rather than always, since two runs can agree by chance.
//
// The analysis walks hash maps keyed by entity id and by node, so the input is
// deliberately wide: enough entities, edges and sinks that an unordered
// container has something to reorder.
//
// `diff` of two empty files also passes, so the counts run first -- they are
// what makes this test non-vacuous. Each stage writes to a fresh path because
// --ssaf-tu-summary-file refuses to overwrite: it reports the failure on
// stderr and leaves the previous run's bytes in place, so a determinism test
// that reused one path would be comparing a file with itself.
//
// Both link units are written to the same basename in different directories,
// because the link-unit namespace name is derived from the output file's stem
// and would otherwise differ for that reason alone.

// RUN: rm -rf %t && mkdir -p %t/one %t/two

// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/one/tu.json
// RUN: %{pe-extract} %s --ssaf-tu-summary-file=%t/two/tu.json
// RUN: grep '"entity_id":' %t/one/tu.json | count 8
// RUN: grep '"reason":' %t/one/tu.json | count 2
// RUN: grep '"callee":' %t/one/tu.json | count 3

// RUN: %{pe-link} %t/one/tu.json -o %t/one/lu.json
// RUN: %{pe-link} %t/two/tu.json -o %t/two/lu.json
// RUN: diff %t/one/lu.json %t/two/lu.json

// RUN: %{pe-analyze} %t/one/lu.json -o %t/one/wpa.json
// RUN: %{pe-analyze} %t/two/lu.json -o %t/two/wpa.json
// RUN: grep '"reason":' %t/one/wpa.json | count 8
// RUN: grep '"params":' %t/one/wpa.json | count 2
// RUN: diff %t/one/wpa.json %t/two/wpa.json

// And re-analyzing the *same* link unit is byte-identical too, which is the
// narrower claim the fixpoint's own tool-level test makes.
// RUN: %{pe-analyze} %t/one/lu.json -o %t/one/wpa-again.json
// RUN: diff %t/one/wpa.json %t/one/wpa-again.json

int *g1;
int *g2;

void unanalyzed(int *);

void sink_global_one(int *p) { g1 = p; }
void sink_global_two(int *p) { g2 = p; }

void forward_one(int *p) { sink_global_one(p); }
void forward_two(int *p) { sink_global_two(p); }

void forward_unanalyzed(int *p) { unanalyzed(p); }

int *returns(int *p) { return p; }

void clean_one(int *p) { (void)*p; }
void clean_two(int *p, int *q) { (void)*p, (void)*q; }
