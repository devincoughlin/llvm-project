#!/usr/bin/env python3
"""Run the ParameterEscape negative corpus under AddressSanitizer.

Every other test in this milestone checks the analysis against itself: the
extractor writes a fact, the fixpoint reads it, a CHECK line asserts the
verdict.  None of them can tell a *true* escape -- a program in which a pointer
outlives the object it points at -- from a program this analysis merely
refuses.  A corpus of the second kind would make every precision measurement in
the milestone meaningless while every test stayed green.

This script is the independent oracle.  Each negative-corpus file that carries
``// DRIVER:`` lines is compiled together with that driver at ``-O2 -g
-fsanitize=address`` and must produce an AddressSanitizer report.  ASan knows
nothing about ParameterEscape, so a report is evidence from outside the
analysis that the file demonstrates a real escape.

``-O2`` is part of the claim, not a detail: an escape the optimizer can delete
was never observable in the first place.

Three directives, all in comments so that lit never sees them:

``// DRIVER: <code>``
    One or more lines, joined in order, appended to the file.  A ``main`` that
    lets the callee retain a pointer into a dead object and then dereferences
    it, plus whatever definitions the corpus file deliberately left bodiless.

``// DRIVER-KIND: <kind>``
    The AddressSanitizer report kind that must be produced, exactly.  Defaults
    to ``stack-use-after-scope``.

``// DRIVER-EVIDENCE: <substring>``
    A substring the report must contain, tying the report to *this file's*
    escape.  Defaults to ``'local'`` -- ASan names the object in the frame
    description, so requiring the name is what stops an unrelated bug in the
    driver from certifying the file.

    An override has a floor: the substring must **not** appear in the
    reference report for its kind, which this script produces itself from a
    program that has nothing to do with the corpus.  A phrase that every
    report of that kind contains would make the check vacuous while reading
    like an attribution, and both overrides in the corpus were exactly that
    before the floor existed (``freed by thread`` in every
    heap-use-after-free, ``is located in stack of thread`` in every
    stack-use-after-scope).

The last two exist because "some AddressSanitizer report was produced" is not
the claim.  Without them a file whose parameter does not escape at all passes,
as long as its driver contains any memory error:

    int sink;
    void escape(int *p) { sink = *p; }        // p does not escape
    // DRIVER: int *volatile leak;
    // DRIVER: int main() { int l = 1; escape(&l);
    // DRIVER:   { int unrelated = 2; leak = &unrelated; }
    // DRIVER:   return *leak; }

That reports ``stack-use-after-scope`` on ``'unrelated'`` and would certify a
*precision loss* as a true escape, which is the exact inversion this script
exists to prevent.

A file that cannot be driven says so with ``// NO-DRIVER: <reason>``.  One of
the two is required of every corpus file: a blanket exemption for "everything
without a driver" would let the corpus lose its drivers one at a time without
any run ever noticing.

A file whose extension is not one of the four this script knows how to compile
is ignored: it cannot be a corpus case.

Usage:
    validate_escape_corpus.py <clang> <corpus-dir> [<corpus-dir>...]

Options:
    --isysroot <path>   passed through to the compiler.  On Darwin this is
                        detected with ``xcrun --show-sdk-path`` when not given,
                        because a just-built in-tree clang does not find the
                        SDK on its own.
    --keep              leave the generated programs on disk and print where.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

# Only these are compiled.  A file with a DRIVER line and some other extension
# is an error rather than a skip, so that a new language in the corpus cannot
# silently stop being validated.
#
# `--driver-mode=g++` rather than `-x c++` alone: one `clang` binary is passed
# in, and its C driver links no C++ runtime.  Without this, throw.cpp's driver
# fails to link against `__cxa_throw` and typeinfo and is reported as a corpus
# failure -- measured, and the reason the flag is here.
#
# `-fblocks` is on for C too: block-capture.c uses a block literal.
LANG_FLAGS = {
    ".c": ["-x", "c", "-std=c17", "-fblocks"],
    ".cpp": ["--driver-mode=g++", "-x", "c++", "-std=c++20", "-fexceptions"],
    ".m": ["-x", "objective-c", "-fblocks", "-fobjc-arc"],
    ".mm": [
        "--driver-mode=g++",
        "-x",
        "objective-c++",
        "-std=c++20",
        "-fblocks",
        "-fobjc-arc",
    ],
}

# `[ \t]*` rather than `\s*`: `\s` matches a newline, so `\s*` would let
# `//\nDRIVER:` count.
DRIVER_RE = re.compile(r"^//[ \t]*DRIVER:[ \t]?(.*)$", re.M)
KIND_RE = re.compile(r"^//[ \t]*DRIVER-KIND:[ \t]*(\S+)[ \t]*$", re.M)
EVIDENCE_RE = re.compile(r"^//[ \t]*DRIVER-EVIDENCE:[ \t]*(.+?)[ \t]*$", re.M)
NO_DRIVER_RE = re.compile(r"^//[ \t]*NO-DRIVER:[ \t]*(\S.*?)[ \t]*$", re.M)

DEFAULT_KIND = "stack-use-after-scope"
DEFAULT_EVIDENCE = "'local'"

# `SEGV` and `SEGV on unknown address` both start with an upper-case word, so
# the character class has to admit them; with `[a-z0-9-]+` a segfault printed
# as an unnamed kind and compared equal to nothing.
KIND_IN_REPORT_RE = re.compile(r"ERROR: AddressSanitizer: ([A-Za-z0-9_-]+)")

# A compile or a run that has not finished in this many seconds is hung.
TIMEOUT = 300


class Failure(Exception):
    pass


def compile_and_run(clang, base_flags, workdir, name, text):
    """Compile `text` as `name`, run it, and describe what happened.

    Returns (reported, kind, output): whether AddressSanitizer reported at all,
    the kind it reported (empty when it did not), and the program's combined
    output.
    """
    src = workdir / name
    src.write_text(text)
    exe = workdir / (src.stem + ".exe")
    flags = LANG_FLAGS[src.suffix]
    try:
        cc = subprocess.run(
            [clang, *flags, *base_flags, str(src), "-o", str(exe)],
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        raise Failure(f"did not compile within {TIMEOUT}s")
    if cc.returncode != 0:
        raise Failure("does not compile:\n" + cc.stderr.rstrip())
    try:
        run = subprocess.run(
            [str(exe)], capture_output=True, text=True, timeout=TIMEOUT
        )
    except subprocess.TimeoutExpired:
        raise Failure(f"did not terminate within {TIMEOUT}s")
    combined = run.stdout + run.stderr
    m = KIND_IN_REPORT_RE.search(combined)
    return m is not None, (m.group(1) if m else ""), combined


# One reference program per report kind, with nothing in common with any corpus
# file.  Each serves two purposes: the stack one is the self-check that proves
# this compiler reports at all, and both are the floor a DRIVER-EVIDENCE
# override has to clear -- a phrase that appears in the reference report of its
# own kind is generic to the kind and attributes nothing.
#
# The object is deliberately *not* called `local`, so that the default evidence
# `'local'` clears the same floor as every override rather than being exempt
# from it.
REFERENCE_PROGRAMS = {
    "stack-use-after-scope": """
int *g;
__attribute__((noinline)) void keep(int *p) { g = p; }
int main() {
  int *volatile k;
  { int selfcheck_subject = 1; keep(&selfcheck_subject); k = g; }
  return *k;
}
""",
    "heap-use-after-free": """
__attribute__((noinline)) void release(int *p) { delete p; }
int main() {
  int *volatile p = new int(1);
  release((int *)p);
  return *p;
}
""",
}

SELFCHECK_ESCAPING = REFERENCE_PROGRAMS["stack-use-after-scope"]
SELFCHECK_SUBJECT = "'selfcheck_subject'"

SELFCHECK_CLEAN = """
int *g;
__attribute__((noinline)) void keep(int *p) { g = p; }
int main() {
  int outer = 1;
  keep(&outer);
  int *volatile k = g;
  return *k - 1;
}
"""


def selfcheck(clang, base_flags, workdir):
    """Prove that this compiler reports the escapes we are about to ask about.

    Without this the script has a silent-success mode that is indistinguishable
    from a green run: a compiler with no ASan runtime, an ASan build with
    use-after-scope detection off, or a `detect_stack_use_after_return`
    environment that suppresses the report would all turn "every file escapes"
    into "no file reported, so nothing failed" if the absence of a report were
    ever treated as anything but a failure.  It is checked in both directions
    because a compiler that reported on *everything* would pass the corpus
    without measuring it.

    The escaping half also pins the two things every corpus file is checked
    against: that the kind is `stack-use-after-scope` and that the report names
    the object in its frame description.

    Returns that report, which is also the reference program's report for
    `stack-use-after-scope`: the same program serves both purposes, so the
    caller seeds the floor cache with it rather than compiling it twice.
    """
    try:
        reported, kind, escaping_out = compile_and_run(
            clang, base_flags, workdir, "selfcheck_escaping.cpp", SELFCHECK_ESCAPING
        )
    except Failure as e:
        raise Failure(
            "cannot build and run an AddressSanitizer program with this\n"
            "compiler.  A clang built without compiler-rt has no ASan runtime;\n"
            "pass one that does (for example /usr/bin/clang, or configure with\n"
            "-DLLVM_ENABLE_RUNTIMES=compiler-rt).\n" + str(e)
        )
    out = escaping_out
    if not reported:
        raise Failure(
            "the self-check program escapes a pointer to a dead local and this\n"
            "compiler reported nothing.  Every corpus result below would be\n"
            "meaningless, so nothing was run.  Output was:\n" + out.rstrip()
        )
    if kind != DEFAULT_KIND:
        raise Failure(
            f"the self-check program escapes a pointer to a dead local and this\n"
            f"compiler reported {kind!r} rather than {DEFAULT_KIND!r}, so the\n"
            f"kind every corpus file is checked against means something else\n"
            f"here.  Output was:\n" + out.rstrip()
        )
    if SELFCHECK_SUBJECT not in out:
        raise Failure(
            f"the self-check program's report does not name "
            f"{SELFCHECK_SUBJECT},\n"
            f"so this compiler's reports carry no frame description and the\n"
            f"attribution every corpus file is checked against cannot work.\n"
            f"Output was:\n" + out.rstrip()
        )
    try:
        reported, kind, out = compile_and_run(
            clang, base_flags, workdir, "selfcheck_clean.cpp", SELFCHECK_CLEAN
        )
    except Failure as e:
        raise Failure("the clean self-check program " + str(e))
    if reported:
        raise Failure(
            "the self-check program keeps its pointee alive and this compiler\n"
            "still reported " + kind + ".  A compiler that reports on\n"
            "everything cannot distinguish the corpus from anything else, so\n"
            "nothing was run.  Output was:\n" + out.rstrip()
        )
    return escaping_out


def reference_report(clang, base_flags, workdir, kind, cache):
    """The report a program of \p kind produces, for the DRIVER-EVIDENCE floor.

    Built once per kind and only for kinds the corpus actually uses.  A kind
    with no reference program is refused rather than floored at nothing: the
    floor is the only thing standing between an override and a phrase that
    attributes nothing.
    """
    if kind in cache:
        return cache[kind]
    program = REFERENCE_PROGRAMS.get(kind)
    if program is None:
        raise Failure(
            f"no reference program for DRIVER-KIND {kind!r}.  Add one to "
            f"REFERENCE_PROGRAMS so that a DRIVER-EVIDENCE for this kind can "
            f"be checked against it"
        )
    name = "reference_" + kind.replace("-", "_") + ".cpp"
    reported, got, out = compile_and_run(clang, base_flags, workdir, name, program)
    if not reported or got != kind:
        raise Failure(
            f"the reference program for {kind!r} reported "
            f"{got or 'nothing'}, so the floor for DRIVER-EVIDENCE cannot be "
            f"computed.  Output was:\n" + out.rstrip()
        )
    cache[kind] = out
    return out


def driver_of(text):
    """The driver source of a corpus file, or None.

    Several `// DRIVER:` lines are joined in order, so a driver can be written
    as readable code rather than crammed onto one line.
    """
    parts = DRIVER_RE.findall(text)
    return "\n".join(parts) if parts else None


def check_one(clang, base_flags, work, src, text, driver, refs):
    """Validate one corpus file.  Returns None on success, or why it failed."""
    # A driver belongs only to a file asserting an escape.  A file under
    # Positive/ asserts the opposite -- that the parameter is annotated -- so
    # an ASan report there would be a false negative in the analysis, which
    # this script would otherwise record as a pass.
    if src.parent.name != "Negative":
        return (
            "DRIVER outside Negative/: a driver asserts that the file escapes, "
            "which is not what this directory means"
        )
    kind_m = KIND_RE.search(text)
    want_kind = kind_m.group(1) if kind_m else DEFAULT_KIND
    ev_m = EVIDENCE_RE.search(text)
    want_evidence = ev_m.group(1) if ev_m else DEFAULT_EVIDENCE
    try:
        reference = reference_report(clang, base_flags, work, want_kind, refs)
    except Failure as e:
        return str(e)
    if want_evidence in reference:
        return (
            f"DRIVER-EVIDENCE {want_evidence!r} appears in the reference "
            f"report for {want_kind}, so every report of that kind contains "
            f"it and the check would attribute nothing.  Choose a substring "
            f"that this file's report has and a generic one does not"
        )
    try:
        reported, kind, out = compile_and_run(
            clang, base_flags, work, src.name, text + "\n" + driver + "\n"
        )
    except Failure as e:
        return "the file and its driver " + str(e)
    if not reported:
        return (
            "no AddressSanitizer report: this file's driver does not "
            "demonstrate an escape.  Either the program is not a true escape "
            "-- in which case the analysis rejecting it is a precision loss "
            "and the corpus asserts the wrong thing -- or the escape is not "
            "reachable from main in a way ASan can see.  Program output "
            "was:\n" + out.rstrip()
        )
    if kind != want_kind:
        return (
            f"reported {kind!r}, not {want_kind!r}.  The driver tripped "
            f"AddressSanitizer for some other reason than the escape this "
            f"file asserts; say so with // DRIVER-KIND: if the change is "
            f"intended.  Program output was:\n" + out.rstrip()
        )
    if want_evidence not in out:
        return (
            f"reported {kind} but the report does not contain "
            f"{want_evidence!r}, so nothing ties it to this file's escape.  "
            f"Program output was:\n" + out.rstrip()
        )
    return None


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("clang")
    ap.add_argument("corpus", nargs="+")
    ap.add_argument("--isysroot")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv)

    isysroot = args.isysroot
    if isysroot is None and sys.platform == "darwin":
        sdk = subprocess.run(
            ["xcrun", "--show-sdk-path"], capture_output=True, text=True
        )
        if sdk.returncode == 0:
            isysroot = sdk.stdout.strip()
    base_flags = ["-O2", "-g", "-fsanitize=address", "-fno-omit-frame-pointer"]
    if isysroot:
        base_flags += ["-isysroot", isysroot]

    files = []
    for root in args.corpus:
        p = pathlib.Path(root)
        if not p.is_dir():
            print(f"error: not a directory: {p}", file=sys.stderr)
            return 2
        files += [f for f in p.rglob("*") if f.is_file()]
    files.sort()

    work = pathlib.Path(tempfile.mkdtemp(prefix="ssaf-escape-corpus-"))
    try:
        try:
            # The self-check's escaping program *is* the reference program for
            # stack-use-after-scope, so its report seeds the floor cache rather
            # than being compiled a second time.
            refs = {"stack-use-after-scope": selfcheck(args.clang, base_flags, work)}
        except Failure as e:
            print(f"error: {e}", file=sys.stderr)
            return 2

        validated, exempt, failures, seen = [], [], [], 0
        for src in files:
            if src.suffix not in LANG_FLAGS:
                continue
            seen += 1
            text = src.read_text()
            driver = driver_of(text)
            if driver is None:
                # Not a blanket exemption: the file has to say, in itself, why
                # it cannot be driven.  Deleting a DRIVER block therefore turns
                # that file red rather than quietly shrinking what is checked.
                no = NO_DRIVER_RE.search(text)
                if not no:
                    failures.append(
                        (
                            src,
                            "neither a // DRIVER: block nor a // NO-DRIVER: "
                            "reason.  Every corpus file must state which it is",
                        )
                    )
                else:
                    exempt.append((src, no.group(1)))
                continue
            why = check_one(args.clang, base_flags, work, src, text, driver, refs)
            if why:
                failures.append((src, why))
            else:
                validated.append(src)
                print(f"escapes: {src.name}")

        for src, reason in exempt:
            print(f"not runtime-validated: {src.name}: {reason}")
        for src, why in failures:
            print(f"FAIL {src.name}: {why}")

        # Validating nothing must not look like validating everything.  This
        # project has no CI; a run that quietly found no work would be the only
        # thing standing between a deleted corpus and a green report.
        if seen == 0:
            print(
                "error: no compilable file was found under "
                + ", ".join(args.corpus),
                file=sys.stderr,
            )
            return 2
        if not validated and not failures:
            print(
                "error: every file under "
                + ", ".join(args.corpus)
                + " is exempt; nothing was validated",
                file=sys.stderr,
            )
            return 2

        print(
            f"{len(validated)} validated, {len(failures)} failure(s), "
            f"{len(exempt)} not runtime-validated"
        )
        return 1 if failures else 0
    finally:
        if args.keep:
            print(f"generated programs left in {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
