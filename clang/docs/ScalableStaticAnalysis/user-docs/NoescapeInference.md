# Noescape Inference

Noescape inference proves, across translation units, that a function never
lets a pointer parameter outlive the call, and writes `noescape` on every
declaration of that parameter. The attribute is what the existing
`-Wlifetime-safety-noescape` analysis, and the optimizer's `captures(none)`,
already want and cannot derive on their own.

Everything below is measured against the implementation rather than taken from
the design document; where the two differ, this file describes what the tools
do.

## What is inferred

A parameter is annotated when the whole-program fixpoint proves that no
execution of the program lets its pointer outlive the call. The proof is
conservative in one direction only: anything the analysis cannot model is an
escape, so a missing annotation is normal and a wrong annotation is a defect.

Two populations matter and they are not the same. A parameter is **analyzed**
when facts are extracted for it, so that what happens to it can reject a
*caller*; it is a **candidate** when an attribute may be written on it.

| parameter shape | analyzed | candidate |
| --- | --- | --- |
| object pointer, or reference, to a non-view type | yes | yes |
| view (`[[gsl::Pointer]]`, `swift_attr("~Escapable")`) by value, or reference to a view | no | no |
| pointer to a view | yes | no |
| function reference, block pointer, Objective-C object pointer | yes | no |
| function pointer | no | no |

Tracked views are deferred; see *Precision* below. Callables — function
pointers, blocks, lambdas, `std::function` — are a later milestone, as are
virtual and Objective-C dispatch.

### Definitions that are refused as a whole

Independently of its parameters, a function definition is outside the candidate
population entirely when any of the following holds. A function refused here is
in *neither* list of the report: it produces no annotation and no rejection, so
its absence is silent.

- **Anything templated.** No function template, no instantiation of one, no
  explicit or member specialization, and nothing lexically inside a dependent
  context is ever annotated. For modern C++ this is the single largest
  practical limitation, and it is deliberate: an instantiation shares the
  pattern's source text, so one instantiation's verdict must never be allowed
  to edit the template — a second instantiation may well escape. Measured:

  ```c++
  template <class T> void reads(T *p) { (void)*p; }
  void plain(int *p) { (void)*p; }
  ```

  `plain`'s parameter is annotated; `reads` appears in neither `non_escaping`
  nor `rejected`.
- `main`, which the runtime calls.
- A **virtual** member function.
- A definition that **may not be the one that runs**: multiversioned, weak, a
  weakref, or `available_externally` (the `extern __inline__` glibc pattern,
  and C99 `inline` without an external declaration).
- A **naked** function, whose parameters are never materialized.
- A **coroutine**.
- Any redeclaration **without a written prototype** (a K&R identifier list),
  or whose location is **a macro expansion** or **in a system header**.
- Any candidate parameter with no written, non-macro source location to edit —
  including a parameter clang synthesized for a declaration written through a
  function typedef.

## Pipeline

Six commands. The first and fourth run alongside an ordinary compile, once per
translation unit; the middle two are whole-program steps; the last two apply
the result.

```console
$ # 1. Extract per-TU facts. One summary per translation unit.
$ clang -fsyntax-only a.cpp \
    --ssaf-extract-summaries=ParameterEscape \
    --ssaf-compilation-unit-id=a \
    --ssaf-tu-summary-file=out/a.tu.json

$ # 2. Link the summaries into one link unit.
$ clang-ssaf-linker out/a.tu.json out/b.tu.json -o out/lu.json

$ # 3. Run the whole-program fixpoint.
$ clang-ssaf-analyzer out/lu.json \
    -a NonEscapingParametersResult -o out/wpa.json

$ # 4. Generate edits, again once per translation unit.
$ clang -fsyntax-only a.cpp \
    --ssaf-source-transformation=infer-noescape \
    --ssaf-global-scope-analysis-result=out/wpa.json \
    --ssaf-compilation-unit-id=a --ssaf-link-unit-id=lu \
    --ssaf-src-edit-file=out/a.edits.yaml \
    --ssaf-transformation-report-file=out/a.report.sarif

$ # 5. Merge the per-TU edits, deduplicating edits to shared headers.
$ clang-ssaf-src-edit-merge out/a.edits.yaml out/b.edits.yaml \
    -o out/apply/merged.yaml

$ # 6. Apply.
$ clang-apply-replacements out/apply
```

The compilation-unit identifier passed in step 4 must be the one passed in
step 1, and the link-unit identifier must name the link unit produced in step
2.

`clang-apply-replacements` reads *every* file in the directory it is given, so
the per-TU YAMLs must stay outside the apply directory or the merge is applied
a second time on top of itself.

The inserted text defaults to `__attribute__((noescape))` and is chosen with
`--ssaf-noescape-spelling=`. The default is deliberately the same in every
language, so that a header shared by C and C++ translation units receives
byte-identical insertions that step 5 can deduplicate. A project that wants
`NS_NOESCAPE`, or a macro of its own, passes it here — and must pass the same
spelling for every translation unit sharing a header, for the same reason.

The merge keys each edit on the canonical absolute path of the file it edits,
not on the path as the compiler spelled it, so a header one translation unit
reached through a quoted relative include and another through `-I` is still
recognized as one file and its edits collapse to one. The merged YAML carries
those absolute paths, and is meaningful from any working directory.

The pipeline is idempotent: a second full run over the rewritten tree proposes
no edits, which is what makes it safe to run inside a build.

## What is trusted

Nothing outside the link unit is believed except two things:

- **`noescape` written by a human**, in source or through API Notes, on a
  declaration of a callee parameter. This is a *claim*, and it is believed
  even when the link unit contains a body that contradicts it. Measured:

  ```c++
  int *g;
  void liar(__attribute__((noescape)) int *q);
  void liar(__attribute__((noescape)) int *q) { g = q; }
  void caller(int *p) { liar(p); }
  ```

  The result rejects `liar`'s own parameter (`StoreToGlobal`) **and annotates
  `caller`'s** in the same run, because the call site trusts the attribute
  rather than the analyzed body. Delete the attribute and `caller` becomes
  `EscapesViaCallee`.

  So a wrong `noescape` anywhere in your headers can produce a wrong
  annotation somewhere else. Before trusting existing annotations, build the
  tree with `-Werror -Wlifetime-safety-noescape`, which is an independent
  analysis that checks bodies against the attributes on them.
- **A table of C library functions**, matched by name and arity against a
  bodiless callee that clang models as a builtin. A row grants a parameter
  benign status only where LLVM's own libcall inference
  (`inferNonMandatoryLibFuncAttrs`) gives it `captures(none)`, and a unit test
  proves every row against LLVM so the table cannot drift. Deallocation and
  reallocation functions are listed in order to be *refused*, because
  `noescape` forbids freeing through the parameter too.

Everything else escapes. A callee with no body and no trusted row rejects its
arguments (`UnnamedCallee`); a callee whose body is in another link unit
rejects them too (`UnanalyzedCallee`).

## Reading the report

Step 4 writes a SARIF document per translation unit with three rule ids:

`noescape-inserted`
: one note per attribute written, naming the function USR and the parameter
  index.

`noescape-skipped`
: one warning per declaration the analysis proved and the transformation could
  not edit. In practice one reason reaches it: the parameter is `noescape` in
  this compilation because **API Notes** supplied the attribute, so the header
  reads as annotated while carrying nothing in its bytes. The other reasons it
  can report — a macro-spelled parameter, a system header, no written
  prototype, a synthesized parameter list, a location that resolves to no file
  — are pre-empted one layer earlier by the whole-definition refusals listed
  above, which take the function out of the candidate population before any
  site is considered. That whole-definition refusal, not this report, is what
  actually prevents the silent C mismatch below; the skip report is the
  backstop for the one case it cannot cover.

`noescape-rejected`
: one note per candidate parameter the analysis refused, carrying the reason,
  the location that decided it, and one hop of blame. This is the precision
  feed, not an error.

**Before applying, compare every `noescape-skipped` location against the
`noescape-inserted` locations from the other translation units.** A function
whose declaration in one place was annotated and in another was skipped is now
declared two ways, and the consequence differs by language — measured:

```c++
void takes(int *p);
void takes(__attribute__((noescape)) int *p);   // C: accepted silently
                                                // C++: error: conflicting
                                                //      types for 'takes'
```

In C the two declarations merge and the program keeps building, with the
attribute present on only some declarations. In C++ the second declaration is
a hard error. C is the more dangerous of the two, because nothing tells you.

`clang/utils/ssaf/noescape_exit_report.py` aggregates the SARIF files under a
directory into counts per rule and rejection reasons ranked by frequency:

```console
$ python3 clang/utils/ssaf/noescape_exit_report.py out/ --blame out/wpa.json
```

Its counts are per *translation unit*: a rejection is reported once per TU that
sees the definition, so a function in a widely included header is counted many
times. `--blame` takes the whole-program result instead, where each node
appears once, and ranks the callee parameters most often blamed for an
`EscapesViaCallee`; rejections blamed on a callee for any other reason are
counted separately rather than ranked with them. `--top` limits the reason
list, `--blame-top` the blame list. The whole-program result is a separate
input partly because each node appears there once, and partly because the
blamed callee's identity is not in the SARIF message at all — the
transformation writes only "via callee parameter *n*".

## Precision, measured

### The procedure

The numbers below are not reproducible without the procedure that produced
them, so it is recorded here rather than described. Measured on this tree,
September 2026:

- **Compiler**: `build/bin/clang`, this tree's own build.
- **Filter**: every entry of `build/compile_commands.json` whose `file`
  contains `llvm/lib/Support` — 183 translation units.
- **Precompiled header**: removed, in *both* of its forms. The build's PCH was
  produced by a different compiler and is rejected outright (`uses an older
  format that is no longer supported`), so `-Winvalid-pch`, `-Xclang
  -include-pch -Xclang <…cmake_pch.hxx.pch>` **and** `-Xclang -include -Xclang
  <…cmake_pch.hxx>` are all dropped. Keeping only the textual `-include`
  changes the result several-fold, so a report that does not say how the PCH
  was handled cannot be compared with this one.
- **Other flags**: `-c` and `-o` replaced by `-fsyntax-only`, plus
  `-Wno-everything`. Everything else is the build's own command line.

Two independent full runs under that procedure produce the identical histogram
below, so within it the figures are stable.

### The result

**28,615 insertions proposed** before the merge deduplicates shared headers,
**0 skipped**, and **10,400 rejections**, every one of which parsed. The whole
histogram, so that it reconciles to the total without an "everything else" row:

| reason | count | share |
| --- | ---: | ---: |
| `EscapesViaCallee` | 5,514 | 53.0% |
| `StoreToField` | 2,304 | 22.2% |
| `Return` | 1,475 | 14.2% |
| `VirtualCall` | 323 | 3.1% |
| `CastToNonPointer` | 292 | 2.8% |
| `Deallocation` | 193 | 1.9% |
| `HeapAllocation` | 107 | 1.0% |
| `UnnamedCallee` | 68 | 0.7% |
| `Capture` | 35 | 0.3% |
| `UnanalyzedCallee` | 30 | 0.3% |
| `StoreThroughPointer` | 24 | 0.2% |
| `AddressTaken` | 16 | 0.2% |
| `IndirectCall` | 7 | 0.1% |
| `UnrecognizedUse` | 6 | 0.1% |
| `StoreToGlobal` | 5 | 0.0% |
| `VarArgs` | 1 | 0.0% |

Whose fault the `EscapesViaCallee` half is, from the whole-program result
(1,699 rejections there, one per link-unit node rather than one per translation
unit), is dominated by a handful of callee parameters. `param -1` is the
implicit object parameter; a non-negative index is a written one:

| blamed callee parameter | count |
| --- | ---: |
| `llvm::raw_ostream::operator<<(StringRef)` param -1 | 29 |
| `llvm::raw_ostream::operator<<(const char *)` param -1 | 26 |
| `llvm::json::ObjectMapper::ObjectMapper` param 0 | 26 |
| `llvm::DynamicAPInt::operator SlowDynamicAPInt` param -1 | 25 |
| `llvm::StringRef::StringRef(const char *, long)` param 0 | 18 |

A further 29 rejections are blamed on a callee for `UnanalyzedCallee` rather
than `EscapesViaCallee` — the callee is simply not in the link unit — and are
reported separately, because widening the link unit is a different action from
improving the analysis.

Read `EscapesViaCallee` as "some callee downstream did one of the other
things": half the rejections are one of the rows below, seen from a distance.

### Views, and `std::span` in particular

Tracked views are **not** modelled. The consequence is severe enough to state
on its own: constructing a `std::span` from a pointer stores that pointer into
the span's data member, which is an ordinary field store inside the span
constructor, and the caller then sees `EscapesViaCallee`. Measured against
this SDK's libc++:

```c++
int span_size(int *p, int n) { return (int)std::span<int>(p, n).size(); }
int span_data(int *p, int n) { return *std::span<int>(p, n).data(); }
```

Both parameters are rejected, both blamed on the span *constructor* rather
than on the accessor — `.size()` never touches the pointer and is rejected
anyway. Over a link unit holding everything reachable from `<span>`,
`non_escaping` comes back **empty**. A codebase built on views should expect
zero annotations from this milestone.

### Other known ceilings

All of these are precision, not soundness — every one refuses in the safe
direction.

- **`memcpy`/`memset` destinations.** LLVM marks `memcpy`'s destination only
  as `returned`, not `captures(none)`, so the table does not trust it and the
  destination parameter is rejected; the source is accepted. `memset` is not
  in the table at all, so its destination is rejected for the weaker reason
  that nothing is known about it. Both appear as `UnnamedCallee` with the
  function's name in `detail`.
- **Non-view aggregates holding pointers.** Storing a parameter into any
  record field is `StoreToField`, including into an object that is itself a
  local and never escapes:
  ```c++
  void f(int *p) { Agg a; a.slot = p; sink(a); }   // p: StoreToField
  ```
- **Call-result over-approximation.** A pointer-carrying result of a call the
  parameter was passed to is treated as an alias of the parameter, whether or
  not the callee returns it.
- **Subobject flows stop at implicit special members.** A flow edge through a
  class whose destructor or constructor is implicit cannot name that member as
  an entity and degrades to `UnnamedCallee` — so the subobject edges survive
  only where every intermediate class declares its special members, which is
  not how ordinary C++ is written. Tracked as
  [#39](https://github.com/devincoughlin/llvm-project/issues/39).
- **No return-slot model.** An object constructed directly into caller-owned
  storage is connected to that storage by no edge.
  [#38](https://github.com/devincoughlin/llvm-project/issues/38).
- **A block literal passed straight to a `noescape` block parameter** is still
  `Capture`: the capture sinks before the argument is matched against the
  callee's promise. Pervasive in Objective-C.
  [#41](https://github.com/devincoughlin/llvm-project/issues/41).
- **Virtual calls, Objective-C message sends, and calls through callables**
  reject (`VirtualCall`, `ObjCMessage`, `IndirectCall`, `CallableUse`). They
  are later milestones, not defects.

## What is stable and what is not

The names are stable; the numbers inside a TU summary are not.

Entity ids in a **TU summary are run-local**. Twelve extractions of one
unchanged source file, with one unchanged compiler, produced **nine distinct
summary files**; the set of entity USRs was identical in all twelve, and the
numbering was not. So:

- a TU summary is **not** content-addressable and must not be cached or
  compared byte-wise;
- nothing downstream may key on a TU-level entity id;
- a test or tool that needs to compare two summaries must resolve every
  `{"@": n}` through `id_table` to a USR first.

What the pipeline does converge on is the **whole-program result**. Measured
over two identical full runs of `llvm/lib/Support`: **179 of the 183 TU
summaries differ**, `lu.json` differs, and `wpa.json` is **byte-identical**. So
the verdict every consumer actually reads is stable even when almost nothing
feeding it is.

The link unit sits in between and must not be treated as either. It is
*canonically* stable — the entity numbering is canonicalized by the linker, and
an order-insensitive comparison of two link units built from differing
summaries is equal — but it is not byte-stable: `flows_to` array orderings are
permuted. At twelve entities over one source file it comes out byte-identical,
which is misleading; at 183 translation units it does not. Do not cache it by
content hash and do not diff it byte-wise.

One further caveat when checking any of this: a link unit is named after the
basename of the `-o` file it was written to, so linking the same input to
`a.json` and to `b.json` produces documents differing in their `"name"` fields
and nowhere else. Hold the output name fixed before diffing.

See [#27](https://github.com/devincoughlin/llvm-project/issues/27).

## Validating the corpus

The negative corpus under
`clang/test/Analysis/Scalable/ParameterEscape/Negative/` is checked against
something other than this analysis. Each file carrying `// DRIVER:` lines is
compiled with that driver at `-O2 -g -fsanitize=address` and must produce an
AddressSanitizer report, which is evidence from outside the analysis that the
file demonstrates a *true* escape rather than merely a program the analysis
rejects:

```console
$ ninja -C build check-clang-ssaf-escape-corpus
```

Today 22 of the 28 corpus files are validated this way. The other six carry a
`// NO-DRIVER: <reason>` line instead; one of the two is required of every
file, so a corpus cannot quietly lose its drivers. Two of the six reasons are
findings rather than limitations: `lambda-capture.cpp` and `callable-use.cpp`
are **not true escapes at runtime** — the closure is invoked in its own frame,
and a static member call passes no object — so the analysis rejecting them is a
precision loss that no driver can ever demonstrate.

"Some AddressSanitizer report was produced" is not the claim, so two more
directives pin the report to *this file's* escape:

`// DRIVER-KIND: <kind>`
: the report kind required, exactly. Defaults to `stack-use-after-scope`;
  `deallocation.cpp` is the one file that sets it, to `heap-use-after-free`.

`// DRIVER-EVIDENCE: <substring>`
: a substring the report must contain. Defaults to `'local'`, the name
  AddressSanitizer prints in its frame description. Without it a file whose
  parameter does not escape at all passes as long as its driver contains any
  memory error somewhere, which would certify a precision loss as a true
  escape.

  An override has a floor: the substring must not appear in the script's own
  reference report for that kind. A phrase every report of the kind contains
  would read like attribution while checking nothing — which is what both of
  the corpus's two overrides were, until the floor was added and rejected them
  both. `deallocation.cpp` and `throw.cpp` now check for their own file name,
  which establishes only that the faulting access is in code compiled from
  that file; each says so, and says that its `DRIVER-KIND` is doing the real
  work.

The target needs a compiler with an ASan runtime. It probes for one at
**configure** time: the in-tree clang when compiler-rt is being built,
otherwise the first host compiler that can actually link `-fsanitize=address`.
Because the probe runs at configure time, installing or removing an
ASan-capable compiler afterwards has no effect until CMake is re-run. Override
it with `-DCLANG_SSAF_ESCAPE_CORPUS_CLANG=<path>`, or run the script
directly:

```console
$ python3 clang/utils/ssaf/validate_escape_corpus.py /usr/bin/clang \
    clang/test/Analysis/Scalable/ParameterEscape/Negative
```

If the compiler it ends up with cannot report a stack-use-after-scope — or
reports one on a program that has none — the script refuses to run at all
rather than validating nothing.

`-O2` is part of the claim: an escape the optimizer can delete was never
observable. Most of the corpus needs nothing special to survive it, but
`return.cpp` does and says so in the file.

## Reference

Design document:
`devincoughlin/features/noescape-inference/specs/2026-09-19-cross-tu-noescape-inference-design.md`
— section 5.2 for the classification rules, 5.4 for the transformation and
5.5 for the remote-redeclaration policy.
