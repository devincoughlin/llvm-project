# Summary Extraction

:::{WARNING}
The framework is rapidly evolving.
The documentation might be out-of-sync with the implementation.
The purpose of this documentation is to give context for upcoming reviews.
:::

## Command-line interface

Two flags control summary extraction:

- `--ssaf-extract-summaries=<name1>,<name2>,...`: Comma-separated list of summary extractor names to enable.
- `--ssaf-tu-summary-file=<path>.<format>`: Output file for the extracted summaries. The file extension selects the serialization format (e.g. `.json`).
- `--ssaf-list-extractors`: List the available summary extractors.
- `--ssaf-list-formats`: List the available serialization formats.

Example invocation:

```bash
clang --ssaf-extract-summaries=MyAwesomeAnalysis \
      --ssaf-tu-summary-file=my-tu-summary.json \
      -c input.cpp -o input.o

clang --ssaf-list-extractors --ssaf-list-formats
```

## Diagnostics

In case the `--ssaf-*` flags are used incorrectly, or some extractor fails to implement the desired serialization format
or just happens to have an error, then the error is forwarded as a `scalable-static-analysis-framework` error.
These errors can be downgraded into warnings using `-Wno-error=scalable-static-analysis-framework`.
These errors can be completely suppressed using `-Wno-scalable-static-analysis-framework`.

See the [diagnostic flags](https://clang.llvm.org/docs/DiagnosticsReference.html#wscalable-static-analysis-framework) for the full list of diagnostics controlled by `-Wscalable-static-analysis-framework`.

## Translation units that fail to compile

A translation unit that fails to compile produces **no summary at all**: no file is written to the path given by `--ssaf-tu-summary-file=`.
Its facts would come from an AST built by error recovery, where a use the analysis relies on seeing may simply be absent — and for an
analysis such as `ParameterEscape`, a missing use is a missing escape, so the summary could assert that a parameter is clean when it is
only clean because the code did not parse.

Two consequences are worth knowing:

- **The refusal is unconditional with respect to the warning flags above.** `-Wno-error=scalable-static-analysis-framework` and
  `-Wno-scalable-static-analysis-framework` change the severity of the report, or silence it, but never cause a summary to be written.
  If you silence the group to reduce build noise and then find no summary file, the extractor is working as intended.
- **After a fatal error the refusal is not reported.** Clang silences every diagnostic that follows a fatal one, and the refusal is one of
  them. So a missing `#include`, or crossing `-ferror-limit`, yields no summary and no message saying why. The compilation itself has
  already failed and exited non-zero, which is the signal to rely on.

A file that already exists at the `--ssaf-tu-summary-file=` path is left untouched when the summary is refused; extraction never removes
or rewrites it. Use a fresh output path per invocation, which a successful extraction requires in any case — it reports
`failed to write file …: file already exists` rather than overwriting.

