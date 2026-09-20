# Cross-TU `noescape` Inference — Design (Milestone 1)

Date: 2026-09-19
Status: Draft v3, revised after adversarial reviews of the spec and of the implementation plan
Scope: Milestone 1 of a staged feature; later milestones are sketched in §10 for
context only.

## 1. Motivation and goals

Swift/C++ interop imports parameters carrying `[[clang::noescape]]` as
non-escaping, which is what lets Swift offer safe `Span`-style overloads for
pointer, reference, and view-typed parameters. Today the attribute must be
written by hand. This feature infers it, across translation units, and emits
it as source edits through the existing clang-reforge pipeline.

Goals:

- Infer `noescape` for parameters that provably do not escape, using
  whole-program information.
- Emit the annotation as source edits (clang-reforge / `clang-apply-replacements`).
- Be **sound**: a wrong `noescape` on a pointer or reference parameter is
  undefined behavior the optimizer acts on (codegen lowers it to a
  `captures(...)` attribute), and on any parameter it would let Swift's "safe"
  overloads retain a reference into memory the callee kept. Anything not
  positively modeled counts as an escape.
- Grow monotonically: later milestones may only add annotations, never retract
  ones M1 would have produced.

Non-goals (M1): compile-time implicit inference (the `-flifetime-safety-inference`
analogue), audit-only reporting, an aggressive/heuristic mode, a hand-maintained
allowlist of external functions.

### 1.1 What "escape" means here

- For a pointer or reference parameter `p`: `p` escapes iff a pointer or
  reference to the object `p` designates, or to any subobject of it, that is
  derived from `p`'s value survives the call — stored anywhere but a local
  automatic scalar, returned, captured, passed to code that may keep it, or
  converted to a non-pointer type. A value *loaded through* `p` (`p->next`) is
  a different pointer with its own provenance; storing it does not escape `p`.
  This is the provenance semantics `noescape` lowers to
  (`clang/lib/CodeGen/CGCall.cpp`, `captures`).
- For a by-value view parameter `v` (§5.2 candidate types): `v` escapes iff a
  copy of any pointer-carrying field of `v`, or a pointer derived from one,
  survives the call. Codegen emits **no** `captures` attribute for such
  parameters (`CGCall.cpp` gates on `isValidPointerAttrType`), so for views
  the guarantee is a frontend/Swift contract only, checked by
  `-Wlifetime-safety-noescape` and consumed by Swift.

## 2. Decisions

| Question | Decision |
|---|---|
| Consumer of the inferred annotations | Source rewriting via clang-reforge |
| Primary motivation | Swift/C++ interop |
| Parameter categories (eventual) | Pointers, references, view-like records, callables |
| Languages (eventual) | C, C++, Objective-C, Objective-C++ |
| First milestone | Sound end-to-end pipeline, narrow precision (C/C++, direct calls) |
| Architecture | Dedicated sound escape extractor + generic whole-program fixpoint + transformation |
| View-record predicate | `isGslPointerType(T)` **or** `T` carries `swift_attr("~Escapable")`, and `T` is trivially copyable and trivially destructible |
| Dependency on `CallGraph` summary | None (wrong granularity; §5.3) |
| Call-site resolution | New shared helper; `PointerFlow` migrates to it **now** |
| Call results | Every pointer-carrying call result is an alias of every alias argument; no reliance on `lifetimebound` |
| Trusted external knowledge | Declared `noescape` (source / API Notes) and a static table of libc functions whose parameters LLVM's libcall inference marks `captures(none)`, verified against LLVM by a unit test, minus deallocators |
| Default spelling | `__attribute__((noescape))` in every language; C++11/C23 spelling only by explicit option |
| Remote unrewritable redeclarations | Documented consequence and SARIF gate; no second pass, no mechanism |

## 3. Background facts the design relies on

- `noescape` is a parameter attribute permitted in this fork on pointer,
  reference, and record-typed parameters
  (`clang/lib/Sema/SemaDeclAttr.cpp`, `handleNoEscapeAttr`). It applies to the
  outermost pointer level only.
- It is part of the function type (`ExtParameterInfo`). In **C++**, a mismatch
  between redeclarations is a hard `conflicting types` error and the attribute
  does not propagate to the definition's `ParmVarDecl`. In **C**, mismatched
  redeclarations merge to a composite type *without* the attribute, with no
  diagnostic (`ASTContext::mergeExtParameterInfo`). Conversions between
  function types drop it without error.
- Codegen emits the `captures` attribute only for pointer-like parameter types
  (`CGCall.cpp`, `isValidPointerAttrType(ParamType, RefOkay=true)`), never for
  by-value records.
- Overrides must preserve it (`-Wmissing-noescape`, both C++ virtual and ObjC).
- API Notes can attach `NoEscapeAttr` to parameters
  (`clang/lib/Sema/SemaAPINotes.cpp`).
- Clang infers `[[gsl::Pointer]]` for `std::span`, `std::basic_string_view`,
  `std::reference_wrapper`, and iterators (`clang/lib/Sema/SemaAttr.cpp`).
  `SWIFT_NONESCAPABLE` expands to `swift_attr("~Escapable")`
  (`clang/lib/Headers/swift/bridging.h`).
- libc++'s `string_view` and `span` carry **no** `_LIBCPP_LIFETIMEBOUND` in this
  checkout; a design that relied on `lifetimebound` would reject every accessor.
- `Decl::isTemplated()` is `isDependentContext()` for a `FunctionDecl`
  (`clang/lib/AST/DeclBase.cpp`): false for instantiations and specializations.
- Coroutines copy parameters into the frame via
  `CoroutineBodyStmt::getParamMoves()` (`clang/include/clang/AST/StmtCXX.h`).
- SSAF's entity model names `FunctionDecl`, `ParmVarDecl`, `VarDecl`,
  `FieldDecl`, `RecordDecl`, and function return values; not ObjC methods,
  blocks, lambdas, or implicit declarations
  (`clang/lib/ScalableStaticAnalysis/Core/ASTEntityMapping.cpp`). Names are
  USRs, so template instantiations are distinct, nameable entities.
- The entity linker keeps the **first** contributor's summary for an
  external-linkage entity and silently drops later ones; duplicate internal or
  local entities are fatal (`EntityLinker::merge`). There is no union merge.
- `clang-ssaf-src-edit-merge` pre-deduplicates byte-identical replacements
  (`std::set<Replacement>`), drops every member of an overlapping cluster, and
  does **not** cluster two zero-length insertions at the same offset with
  different text (`offset < LastEnd`); those reach `Replacements::add` and fail
  with `insert_conflict`.
- Prefix attribute placement appertains to the parameter for named, unnamed,
  and function-pointer parameters in both spellings (verified against the
  built clang).
- `SSAFAnalysesCommon.h` provides `hasPtrOrArrType`;
  `clang/include/clang/Analysis/Analyses/LifetimeSafety/LifetimeAnnotations.h`
  provides `isGslPointerType`, `isGslOwnerType`, `isStdReferenceCast`,
  `isStdCallableWrapperType`.
- LLVM's libcall attribute inference (`llvm/lib/Transforms/Utils/BuildLibCalls.cpp`,
  `inferNonMandatoryLibFuncAttrs`) records which arguments of recognized C
  library functions are not captured; the optimizer already relies on it.

## 4. Architecture and data flow

Three new SSAF components plug into the unchanged clang-reforge pipeline:

```
per TU:  clang -c foo.cpp --ssaf-extract-summaries=ParameterEscape --ssaf-tu-summary-file=foo.json
            └─ ParameterEscapeExtractor → per-parameter escape facts
LU:      clang-ssaf-linker a.json b.json … → lu.json                      (existing)
LU:      clang-ssaf-analyzer -a NonEscapingParameters lu.json → wpa.json
            └─ ParameterEscapeAnalysis (SummaryAnalysis) aggregates facts
            └─ NonEscapingParametersAnalysis (DerivedAnalysis) runs the fixpoint
per TU:  clang -c foo.cpp --ssaf-source-transformation=infer-noescape
                         --ssaf-global-scope-analysis-result=wpa.json …
            └─ InferNoescape (Transformation) → foo.edits.yaml + foo.sarif
LU:      clang-ssaf-src-edit-merge → clang-reforge → clang-apply-replacements   (existing)
```

Division of responsibility:

- The **extractor** is the only component that understands language semantics.
  It reduces each parameter to a language-neutral lattice fact.
- The **fixpoint** is a generic graph closure with no knowledge of C++/ObjC.
- The **transformation** knows only how to spell and place the attribute.

Later milestones extend the extractor's classifier and the call-site resolver;
the fixpoint and emitter do not change.

Trust boundary — exactly two external sources are trusted:

1. `noescape` declared on a callee's visible declaration, from source or API
   Notes.
2. LLVM's libcall capture inference for callees recognized as C library
   functions (§5.2), excluding deallocation and reallocation functions.

Every other unanalyzed callee parameter is an escape.

Precondition: the link unit is the program (ODR holds; no other project
declares the link unit's functions by hand).

## 5. Components

### 5.1 Shared call-site resolution (`SSAFAnalysesCommon`) — lands first

```c++
struct CallSite {
  const FunctionDecl *Callee;                 // null ⇒ unresolvable
  const Expr *ImplicitObjectArg;              // member calls, member operator calls
  SmallVector<std::pair<const Expr *, unsigned>> Arguments; // arg ↔ param index
  SmallVector<const Expr *> UnmatchedArgs;    // variadic tail, arity mismatch, K&R
};
std::optional<CallSite> resolveCallSite(const Stmt *S);
// Handles CallExpr, CXXMemberCallExpr, CXXOperatorCallExpr, CXXConstructExpr.
```

It absorbs what `PointerFlowExtractor` does by hand today: `getDirectCallee`,
the `CXXOperatorCallExpr` first-argument offset for non-explicit-object member
operators, and the `matchesArgsWithParams` pairing loop. It makes explicit two
things `PointerFlow` currently discards: the implicit object argument and
unmatched arguments.

- `PointerFlow` migrates to it as a **behavior-preserving refactor**: it
  consumes `Arguments` only, ignoring `ImplicitObjectArg` and `UnmatchedArgs`.
  Its existing lit tests (`clang/test/Analysis/Scalable/PointerFlow`) and unit
  tests are the regression gate; expected diff is zero.
- The escape extractor consumes all four fields (§5.2).
- `CallGraph` stays on `clang::CallGraph`; it performs no argument matching.
- M2/M4 extend resolution (override sets, selector dispatch) inside this helper.

### 5.2 `ParameterEscapeExtractor` (per TU)

**Node identity.** A node is `(function EntityId, parameter index)`, with a
reserved index for the implicit object parameter (`this`). All redeclarations
share one node; `this` is first-class without entity-model changes.

**Populations.**

- *Analyzed* = every function definition in the TU the extractor can name,
  including inline functions and template instantiations from headers, subject
  to the existing `ExtractFromSystemHeaders` policy. Non-candidates are still
  analyzed; this is what resolves `v.push_back(p)` (escapes) and `s.size()`
  (does not) precisely.
- *Candidates* ⊂ analyzed = eligible for annotation in M1. The definition must
  be:
  - not virtual, not an ObjC method;
  - not templated in any sense: `isTemplated()` false, `getTemplateSpecializationKind() == TSK_Undeclared`,
    `isTemplateInstantiation()` false, `getInstantiatedFromMemberFunction()`
    null, `getTemplateInstantiationPattern()` null — instantiations share the
    pattern's source, so one instantiation's verdict must never edit the
    template;
  - not a coroutine;
  - with a written prototype (no K&R identifier-list definitions);
  - and every redeclaration visible in this TU (`FD->redecls()`) must be
    outside system headers, outside macro expansions, and outside dependent
    contexts.
- Bodiless declarations contribute nothing. (Their declared `noescape` is
  consumed at call sites; contributing a summary for them would shadow the
  definition's facts under first-wins merging.)

**Candidate parameter types (M1).** Object pointers (not function or block
pointers), lvalue/rvalue references, and *tracked views*: record types with
`isGslPointerType(T) || hasSwiftAttr(T, "~Escapable")` that are trivially
copyable and trivially destructible (`std::span`, `std::string_view` qualify).
Arrays decay to pointers. Owner records by value are not candidates. A
non-trivial view is neither a candidate type nor tracked (see the table).

**Aliases.** Per parameter, a flow- and path-insensitive over-approximation of
two kinds of expression, seeded with the parameter itself:

- *Value aliases*: values of pointer, reference, or tracked-view type carrying
  the parameter's provenance.
- *Place aliases*: glvalues denoting the object the parameter designates or a
  subobject of it: `*a`, `a->m`, `a[i]`, `(*a).m`, base/derived subobject
  conversions, and for a reference parameter the referenced object itself.

Derivations:

- From a place alias: `&place`, binding a reference to it, array-to-pointer
  decay of it, and returning it by reference produce value aliases. An
  lvalue-to-rvalue load from a place yields a **fresh value**, whatever its
  type (§1.1).
- From a value alias: parens; casts between pointer-carrying types; `a + n`,
  `a - n`; conditional and comma; reference casts (`std::move`, `std::forward`,
  `isStdReferenceCast`); assignment/initialization of a local scalar variable.
- *Call results*: the result of any call — including constructor calls,
  member calls, operators, and conversion functions — whose result type is
  pointer-carrying is a value alias if any alias was passed as an argument or
  as the implicit object. No annotation is consulted; this over-approximation
  is what makes `sv.data()`, `s[i]`, `v.begin()` sound without `lifetimebound`.
  A pointer-carrying result of a call with no alias arguments (`malloc`) is not
  an alias.
- *Storage and references*: only a local automatic, non-reference variable of
  pointer-carrying type is *local alias storage* — storing an alias into it is
  benign and it joins the alias set. A reference-typed variable, including a
  reference parameter, is never storage: writing through it writes the
  referent (a sink), and it joins the alias set only through its own
  initializer. Variables with `__block` or `cleanup` attributes are never
  storage. A structured-binding name resolves through its binding expression;
  a GNU statement expression has the alias kind of its last expression.
- *Tracked views*: a tracked-view value alias's pointer-carrying fields are
  aliases; copying or moving it (including via its trivial implicit copy/move
  members, recognized at the call site rather than as callee calls) yields an
  alias; constructing a tracked view with an alias argument yields an alias.
  Inside a member function of a tracked view, `this` is the alias node and
  `this->f` loads yield aliases of `this`. A view found *inside* a pointee
  (`a->view_member`) is loaded, hence fresh.

**Traversal coverage.** The visitor runs with implicit code visited and covers:
the function body; `CXXCtorInitializer`s (member, base, delegating);
`CXXDefaultInitExpr`; `CXXDefaultArgExpr`; implicit conversions and conversion
function calls; `CXXNewExpr` and `CXXDeleteExpr`; `CoroutineBodyStmt` (§table);
and, in ObjC++ TUs, `ObjCMessageExpr`, `BlockExpr`, `PseudoObjectExpr`,
`ObjCAtThrowStmt`, and `__block` variables. Anything a visitor method does not
recognize reaches the default row.

**Use classification.** Derivations (above) say which expressions *are*
aliases; the table below says what happens to a parameter when one of its
aliases is *used*. One expression can be both: `V v(p)` derives the alias `v`
and classifies `p`'s use as `FlowsTo(V::V, 0)`. Rows are tried top to bottom;
every use lands in the first matching row; the last row is the default. Using
an alias as a *location* — reading or writing through it — is benign for that
alias; the classification concerns what happens to the alias's *value*.

| Use of an alias | Class |
|---|---|
| function body is a `CoroutineBodyStmt` | every parameter and `this`: sink (`Coroutine`) |
| load through a place alias; compare; convert to bool; unevaluated operand; write *into* a place alias (the written value is classified separately) | benign |
| argument to a callee parameter declared `noescape` (source or API Notes) | benign |
| argument to a recognized C library function parameter that LLVM's libcall inference marks non-capturing, where the callee is not a deallocation/reallocation function (§ below) | benign |
| initialization or assignment of a local automatic scalar variable of pointer/reference/tracked-view type | benign (the variable joins the alias set) |
| copy or move of a **tracked view** through its trivial implicit copy/move members (implicit declarations are unnamable, so this is recognized at the call site) | benign (the copy is an alias) |
| store of an alias into `this->f` inside a **constructor** of a tracked view | `FlowsTo(self, this)` |
| argument paired by `resolveCallSite` to a callee parameter (including tracked-view constructors); implicit object argument of a member call | `FlowsTo(callee, idx)` / `FlowsTo(callee, this)` |
| `return` of an alias (by value or by reference) | `ReturnsSelf` |
| construction or copy of a **non-trivial** view from an alias | sink (`NonTrivialView`) |
| `new` expression with an alias argument | sink (`HeapAllocation`) |
| store to a field (any object, including `this` outside tracked-view constructors, and local aggregates), global, namespace-scope, static-local, or `thread_local`; store through any other lvalue; address of any alias-holding variable taken or bound to a reference | sink |
| `Callee == nullptr`; any `UnmatchedArgs` position; conversion to a non-pointer-carrying type; `throw`; `delete`; call to a deallocation/reallocation function; inline asm operand; `va_start`/`va_arg` | sink |
| lambda or block capture; virtual call; ObjC message send; `__block` variable; use of a callable-typed value *(M1 only)* | sink |
| anything else | sink (`UnrecognizedUse`, with statement class) |

**Library-function knowledge.** A bodiless callee (or clang builtin, by name
without the `__builtin_` prefix; never a definition, never under
`-fno-builtin`, and in C++ only `extern "C"`) is matched by name and arity
against a static table of C library functions. A table row marks a parameter
benign iff LLVM's own libcall inference (`inferNonMandatoryLibFuncAttrs` in
`llvm/lib/Transforms/Utils/BuildLibCalls.cpp`) gives it `captures(none)`;
deallocation and reallocation functions are listed so they can be refused
entirely, because `noescape` also forbids deallocation through the parameter.
Arguments LLVM marks only as `returned` (e.g. `memcpy`'s destination) are not
trusted. The table lives in `clangScalableStaticAnalysisAnalyses` with no LLVM
IR dependency — that library is linked into `clangDriver`, `clangFrontendTool`
and the `clang-ssaf-*` tools — and a unit test that does link
`LLVMTransformUtils` proves every row against LLVM, so the table cannot drift
unnoticed. This puts the analysis at parity with what the optimizer already
assumes about these functions.

**Summary** (`ParameterEscapeSummary`, keyed by the defining function's
entity):

```
struct FlowTarget { EntityId Callee; int ParamIndex; }          // ParamIndex == This for implicit object
struct Sink { EscapeReason Reason; SourceLocationRecord Location; }  // first sink only
struct EscapeFact {
  std::map<FlowTarget, SourceLocationRecord> FlowsTo;            // target → first call site
  std::optional<SourceLocationRecord> ReturnsSelfAt;             // an alias is returned
  std::optional<Sink> OtherSink;                                 // any non-return sink
};
struct ParameterEscapeSummary : EntitySummary {
  static constexpr llvm::StringLiteral Name = "ParameterEscape";
  std::vector<EscapeFact> Params;                                // by index
  std::optional<EscapeFact> This;
  bool IsCandidate;                                              // candidacy above
  std::vector<bool> CandidateParam;                              // per index: candidate type?
};
```

`EscapeReason` enumerates every sink row: `Return`, `StoreToField`,
`StoreToGlobal`, `StoreThroughPointer`, `AddressTaken`, `IndirectCall`,
`UnmatchedArgument`, `CastToNonPointer`, `Throw`, `Coroutine`, `Deallocation`,
`HeapAllocation`, `NonTrivialView`, `Asm`, `VarArgs`, `Capture`, `VirtualCall`,
`ObjCMessage`, `CallableUse`, `UnnamedCallee`, `UnrecognizedUse`.

Serialization: a JSON `FormatInfo` registered per the framework's pattern
(`clang/docs/ScalableStaticAnalysis/developer-docs/HowToExtend.md`), plus the
force-linker anchors in `BuiltinAnchorSources.def`.

**TU independence.** Facts depend only on the function body and the callee
declarations visible in the TU. Under ODR the first-wins merge therefore keeps
a correct summary. Two TUs may see different *declarations* (API Notes applied
only under modules; macro-conditional attributes); both resulting facts are
sound and first-wins picks one arbitrarily — a precision nondeterminism, not a
soundness issue. Divergent *bodies* are ODR violations and outside the
precondition; the linker has a TODO to diagnose them.

### 5.3 Whole-program analyses

Two analyses registered with `AnalysisRegistry`; no dependency on `CallGraph`
or `PointerFlow`. (`CallGraph` records function→function edges; the fixpoint
needs argument→parameter edges, which the extractor must compute anyway, and
`CallGraph` asserts against indirect calls and drops ObjC.)

**`ParameterEscapeAnalysis`** — `SummaryAnalysis<ParameterEscapeResult, ParameterEscapeSummary>`.
`add()` builds `std::map<Node, EscapeFact>` plus candidacy flags. `std::map`
ordering keeps output deterministic.

**`NonEscapingParametersAnalysis`** — `DerivedAnalysis<NonEscapingParametersResult, ParameterEscapeResult>`.

A return is an escape of the callee's *own* parameter but never of the
*caller's* argument: the caller already treats every pointer-carrying call
result as an alias (§5.2), and a return through a non-pointer-carrying type is
a sink in the callee. So:

```
EscapesForCaller(Q) := Q ∉ table                                   // unanalyzed, unnamed, skipped
                     ∨ OtherSink(Q)
                     ∨ ∃R ∈ FlowsTo(Q): EscapesForCaller(R)
Annotate(X)         := Candidate(X) ∧ ¬OtherSink(X) ∧ ¬ReturnsSelf(X)
                     ∧ ∀Q ∈ FlowsTo(X): ¬EscapesForCaller(Q)
```

- `initialize`: seed `EscapesForCaller` with every node satisfying one of the
  first two disjuncts; build reverse edges from each `FlowTarget` back to its
  sources.
- `step`: worklist. When `Q` becomes `EscapesForCaller`, every `P` with
  `Q ∈ FlowsTo(P)` does too, blamed on `Q`. Converges in O(nodes + edges).
  Cycles with no sink remain non-escaping — the correct least fixpoint, since
  escape derives only from positive evidence.
- `finalize`: evaluate `Annotate` over candidates.
  - `NonEscaping: std::map<EntityId, std::set<unsigned>>` — candidate
    functions → candidate parameter indices. `this` is excluded (nowhere to
    spell it).
  - `Rejected: std::map<Node, {EscapeReason, SourceLocationRecord, std::optional<Node> Blame}>`
    — candidates that failed `Annotate`, with one hop of blame (`Return` is a
    reason of its own).

### 5.4 `InferNoescape` transformation

Registered as `infer-noescape`; consumes `NonEscapingParametersResult`.

- Maps result entities to declarations as `CppBoundedBuffers` does
  (`Suite.getIdTable()` name→id, `ASTEntityMapping` on visited decls) and visits
  **every `FunctionDecl` redeclaration node** in the TU, emitting one
  replacement per listed parameter of each.
- **Placement:** prefix insertion at the parameter's begin location:
  `void f(int *p)` → `void f(__attribute__((noescape)) int *p)`. Valid for
  named, unnamed, and function-pointer parameters; needs no declarator parsing.
- **Spelling:** `__attribute__((noescape))` by default in every language, so
  a header shared by C and C++ TUs receives byte-identical insertions that the
  merge dedupes. Overridable via new `--ssaf-noescape-spelling=<text>` (driver
  + cc1; `SSAFOptions` string field), e.g. `[[clang::noescape]]`,
  `__noescape`, `NS_NOESCAPE`. The pipeline must pass the same value to every
  TU of a link unit; differing values at one site are not detected by the merge
  tool and fail later in `clang-apply-replacements` (§3). Making a macro's
  header available is the project's responsibility.
- **Skips:** parameter already has `NoEscapeAttr` → silent (idempotent);
  begin location in a macro expansion or system header → skip and report.
- **SARIF rule ids:** `noescape-inserted` (note, per edit, carrying the
  entity USR and edit location); `noescape-skipped` (warning, per unrewritable
  site, carrying the entity USR); `noescape-rejected` (note, once at the
  definition, with reason and blame — the precision-tuning feed).
- **Overlap with other transformations** is handled by policy, not code: run
  `infer-noescape` as its own clang-reforge pass.

### 5.5 Redeclarations outside the defining TU

Candidacy vetoes unrewritable redeclarations visible from the defining TU. A
redeclaration visible only in another TU is edited by that TU's pass; if it is
unrewritable there (macro-generated), it stays unedited. Consequences, by
language:

- **C:** none. Mismatched redeclarations merge to a composite type without the
  attribute in that TU (§3); callers there merely lose the guarantee. The
  header declaration Swift imports keeps it.
- **C++ / ObjC++:** a `conflicting types` error in the TU containing the
  unedited redeclaration, naming both declarations. It requires a candidate
  function, a proven parameter, and a macro-generated redeclaration in another
  TU — rare in C++ — and it is pre-announced by a `noescape-skipped` result in
  the merged SARIF.
- A header that is a system header for some TUs and a project header for
  others is **not** a failure: if any TU emits a site's edit, the file is
  edited; a skip for a site another TU covered is a false alarm.

Soundness is never affected. No mechanism is added. Recommended pipeline
policy (in clang-reforge, SARIF-only, no extra compile): refuse to apply a
merged edit set if any `noescape-skipped` site lacks a `noescape-inserted`
result at the same location.

### 5.6 Options

- New: `--ssaf-noescape-spelling=<text>`.
- Reused: `ExtractFromSystemHeaders`, `IncludeLocalEntities`.
- No aggressive mode, no hand-maintained allowlist. Parameters passed to
  un-annotated, non-library external functions are rejected; the remedy is
  annotating those declarations (API Notes for SDKs).

## 6. Soundness invariants

1. **Exhaustive classification.** The use visitor's default case is sink, and
   the traversal roots in §5.2 are all visited with implicit code enabled.
2. **Alias over-approximation.** No kills; conditionals add, never replace;
   every pointer-carrying call result with an alias argument is an alias.
3. **Trust only two external sources:** declared `noescape` on the callee's
   visible declaration, and LLVM's libcall capture inference minus
   deallocators. No reliance on `lifetimebound`.
4. **Unanalyzed ⇒ escapes**, at extraction (unresolvable callee) and at the
   fixpoint (missing node).
5. **TU-independent facts** under ODR (required by first-wins merging).
6. **Monotone precision.** Later milestones may only move uses from sink rows
   to more precise rows; each move is recorded in this spec's successor with
   its soundness argument.
7. **M1 exclusions.** No annotation on virtual, override, templated (pattern,
   instantiation, or specialization), coroutine, K&R, or ObjC functions.
8. **Loads are fresh.** A value loaded through an alias is never an alias.
9. **Field sensitivity only for tracked views**, which are trivially copyable
   and trivially destructible, so no hidden copy or destructor body exists.

## 7. Error handling and operations

- **Sink, don't abort.** Unmodeled constructs are the normal soundness path,
  not errors. Infrastructure failures degrade in the same direction: an
  unnamable function is not analyzed (callers treat its parameters as
  escaping); an unnamable callee makes the argument a sink. Both are logged via
  the downgradable `scalable-static-analysis-framework` diagnostic path. The
  compile never fails because of this feature.
- **`-Wlifetime-safety-noescape` is advisory only.** Its checker sees only
  return/field/global escapes of already-annotated parameters
  (`clang/lib/Analysis/LifetimeSafety/Checker.cpp`, `checkAnnotations`); it
  does not model retaining callees, coroutines, destructors, or casts.
  Silence proves nothing; a report is still a test failure to investigate.
- **Runbook.** Apply the pipeline policy of §5.5 before
  `clang-apply-replacements`.
- **Determinism.** Sorted containers throughout; stable JSON; one spelling per
  link unit.

## 8. Testing

Weighted toward negative cases: every sink row must be shown to reject, and
every adversarial finding that shaped this revision has a corresponding case.

**Unit tests** (`clang/unittests/ScalableStaticAnalysis/Analyses/`,
`…/SourceTransformation/`):

- `CallSiteResolver`: free call; member call; member vs. free `operator()`;
  explicit-object member; constructor; variadic tail → `UnmatchedArgs`;
  unprototyped C call; indirect call → null callee. Existing `PointerFlow` unit
  tests unchanged.
- Classifier, positive and negative per table row, plus:
  place vs. value aliases (`g = s->a` with an array member ⇒ sink;
  `int &r = *p; g = &r;` ⇒ sink; `g = p->next` ⇒ benign);
  tracked views (`string_view sv(p); return sv.data();` ⇒ `Return`;
  a view member storing `this->d` globally ⇒ `this` escapes;
  `puts(sv.data())` with `puts` non-capturing ⇒ accepted);
  non-trivial view construction ⇒ `NonTrivialView`;
  a view destructor or user copy constructor never consulted for tracked views
  (they cannot exist by the triviality predicate);
  constructor-initializer flows (`Holder(const char *p) : sv(p) {}` ⇒ sink);
  coroutine body ⇒ every parameter `Coroutine`;
  `new V(p)` ⇒ `HeapAllocation`;
  library knowledge (`strlen`, `memcpy` benign; `free`, `realloc` sinks;
  `strchr` argument not trusted);
  exhaustiveness invariant; candidate-type predicate (pointer, reference,
  trivially-copyable `gsl::Pointer` or `~Escapable` in; function pointer,
  owner, non-trivial view, scalar out); candidacy filters (virtual, template
  pattern, implicit and explicit instantiation, member of a class template
  specialization, coroutine, K&R, system header, macro redecl).
- Fixpoint, on hand-built summaries: chain; cycle without sink ⇒ non-escaping;
  cycle with sink; missing node ⇒ escape; `ReturnsSelf` rejects the candidate
  itself but never propagates to callers; one-hop blame; candidate-only
  filtering; determinism.
- Emitter: placement for named, unnamed, function-pointer, array parameters;
  default GNU spelling regardless of language; override; idempotency;
  skip-and-report on macro/system header; every redeclaration edited.

**Lit tests** (`clang/test/Analysis/Scalable/ParameterEscape/`,
`…/source-edit-generation/`), `split-file` multi-TU:

- Extraction JSON snapshots.
- `-a NonEscapingParameters` on linked inputs: callee in TU B decides the
  annotation in TU A; unanalyzed external ⇒ rejected; declared-`noescape`
  external via source attribute and via API Notes ⇒ accepted; ODR inline
  function present in both TUs; template-instantiation callees resolved
  (`std::vector<int*>::push_back(p)` ⇒ rejected, `std::span<int>(p, n).size()`
  ⇒ accepted); the `store<Drop>`/`store<Keep>` template case ⇒ not a candidate.
- Transformation: YAML for decl + def across TUs; a header shared by a C TU
  and a C++ TU produces one deduplicated insertion; all three SARIF rule ids.
- **End-to-end soundness oracle:** pipeline → merge → `clang-apply-replacements`
  → verify the number of inserted attributes in the rewritten tree equals the
  number of `noescape-inserted` results → recompile all TUs: `-fsyntax-only`
  passes; `-Wlifetime-safety-noescape -Werror` is clean; a second full run
  produces zero edits.
- Negative corpus: one file per sink kind asserting rejection with the expected
  reason. C (`-x c`) and C++ variants. `PointerFlow` lit output unchanged after
  the resolver migration.

**Corpus validator (non-lit, `clang/utils/ssaf/`):** compiles each
negative-corpus file with a driver that retains through the callee and
dereferences after return, under `-O2` and AddressSanitizer, and expects a
report — proving the corpus consists of true escapes. Run in CI, outside lit.

**M1 exit report (non-gating numbers, required artifact):** run the pipeline
on a real target and report candidates, annotated, and rejected-by-reason
counts from the merged SARIF. This is the baseline for M2+ precision work.

## 9. Delivery sequence

1. `resolveCallSite` helper + `PointerFlow` migration; all existing tests
   green; zero output diff.
2. `ParameterEscapeSummary`, JSON format, anchors, extractor with the M1
   classifier and library-function knowledge; unit + lit tests.
3. `ParameterEscapeAnalysis` and `NonEscapingParametersAnalysis`; unit + lit
   tests.
4. `InferNoescape` transformation and `--ssaf-noescape-spelling`; unit + lit
   tests.
5. End-to-end oracle test, negative corpus, corpus validator.
6. Documentation under `clang/docs/ScalableStaticAnalysis/user-docs/`; M1 exit
   report.

## 10. Roadmap beyond M1 (context only)

- **M2 Dispatch:** override-set closure for C++ virtuals; ObjC method entities
  in the entity model; selector-based dispatch in `resolveCallSite`; candidacy
  extended to methods whose entire override set is non-escaping.
- **M3 Callables and captures:** function pointers, blocks, lambdas,
  `std::function`-likes as candidates; invoked ≠ escaped; captured aliases
  escape iff the closure does.
- **M4 ObjC emission:** `NS_NOESCAPE` placement inside method-parameter type
  parentheses; ObjC override/protocol consistency.
- **Precision follow-ups:** conditional return edges (track a call result as
  an alias only if the callee actually returns the argument) to replace the
  over-approximation of §5.2; non-view aggregates holding aliases;
  `lifetime_capture_by(this)`; non-trivial views via analyzed copy/destructor
  members; a Core per-TU census summary if remote-redeclaration vetoes ever
  need automating.

## 11. Risks and open items

- **First-wins merge** blocks any per-site aggregation. Documented consequence
  in §5.5; an SSAF-level multi-contributor summary kind would remove it.
- **Precision** on real code may be low until M2/M3; the `noescape-rejected`
  feed and the M1 exit report exist to measure and direct that work. The
  call-result over-approximation rejects parameters whenever *any*
  pointer-carrying result of a call they were passed to is stored or returned,
  even if the callee did not return them.
- **`-isystem` variance** can make candidacy differ between TUs for the same
  header; first-wins picks one arbitrarily. Sound either way; may under-annotate.
- **Address-as-integer:** codegen's `captures(address)` semantics technically
  permit an integer copy of the address to escape. M1 treats conversion to a
  non-pointer type as a sink; relaxing this needs a Swift-side safety argument.
- **Merge-tool gap:** same-offset insertions with differing text are not
  clustered and fail late; fixing the predicate is a small SSAF improvement
  outside this feature's scope.
- **Library knowledge table drift:** the table is a copy of LLVM facts; the
  cross-check unit test is the only thing keeping it honest, so it must stay
  in the default unit-test target.
