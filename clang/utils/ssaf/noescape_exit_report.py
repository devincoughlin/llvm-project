#!/usr/bin/env python3
"""Aggregate infer-noescape SARIF reports over a tree.

The transformation writes one SARIF file per translation unit
(``--ssaf-transformation-report-file=``) carrying three rule ids:

    noescape-inserted   an attribute was written at this declaration
    noescape-skipped    a declaration that could not be edited; if some other
                        translation unit did edit a redeclaration of the same
                        function, the two now disagree (design section 5.5)
    noescape-rejected   a candidate parameter the analysis refused, with the
                        reason and the location that decided it

Per run this prints the count of each rule and, for the rejections, the reasons
sorted by frequency.  That ranking is the milestone's exit evidence and the
prioritization feed for later precision work: the reason at the top of the list
is the one that would buy the most annotations.

Counts are per *translation unit*: a rejection is reported once per TU that
sees the definition, so a function in a widely included header is counted many
times.  ``--blame`` below reads the whole-program result instead, where each
node appears once.

Usage:
    noescape_exit_report.py <directory> [<directory>...]

Options:
    --top N          show only the N most frequent rejection reasons.  It does
                     not apply to the blame list; that has --blame-top.
    --blame <wpa>    rank the callee parameters most often blamed for an
                     ``EscapesViaCallee``, read from the whole-program result
                     (``clang-ssaf-analyzer``'s ``-o`` file).  Rejections
                     blamed on a callee for any *other* reason --
                     ``UnanalyzedCallee``, whose callee is simply not in the
                     link unit -- are counted separately and not ranked, since
                     the action they call for is different.

                     It reads the whole-program result rather than the SARIF
                     for two reasons.  Each node appears once there, where the
                     SARIF reports it once per translation unit that sees the
                     definition, so a header-declared function is not counted
                     dozens of times.  And the blamed callee's identity is not
                     in the SARIF message at all: InferNoescape.cpp writes only
                     " via callee parameter <n>".  (That is a property of the
                     message, not an unavoidable one -- the transformation
                     holds the id table and could render the callee.)
    --blame-top N    how many blame rows to print.  Default 10.
"""

import argparse
import collections
import json
import pathlib
import re
import sys

RULES = ("noescape-inserted", "noescape-skipped", "noescape-rejected")

# `parameter 0 of c:@F@f#*I# not annotated: StoreToGlobal at a.cpp:3:7 (g)`,
# optionally followed by ` via callee parameter 0`.  Anchored on the literal
# the transformation emits; see InferNoescape.cpp reportRejected().
#
# The USR is matched lazily rather than as `\S+`: a USR can contain a space
# (`...@F@operator delete#*v#S`, `...@F@skip_while# #*1C#`), and `\S+` dropped
# nine rejections out of 10400 over llvm/lib/Support before this was fixed.
REJECT_RE = re.compile(
    r"parameter (?P<param>-?\d+) of (?P<usr>.+?) not annotated: "
    r"(?P<reason>\w+) at (?P<loc>.*?)(?: \((?P<detail>.*)\))?"
    r"(?: via callee parameter (?P<blame>-?\d+))?$"
)


def results(path):
    """Yield (ruleId, message) from one SARIF file."""
    try:
        doc = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        print(f"warning: {path}: {e}", file=sys.stderr)
        return
    for run in doc.get("runs", []):
        for res in run.get("results", []):
            yield res.get("ruleId", ""), res.get("message", {}).get("text", "")


def blame_ranking(path, top):
    """Rank blamed callee parameters in a whole-program result file.

    The result nests the id table and the verdict at depths this script has no
    business knowing, so both are found by walking the document.
    """
    doc = json.loads(path.read_text())
    names, verdicts = {}, []

    def walk(o):
        if isinstance(o, dict):
            if (
                "id" in o
                and isinstance(o.get("name"), dict)
                and "usr" in o["name"]
            ):
                names[o["id"]] = o["name"]["usr"]
            if "non_escaping" in o and "rejected" in o:
                verdicts.append(o)
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(doc)
    if not verdicts:
        return None
    blamed = collections.Counter()
    other_blamed = collections.Counter()
    for v in verdicts:
        for rej in v["rejected"]:
            b = rej.get("blame")
            if not b:
                continue
            key = (names.get(b["function"]["@"], "?"), b["param"])
            if rej.get("reason") == "EscapesViaCallee":
                blamed[key] += 1
            else:
                other_blamed[rej.get("reason", "?")] += 1
    return blamed.most_common(top), other_blamed


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--top", type=int)
    ap.add_argument("--blame", metavar="WPA")
    ap.add_argument("--blame-top", type=int, default=10)
    args = ap.parse_args(argv)

    files = []
    for root in args.roots:
        p = pathlib.Path(root)
        # A path that does not exist used to become a one-element file list, so
        # a typo in the directory name printed three zeroes and exited 0 --
        # indistinguishable from a run in which everything was accepted.
        if not p.exists():
            print(f"error: no such path: {p}", file=sys.stderr)
            return 2
        files += sorted(p.rglob("*.sarif")) if p.is_dir() else [p]

    # An empty tree produces three zeroes, which reads exactly like a run in
    # which everything was accepted.  This has no CI behind it, so say so and
    # exit non-zero instead.
    if not files:
        print(
            "error: no .sarif file found under " + ", ".join(args.roots),
            file=sys.stderr,
        )
        return 2

    rules = collections.Counter()
    reasons = collections.Counter()
    other = collections.Counter()
    unparsed = 0
    for path in files:
        for rule, text in results(path):
            rules[rule] += 1
            if rule not in RULES:
                other[rule] += 1
                continue
            if rule != "noescape-rejected":
                continue
            m = REJECT_RE.search(text)
            if not m:
                # Counted rather than dropped: a message the transformation
                # changed shape on must not quietly shrink the ranking below.
                unparsed += 1
                continue
            reasons[m.group("reason")] += 1

    print(f"{len(files)} SARIF file(s)")
    for rule in RULES:
        print(f"{rule}: {rules[rule]}")
    parsed = sum(reasons.values())
    if parsed or unparsed:
        # Both numbers, always: percentages are over the messages that parsed,
        # and printing only the rule total above them would make a shortfall
        # look like a rounding error.
        print(
            f"rejection reasons ({parsed} of "
            f"{rules['noescape-rejected']} parsed):"
        )
        listed = reasons.most_common(args.top) if args.top else reasons.most_common()
        for reason, n in listed:
            print(f"  {reason}: {n} ({100.0 * n / parsed:.1f}%)")
        if unparsed:
            print(f"  <unparsed message>: {unparsed}")
    for rule, n in sorted(other.items()):
        print(f"other rule {rule}: {n}")

    if args.blame:
        wpa = pathlib.Path(args.blame)
        if not wpa.is_file():
            print(f"error: no such whole-program result: {wpa}", file=sys.stderr)
            return 2
        result = blame_ranking(wpa, args.blame_top)
        if result is None:
            print(
                f"error: {wpa} holds no NonEscapingParametersResult",
                file=sys.stderr,
            )
            return 2
        ranking, other_blamed = result
        print("blamed callee parameters:")
        for (usr, param), n in ranking:
            print(f"  {usr} param {param}: {n}")
        for reason, n in sorted(other_blamed.items()):
            print(f"blamed for {reason} (not ranked): {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
