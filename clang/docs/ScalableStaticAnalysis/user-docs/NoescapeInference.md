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

The fixpoint proves what it can and believes exactly two things it has not
proved. Neither is a rule about what lies *outside* the link unit: the first
is believed wherever it is written, whether the callee's body is in the link
unit or not, and the second applies to a callee with no body *in the
translation unit holding the call* — a definition elsewhere in the link unit
does not displace it.

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

  That is the rule rather than an accident of it, and it was kept over the
  alternative
  ([#44](https://github.com/devincoughlin/llvm-project/issues/44)). The
  attribute is trusted in the first place because a declaration's body may be
  absent from the link unit — a system library, a header over a binary — and
  believing what a human wrote on the declaration is then the only way to get
  an answer at all. The alternative was to narrow that trust: prefer the
  analyzed verdict whenever the body *is* present, and believe the attribute
  only when it is not. It was considered and declined. The analysis is
  conservative by construction — anything it cannot model is an escape — so
  wherever it is imprecise, the analyzed verdict would override a correct
  human annotation and lose exactly the precision the annotation was written
  to supply. The trust is therefore uniform: a written `noescape` outranks the
  analysis's own verdict on the same parameter, even one it has already
  computed.

  The cost of that choice is accepted, and it is specific. The pipeline is
  otherwise conservative in one direction only, so that a defect is a
  *missing* annotation. This is one of two places a defect is an **inserted**
  one, and the one a written attribute controls: a wrong or stale `noescape`
  anywhere in your headers can produce a wrong annotation somewhere else,
  whose proof rested on the claim. Nothing in the pipeline relates the two
  facts it holds. Measured: with the attribute present, the
  per-translation-unit summary records no flow from `caller`'s parameter to
  `liar` at all — the call site's trust discharges the edge at extraction —
  so the whole-program step never sees a contradiction to report, and the run
  above lists `liar`'s rejection and `caller`'s annotation side by side
  without comment.

  The other place is the table of C library functions below. Its rows cannot
  be wrong — a unit test proves each against LLVM — but its premise, that the
  definition which links is the C library's, is checked by nothing, and the
  audit cannot reach it: a program with no written `noescape` gives
  `-Wlifetime-safety-noescape` nothing to check. Measured: a `.c` file
  defining a `strlen` that stores its argument to a global, linked with a
  caller that reaches it through a bare declaration, annotates the caller's
  parameter with an empty `rejected` list, and step 4 writes the attribute.
  The definition is never summarised at all — a function clang models as a
  builtin gets no entity name (`ASTEntityMapping.cpp`) — so the link unit
  never holds the contradicting evidence. The same body named `mylen` is
  `StoreToGlobal`, and its caller `EscapesViaCallee`. The premise is the one
  the optimizer already makes of the same declaration, so a program that
  breaks it is already miscompiled; that makes the case defensible rather
  than absent, and it is open as
  [#57](https://github.com/devincoughlin/llvm-project/issues/57).

  For the written attribute, that makes the audit a precondition rather than
  a suggestion: it is what makes this choice safe, not something attached to
  it. Before trusting existing annotations — before running the inference
  over a tree that has any — audit them with `-Wlifetime-safety-noescape`, an
  independent analysis that checks bodies against the attributes on them, and
  fix what it finds. The inference's output is then only as trustworthy as
  that audit was, and the audit checks only what it examined: an annotation
  on a declaration whose body it never reached is believed on the writer's
  word, with nothing behind it. *Auditing existing annotations* below gives
  the command — which needs an extra flag on C — and the control without
  which a clean audit is not evidence.

  **In C the audit cannot reach the annotations this rule is about.** The
  attribute is believed when a human writes it on a *declaration*, and a
  `noescape` written only on a prior declaration never reaches the
  definition's parameter — which is the one the checker reads. Measured: a
  header declaring `int api_read(__attribute__((noescape)) int *p);` and a
  `.c` file defining `int api_read(int *p) { sink = p; ... }` audits clean,
  with the `NoEscapeAttr` plainly present in the AST on the header's
  `ParmVarDecl` and absent on the definition's. Writing the attribute on the
  definition as well makes it fire. In C++ the same pair is
  `error: conflicting types`, so this is C's alone.

  **This pipeline's own output is not affected.** The transformation emits one
  edit per declaration *including the definition*: measured, a two-file run
  over a header and its implementation produced a replacement in each. So a
  tree this pipeline has rewritten is auditable. It is **hand-written**
  annotations, and those supplied by API Notes, that live on a declaration
  alone — and those are exactly the ones this rule believes without evidence.
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

## Auditing existing annotations

The audit is a syntax-only pass over every translation unit, with this checker
promoted to an error. The build's own flags go **first** and the audit's flags
last:

```console
$ clang -fsyntax-only <the build's own flags> -Wno-everything \
    -Wsystem-headers -Werror=lifetime-safety-noescape \
    -Werror=ignored-attributes -Xclang -fexperimental-lifetime-safety-c <tu>
```

### What a green audit means

> Of the annotated parameters the analysis **actually examined**, none
> exhibited one of the escape shapes it **models**, at a location where the
> diagnostic **was enabled**, in a way that reached the **exit status** of the
> command above.

"Annotated" means annotated **in the AST** rather than in your source, which
is group 2 below. "Examined" means the analysis ran over that body **and** gave
that parameter an origin; the two halves fail independently, groups 1 and 4
being the first and the first layer of group 3 the second. "Models" is not a
vague qualifier: the analysis has exactly three escape facts —
`ReturnEscapeFact`, `FieldEscapeFact` and `GlobalEscapeFact`, in
`clang/include/clang/Analysis/Analyses/LifetimeSafety/Facts.h` — and group 3
states the rule for each. The last clause is there because one member of group
4 leaves the analysis running and the diagnostic printed while the command
still exits 0.

That is the whole of it. It is **not** evidence that the annotations are
correct, and it is not a lower bound on how many of them were checked — a
green audit is consistent with nothing at all having been examined, which is
the failure this section exists to prevent.

The four groups below organise the known ways a parameter is annotated and not
examined, or examined and not reported. That list is not closed, and should not
be read as though it were: one more mechanism was found while this section was
being written, and is recorded in it as
[#54](https://github.com/devincoughlin/llvm-project/issues/54). Treat a new
one arriving in any of the four groups as the expected case.

What "examined" and "models" exclude is easiest to see in one file:

```c
int *ctlsink;
void control(__attribute__((noescape)) int *p) { ctlsink = p; }  /* fires */

int *registry[8];
void reg(__attribute__((noescape)) int *p) { registry[0] = p; }  /* silent */

struct state { int *cb; };  struct state gstate;
void set_cb(__attribute__((noescape)) int *p) { gstate.cb = p; } /* silent */

typedef void (^blk)(void);  blk ghandler;
void install(__attribute__((noescape)) blk b) { ghandler = b; }  /* silent */
blk  fetch(__attribute__((noescape)) blk b) { return b; }        /* silent */
```

Measured under the command above, plus `-fblocks` for the last two: **exit 1,
one error** — the control's — with all five `NoEscapeAttr` nodes present in the
AST, one on each definition's own parameter. `install` and `fetch` were never
**examined**: a block pointer is not a type the analysis tracks, so nothing
done to one is observed. `reg` and `set_cb` were examined, but what they do is
not a shape the checker **models**: a `GlobalEscapeFact` is created only for an
origin whose declaration is a variable with global storage
(`FactsGenerator.cpp`), and an array element and a struct field are neither.

"Global storage" is the operative phrase rather than "global variable":
measured, a `static` local, a file-scope `static` and a `_Thread_local` or
`thread_local` variable all count, and an ordinary local does not.

A second file shows that "models" is narrower still, in a way that is a defect
rather than a design limit:

```c
#define NE __attribute__((noescape))
int *g, *g2;
void use(void);
__attribute__((noreturn)) void die(void);

void publish_clear(NE int *p, NE int *r) { g = p; use(); g = 0;  /* silent */
                                           g2 = r; }             /* reported */
void install_loop(NE int *p) { g = p; for (;;) use(); }          /* silent */
void noreturn_case(NE int *p) { g = p; die(); }                  /* reported */
```

Measured: four attributes in the AST, **two** diagnostics. The escape is
recorded only for an origin that is *still live where the function returns*
(`FactsGenerator.cpp`: "for all origins with global-storage that remain live at
exit"), so clearing the global afterwards hides it, and a function with no
reachable exit reports nothing at all. `noreturn_case` is not the same case and
does fire. The sibling parameter `r`, reported from inside the same function,
is what rules out every other explanation: the analysis ran, the attribute was
there, the shape is modelled. Tracked as
[#54](https://github.com/devincoughlin/llvm-project/issues/54).

### The flags, and why each one is there

**`-Werror=lifetime-safety-noescape -Werror=ignored-attributes`, rather than a
bare `-Werror`.** Each enables its group and promotes only that group, so the
exit status means exactly "a `noescape` escaped, or an attribute was
discarded", whatever else the build's flags emit. A bare `-Werror` is both too
strong and too weak: too strong because it promotes every unrelated warning the
translation unit already has, and too weak because a
`-Wno-error=lifetime-safety-noescape` among the build's flags demotes the
escape back to a warning and the audit exits 0 with the defect present. A plain
`-Wno-error` there does *not* — a later bare `-Werror` wins, measured exit 1 —
so only the group-specific form defeats it from in front. Measured on a file
holding one escape and one unused variable: with these two flags the command
exits 1 on the escape alone, with `-Wall -Wextra` in either position and with
either form of `-Wno-error` ahead of them.

**The build's flags first.** Both **enablement and promotion** are
last-one-wins, so either can be undone from behind. Measured, each exiting 0
with the escape present: a `-Wno-lifetime-safety-noescape` placed after the
audit's flags turns the check off, and a `-Wno-error=lifetime-safety-noescape`
placed after them demotes it to a warning. Ahead of the audit's flags both are
overridden and the command exits 1. A makefile that appends the build's flags
last — the ordinary `$(CC) $(AUDITFLAGS) $(CFLAGS)` shape — hits both, which is
a reason to check the order rather than assume it.

**`-w` must be removed from the build's flags rather than overridden.** It
silences every warning from either position — measured exit 0 and no output
both ways.

**`-Wsystem-headers`.** A function defined in a header reached through
`-isystem` is not diagnosed at all: the analysis asks
`DiagnosticsEngine::isIgnored` at the *declaration's own location*
(`ShouldCheckNoescapeViolations`, `clang/lib/Sema/SemaLifetimeSafety.h`), and a
system header's location answers yes. Measured: the same header exits 1 through
`-I` and 0 through `-isystem`, and 1 again once `-Wsystem-headers` is added.
Builds routinely route their own SDK and framework headers through `-isystem`.

**`-Werror=ignored-attributes`.** `noescape` applies only to a pointer,
reference, class, struct or union; on anything else clang **discards the
attribute**, and that group alone is where it says so. Silenced, the audit
certifies an annotation clang never applied — which constrains nothing, and
which the inference does not believe either. Measured on

```c
int *gi;
void s_atomic(__attribute__((noescape)) _Atomic(int *) a) { gi = a; }
```

the command without it exits 0 having printed nothing; with it, exit 1 on
`'noescape' attribute only applies to a pointer, reference, class, struct, or
union ('_Atomic(int *)' is invalid)`. A pointer-to-member parameter is
discarded the same way. A well-formed annotation is silent either way, in C
and in C++, so re-enabling the group adds no noise.

**`-Xclang -fexperimental-lifetime-safety-c` is not optional on C.** Without
it the analysis is never invoked for a C translation unit, so the audit exits 0
having compared no body against any attribute — a pure-C tree reads as fully
audited while nothing was checked. The flag is `-cc1`-only; spelled without
`-Xclang` the driver rejects it. Measured on this tree, on the two shapes the
checker does catch:

```c
int *g;
int *ret(__attribute__((noescape)) int *p) { return p; }
void stash(__attribute__((noescape)) int *p) { g = p; }
```

| how that file is compiled | diagnostics |
| --- | ---: |
| `-x c` | 0 |
| `-x c` plus `-Xclang -fexperimental-lifetime-safety-c` | 2 |
| the same content as `-x c++` | 2 |

> **Delete this flag when it defaults on.** `-Xclang
> -fexperimental-lifetime-safety-c` is a stopgap for
> `LangOptions::EnableLifetimeSafetyInC` defaulting to 0. When
> [#51](https://github.com/devincoughlin/llvm-project/issues/51) defaults it on
> — the rename is #51's, and whether this spelling survives as an alias is an
> open upstream question there, so do not count on it — drop the flag from the
> command above, and delete this note along with the paragraph beginning
> "`-Xclang -fexperimental-lifetime-safety-c` is not optional on C", the code
> block under it and the table under that. The rest of this section is about
> the checker rather than about the flag and stays. Other text needs *editing*
> rather than deleting — at least this much:
>
> - **The control's structure stays; both of its rows must be respelled.** Row
>   one names a spelling that will no longer exist. Row two reaches the "off"
>   half by *omitting* the flag, which after #51 disables nothing — so both
>   rows become the presence and the absence of the negative flag
>   (`-fno-lifetime-safety-c`, or whatever #51 lands as), and the sentence
>   after the table needs the same substitution.
> - **The Objective-C paragraph needs rewording, not deletion.** Its "earlier
>   still", "before the C gate above is reached" and "the same file ... with
>   the flag" all lean on text this deletion removes.
> - **One phrase outside this section goes too.** The *What is trusted* bullet
>   above says this audit "needs an extra flag on C", which is false once the
>   option defaults on. Nothing else in the document qualifies a measurement by
>   the flag; that was checked rather than assumed.

Objective-C and Objective-C++ are refused earlier still: the analysis bails on
`LangOpts.ObjC` before the C gate above is reached, so no flag makes this audit
check anything in either. Measured: the same file compiled `-x objective-c`
with the flag, and `-x objective-c++`, both produce 0 diagnostics. Nothing else
gates it — C89, C23, C++98, `-x c-header`, OpenCL C and translation-unit
analysis mode all diagnose normally, measured.

### The control, and the exact thing it proves

A clean audit is evidence only if something in the same run has been *seen to
fail*. Compile one deliberately wrong function alongside the tree, under the
same flags every other translation unit gets:

```c
extern int *noescape_audit_control_sink;
void noescape_audit_control(__attribute__((noescape)) int *p) {
  noescape_audit_control_sink = p;   /* the parameter is not noescape */
}
```

and require **both** outcomes, not just the first:

| the control, compiled as C with the audit command | expected |
| --- | --- |
| with `-Xclang -fexperimental-lifetime-safety-c` | **fails**, exit 1 |
| with that flag removed | **passes**, exit 0 |

The control failing is what proves the checker ran; the control passing once
the flag is removed is what proves the flag is why it ran. Measured: exit 1
and exit 0 respectively. An audit that came back green without its control
having fired has established nothing, which is how a pure-C tree came to be
recorded as audited.

**What it does not prove.** The control establishes that the checker ran *for
that function, at that location, under those flags* — and that is the whole of
it. Group 4 below splits by **scope**, and only one third of it is something a
control can see:

- **Per location** — the pragma, and a definition in a system header. A control
  elsewhere in the same translation unit fires happily while the annotated
  function is never analyzed.
- **Per function** — `max-cfg-blocks`, and a body with no CFG. At any realistic
  threshold a small control fires while a large function is skipped, so the
  control does not vouch for these either. (A degenerate `=1` happens to
  silence the control too, which is not something to rely on.)
- **Per command** — `-w`, `-Wno-lifetime-safety-noescape` and
  `-Wno-error=lifetime-safety-noescape`. All three break the control's own
  "fails" row — the first two by silencing it, the third by demoting it to a
  warning — so these the control does catch, provided it is compiled with the
  same flags in the same position.

So the control is necessary and nowhere near sufficient. Compiling it in every
mode and flag position the tree is covers the per-command flags and nothing
else; the per-location and per-function members remain invisible to it.

### The audit is intraprocedural; the inference is not

This is the largest single gap and it is structural. The checker looks only
inside one body. Passing a `noescape` parameter to another function is not an
escape as far as the audit is concerned, whatever that function does with it.
Measured, with a control in the same file to prove the run had power: a
parameter passed to a bodiless callee, and one passed to a callee that stores
it to a global, are both silent; the same parameter laundered through a local
and then stored is caught.

The inference has no such limit, and the difference is most of its verdict. By
the histogram in *Precision* below, `EscapesViaCallee`, `UnnamedCallee` and
`UnanalyzedCallee` together are **54%** of its rejections on
`llvm/lib/Support`. So for the majority of the cases where the two disagree,
the audit is silent by construction. A green audit does not corroborate the
inference's verdict; the inference is the stricter of the two.

The converse is the case the audit exists for, and it is narrow: an annotation
on a *bodiless* declaration is exactly what *What is trusted* believes without
evidence, and is exactly what an audit of bodies can never check.

### What a green audit does not establish

The checker is advisory — see the design document's §7 — and what follows is
what that word costs in practice. Four groups, which organise the mechanisms
that are **known**. They are not a partition of a closed space: every review
round of this section has turned up a mechanism the previous round had missed,
most recently
[#54](https://github.com/devincoughlin/llvm-project/issues/54). A shape that
fits none of the four is a reason to suspect the list, not the shape.

**1. No analysis ever ran over that body.** Nothing in a per-translation-unit
exit status can report an annotation that no compilation reached: a translation
unit left out of the loop, one that fails to compile, or a header no audited
translation unit includes. To these add the language gates above, and —
measured, in C++ — an **uninstantiated function template**, whose body is never
analyzed at all. A control on the next line fires while the template pattern is
silent; instantiating it through a call makes both fire. This is the one gap the
inference shares, since it refuses everything templated.

**2. The attribute was not in the AST.** The audit sees the AST, not your
source, and four things separate them. The attribute may be **discarded as
inapplicable** — now visible, because `-Werror=ignored-attributes` is in the
command. The spelling macro may **expand to nothing** in this configuration
(`#define NS_NOESCAPE` with an empty body: exit 0, zero `NoEscapeAttr` nodes,
where the same file with the macro defined properly exits 1). The annotated
declaration may sit inside an **`#if` that is false here** — same signature.
Or, in C only, the `noescape` may be on a **prior declaration while the
definition is written bare**, which is the ordinary shape of a
header-annotated C API; see *What is trusted* above, where that case is set out
against the trust rule it undermines.

**3. The checker does not model it.** Two layers. First, whether the parameter
is examined at all: only a type the analysis gives an *origin* is, and
`OriginManager::hasOrigins` accepts exactly six things — anything satisfying
`isPointerOrReferenceType`, a `gsl::Pointer` type, a type registered in
`LifetimeAnnotatedOriginTypes`, an `_Atomic` wrapper around any of these (it
sees through), a standard callable wrapper such as `std::function`, and a
lambda with a pointer-like field. Nothing else, which is why the list below is
short rather than illustrative. Measured as examined:
`void *`, function pointers, array parameters, `int **`, `const int *`,
references to a struct, `gsl::Pointer` views, and `std::function` **by value**,
which is examined and does fire. Measured as **never examined**, with a control
in the same file:

- **block-pointer parameters**, in both languages — `BlockPointerType` is not a
  `PointerType`, so nothing done to a block parameter is ever observed. That is
  the attribute's original and dominant use, through `NS_NOESCAPE`:

  ```c
  typedef void (^blk)(void);
  blk gb;
  void p1(__attribute__((noescape)) blk b) { gb = b; }   /* to a global */
  blk  p2(__attribute__((noescape)) blk b) { return b; } /* returned */
  ```

  Both are shapes the audit otherwise catches, and both give **0 diagnostics**
  where the same functions written with `int *` give 2. A control written with
  `int *`, like the one above, proves the checker ran and proves *nothing*
  about a block-heavy tree.
- **a class, struct or union parameter passed by value** that is not a lambda,
  a standard callable wrapper or a `gsl::Pointer` view — the `function_ref`
  shape, and any handle struct holding a pointer. `noescape` explicitly permits
  these types, so they get annotated; they are simply never looked at.

Second, for the parameters it does examine, what counts as an escape. There are
exactly three escape facts, so this layer is closed at the fact level rather
than by enumeration — but each one's rule is narrower than its name, and the
narrowing is what a reader gets wrong:

`ReturnEscapeFact`
: the parameter is returned.

`GlobalEscapeFact`
: an origin whose declaration is a **variable with global storage** is live at
  a point the function returns from. Three things put a parameter into such an
  origin. One is an **assignment** whose left-hand side, after
  `FactsGenerator::handleAssignment` strips parentheses and implicit casts, is
  a `DeclRefExpr` naming that variable — which includes a **qualified-id** such
  as `ns::g` or `C::sm`, not just a bare name; in C, which has no
  qualified-ids, a `DeclRefExpr` left-hand side *is* a bare name.
  `handleAssignment` builds a left-hand origin list only for a `DeclRefExpr`
  or a `MemberExpr` and `return`s having recorded nothing otherwise, which is
  what makes the assignment shapes below silent. The second is the
  **initializer of a variable declaration** (`VisitDeclStmt`), which reaches
  global storage through a `static` or `thread_local` local — in C++
  only, since C rejects initializing a static local from a parameter outright
  (`initializer element is not a compile-time constant`), leaving a separate
  assignment as the only route there. The third is passing the parameter to a
  **`[[clang::lifetime_capture_by(x)]]`** parameter whose capturing parameter
  is a reference and whose capturing argument is a `DeclRefExpr` naming a
  global-storage variable (`handleLifetimeCaptureBy`; the declaration used in
  these measurements is shown after this list) — measured,
  `captureIntoX(p, gx)` reports "escapes to this global storage", while the
  same call with a *local* `X` is silent.

`FieldEscapeFact`
: an origin whose declaration is a **`FieldDecl`** is live at such a point.
  Three things put a parameter into one: a **member access on the implicit
  object**, written `f = p` or `this->f = p`, in a member function defined
  in-class or out of line; a **constructor member-initializer**,
  `C(NE int *p) : f(p) { }` (`handleCXXCtorInitializer`), which is the ordinary
  way a C++ class stores a constructor parameter into a field and **is**
  reported; and a **`[[clang::lifetime_capture_by(x)]]`** argument where `x` is
  a member access on the implicit object — `captureIntoX(p, f)` and
  `captureIntoX(p, this->f)` both report "escapes to this field". Note that the
  last is why the first is stated as *how the field is written* rather than as
  the field alone: `captureIntoX(p, f)` is also a member access on the implicit
  object, and is not an assignment.

  All three are C++ only, for two different reasons. C has neither an implicit
  object nor a member-initializer list; and it cannot reach the capture route
  either, because with no reference parameters the capturing argument is always
  a prvalue and never peels to a declaration's origin — measured,
  `captureIntoX(p, &gx)` in C is silent while the control in the same file
  fires. So C has no instance of this fact at all.

Both capture routes are the same attribute, and every C++ measurement above
used this declaration:

```c++
struct X {} gx;
void captureIntoX(const int *i [[clang::lifetime_capture_by(x)]], X &x);
```

**The capturing parameter has to be a reference**, which is a necessary
condition the rule above states but is easy to lose: `getRValueOrigins` peels
to the capturing argument's declaration origin only for a glvalue. Measured,
the same function declared `X x` by value is silent even when called with the
global `gx`, although that argument *is* a `DeclRefExpr` naming a
global-storage variable. Nothing can outlive the call through a by-value
capturer, so this costs no coverage — but it is also the reason C cannot reach
this route at all.

The capturing type need not be pointer-like. `collectCaptureBy` registers it
in `LifetimeAnnotatedOriginTypes`, the third arm of `hasOrigins` above, so even
an empty `struct X {}` acquires origins there. It is the one place the
tracked-type rule will mislead a reader who applies it to the capturing
argument rather than to the annotated parameter.

Those four producers — `handleAssignment`, `VisitDeclStmt`,
`handleCXXCtorInitializer` and `handleLifetimeCaptureBy` — are the complete
set, and the list is checkable rather than asserted. In `Origins.cpp`, an
origin is keyed to a declaration only through
`OriginManager::getOrCreateList(const ValueDecl *)` and through
`initializeThisOrigins`, whose origins are keyed to the `CXXMethodDecl` and so
are neither a `FieldDecl` nor a variable — `handleExitBlock` emits nothing for
them. The `Expr` overload reuses a declaration's list only for a `CXXThisExpr`,
a `DeclRefExpr`, or a `MemberExpr` on a `CXXThisExpr`. Enumerating the flow
*destinations* in `FactsGenerator.cpp` that can resolve that way — all of them,
not only the direct `createFact<OriginFlowFact>` sites, since `flow()` and
`killAndFlowOrigin()` reach the same fact — yields those four and nothing else.

Two consequences do most of the damage.

**An assignment's left-hand side is a syntactic test, so semantically
identical stores differ.** Measured, with a reporting sibling in every file:
`g = p`, `(g) = p` and `ns::ng = p` are reported — parentheses are stripped and
a qualified-id is still a `DeclRefExpr` — while `*(&g) = p`,
`int **q = &g; *q = p`, `arr[0] = p` and `*out = p` are all silent, none of
them being a `DeclRefExpr` or a `MemberExpr` at all. `gs.f = p` on a global
struct and `s->f = p` through a pointer parameter *are* member accesses and
pass that gate, but name a field of something other than the implicit object,
and are silent too. This one rule is why an array-element store, an
out-parameter store, a store through a pointer to a global and a store through
`&` all go unreported; they are not four separate limitations.

**Liveness at exit is required, and that part is a defect** — see the
`publish_clear` measurement above and
[#54](https://github.com/devincoughlin/llvm-project/issues/54). Assigning the
parameter to a global and clearing the global before returning is not reported,
and neither is a function with no reachable exit, such as one ending in
`for (;;)`. A function ending in a `noreturn` call is a different case and
**is** reported.

Also measured as *not* reported: a store into a field of a *subobject* of the
implicit object (`in.g = p` and `this->in.g = p`, where `in` is a member — only
direct fields count); a store into a field of another object of the same class,
whether reached by pointer (`o->f = p`) or held by value (`o.f = p`); a capture
of the pointer by a block; and laundering the pointer through an integer.

**Reference parameters are examined but almost nothing done through one is
reported**, which the tracked-type rule above does not convey on its own.
Measured in C++, each with a reporting sibling in the same function to prove
the run had power: storing a pointer loaded from an `int *&` parameter into a
global is silent, and so is `int *&&`; *returning* an `int *&` parameter is
silent, where returning an `int &` parameter is reported; and binding a
reference to a direct field of the implicit object and storing through it
(`int *&r = this->f; r = p;`) is silent, where `this->f = p` in the same class
is reported.

Everything interprocedural is in this group too, by the section above it.

**4. The analysis never ran for that body, or its report never reached the
exit status.** These compile, carry the attribute in the AST, and are shapes
the checker does model. They divide by **scope** — two bullets each for per
location, per function and per command — and only the per-command ones are
something the control can catch, as *The control* above sets out:

*Per location* — `ShouldCheckNoescapeViolations` asks `isIgnored` at the
declaration's own location, so the analysis does not run there at all:

- **`#pragma clang diagnostic ignored "-Wlifetime-safety-noescape"`**, in the
  translation unit or leaked out of a header it includes. Measured: exit 0 with
  the `NoEscapeAttr` present in the AST. This is not warning suppression.
- **A definition in a system header**, if you cannot add `-Wsystem-headers`.
  Same mechanism, same location test.

*Per function* — the body is reached and then skipped:

- **`-Xclang -lifetime-safety-max-cfg-blocks=N`.** The default is 0, meaning
  unlimited, so a default audit is unaffected; but when set, a function whose
  CFG exceeds `N` is skipped, and says so only in a debug build
  (`LifetimeSafetyAnalysis::run`). Measured: `=1` silences even a two-block
  function — a degenerate threshold that happens to silence a control too. At
  any realistic `N` the control fires and the large function is still skipped.
  The driver spelling without `-Xclang` is accepted and silently not forwarded
  to `-cc1`, so only the `-Xclang` form has any effect.
- A body for which **no CFG can be built** would be skipped the same way. No
  attempt to construct one succeeded, so read this as where such a case would
  belong rather than as something known to happen.

*Per command* — three flags in two bullets, and the control catches all of
them, because each breaks its "fails" row as surely as it silences the tree:

- **A `-w` or a `-Wno-lifetime-safety-noescape` in a position that defeats the
  command** — see *The flags* above. `isIgnored` is then true, so the analysis
  does not run for that body either, and the control goes quiet with it.
- **`-Wno-error=lifetime-safety-noescape` in a position that defeats the
  command** is the one member where the analysis *does* run: `isIgnored` is
  false for a demoted warning, so the escape *is* found and the diagnostic *is*
  printed — measured `exit=0`, 0 errors, 1 warning, with the text on stderr.
  Only the exit status is lost, which is why the statement at the top of this
  section names the exit status rather than the diagnostic. A run that reads
  the output rather than only the status catches this one without a control.

None of this makes the audit worthless — it makes it asymmetric. **A report
from it is a real defect and worth acting on; silence from it bounds nothing.**

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
