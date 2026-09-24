//===- SrcEditMerge.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// clang-ssaf-src-edit-merge: per-LU source-edit YAML merge tool.
//
// Reads N per-TU clang::tooling::TranslationUnitReplacements YAML files,
// deduplicates and merges them into one flat, conflict-resolved list (see
// "Conflict policy" below), and writes a single merged YAML spanning all N
// TUs. The tool does NOT write source files — applying the merge result is
// the caller's responsibility (typically clang-reforge invokes
// `clang-apply-replacements` after this tool returns).
//
// Conflict policy: this tool implements a drop-all policy. For each file, a
// maximal group of transitively-overlapping input Replacements (a cluster)
// is computed directly from the input; if a cluster has more than one
// member, every member is removed from the merged output — not just enough
// of them to resolve the overlap. A one-line stderr summary is emitted per
// dropped cluster, and the tool still exits 0. Any input file that does not
// exist on disk is excluded entirely from the merged output.
//
// Every path in every input — each Replacement's FilePath and each TU's
// MainSourceFile — is canonicalized to an absolute path (symlinks resolved)
// as soon as its input is read, before anything compares two of them.
// Per-TU YAML records paths as the compiler spelled them, so two TUs that
// reach one shared header through different spellings — a quoted relative
// include in one, an -I search path in the other — would otherwise land in
// different per-file buckets, never be deduplicated or compared, and both be
// applied to the same file. The merged YAML therefore carries only absolute
// paths and is meaningful from any working directory.
//
// Canonicalization may rename a path; it never creates existence. Whether a
// Replacement's file exists is decided on the spelling the input gave, before
// that spelling is replaced, so a spelling the base tool refused — '',
// 'foo.cpp/', 'foo.cpp/.' — is still refused even though real_path or
// make_absolute would resolve it to something that does exist. The canonical
// spelling is the key; the raw spelling is what a missing-file diagnostic
// names.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/Version.h"
#include "clang/Tooling/ReplacementsYaml.h" // IWYU pragma: keep
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace cl = llvm::cl;

//===----------------------------------------------------------------------===//
// Error Messages
//===----------------------------------------------------------------------===//

constexpr const char *ToolName = "clang-ssaf-src-edit-merge";

constexpr const char *CannotReadInput = "cannot read {0}: {1}";

constexpr const char *InvalidReplacementsYaml =
    "{0}: invalid TranslationUnitReplacements YAML";

constexpr const char *ConflictClusterSummary =
    "conflict: skipped {0} overlapping replacement(s) at {1}:{2}";

constexpr const char *CannotWriteFile = "cannot write {0}";

constexpr const char *WriteErrorOnFile = "write error on {0}";

constexpr const char *CannotWriteOutput = "cannot write {0}: {1}";

constexpr const char *MissingReplacementFile =
    "{0}: file does not exist; skipping its replacement(s)";

constexpr const char *CandidateEditMessage = "candidate edit: \"{0}\"";

constexpr const char *ConflictSarifMessage =
    "{0} overlapping replacement(s) at {1} byte {2} were dropped; resolve "
    "manually.";

cl::OptionCategory MergeCategory("clang-ssaf-src-edit-merge options");

cl::list<std::string> InputFiles(cl::Positional, cl::OneOrMore,
                                 cl::desc("<input.yaml>..."),
                                 cl::cat(MergeCategory));

cl::opt<std::string> OutputFile("o", cl::Required, cl::value_desc("path"),
                                cl::desc("Output path for the merged YAML."),
                                cl::cat(MergeCategory));

cl::opt<std::string> SarifConflictsOut(
    "sarif-conflicts-out", cl::value_desc("path"),
    cl::desc("Optional path. When supplied, write a SARIF document "
             "listing conflict clusters dropped from the merged output."),
    cl::cat(MergeCategory));

/// Read one input YAML into a TranslationUnitReplacements.
///
/// Returns true on success. On failure, prints a one-line diagnostic to
/// stderr and returns false; the caller surfaces this as a non-zero exit.
bool readInput(llvm::StringRef Path,
               clang::tooling::TranslationUnitReplacements &Out) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> Buffer =
      llvm::MemoryBuffer::getFile(Path);
  if (std::error_code EC = Buffer.getError()) {
    llvm::errs() << ToolName << ": "
                 << llvm::formatv(CannotReadInput, Path, EC.message()) << "\n";
    return false;
  }
  llvm::yaml::Input YAML(Buffer.get()->getBuffer());
  YAML >> Out;
  if (YAML.error()) {
    llvm::errs() << ToolName << ": "
                 << llvm::formatv(InvalidReplacementsYaml, Path) << "\n";
    return false;
  }
  return true;
}

/// The merged output: a flat list of Replacements spanning every input file.
///
/// Deliberately not clang::tooling::TranslationUnitReplacements: that type
/// represents one TU's replacements, but this represents the drop-all-policy
/// result of merging N TUs' worth of edits. `MappingTraits` below serializes
/// this to the identical YAML shape (the same two required keys,
/// MainSourceFile and Replacements) so clang-apply-replacements — which only
/// knows how to read that shape — can still consume the output; only the
/// C++ type stops making a claim about the data it doesn't hold.
/// MainSourceFile is populated on a best-effort basis (computeMainSourceFile,
/// below) purely for wire compatibility: nothing in the merge/apply pipeline
/// reads it back.
struct MergedReplacements {
  std::string MainSourceFile;
  std::vector<clang::tooling::Replacement> Replacements;
};

/// Compute the shared MainSourceFile across inputs.
///
/// Per spec: if every input declares the same MainSourceFile, use that;
/// otherwise use the empty string. The comparison is on the raw strings, so
/// it relies on the caller having canonicalized each TU's MainSourceFile
/// first; two spellings of one file then agree instead of collapsing to ''.
std::string computeMainSourceFile(
    const std::vector<clang::tooling::TranslationUnitReplacements> &TUs) {
  if (TUs.empty())
    return "";
  const std::string &First = TUs.front().MainSourceFile;
  for (const auto &TU : TUs)
    if (TU.MainSourceFile != First)
      return "";
  return First;
}

/// Build the conflict cluster list from the merged input key set.
///
/// `InputKeysByFile` is every input Replacement with length > 0, grouped by
/// file. Zero-length insertions are excluded by the caller because they never
/// overlap anything and are a no-op to apply, so they can never affect this
/// function's clustering.
///
/// A cluster is a maximal connected component of one file's input
/// replacements whose [offset, offset+length) byte ranges transitively
/// overlap.
///
/// The walk merges into the current cluster whenever
///   key.offset < lastEnd, where
///   lastEnd = max(member.offset + member.length) across cluster members.
/// Otherwise the current cluster closes and a new one opens.
///
/// Only clusters of size > 1 are returned; singletons are not conflicts.
///
/// Each returned cluster's member list is sorted by (offset, length, text);
/// the cluster list itself is sorted by (file, min-offset) ascending. This
/// pins iteration order for both stderr cluster lines and (in a follow-on
/// task) the SARIF results array.
std::vector<std::vector<clang::tooling::Replacement>> buildConflictClusters(
    const std::map<std::string, std::set<clang::tooling::Replacement>>
        &InputKeysByFile) {
  std::vector<std::vector<clang::tooling::Replacement>> Clusters;
  for (auto &Entry : InputKeysByFile) {
    // Keys is a std::set<Replacement>, so it's already ordered by
    // Replacement::operator< — the (offset, length, text) order the cluster
    // walk below needs, since every entry here shares Entry.first as its
    // file path.
    auto &Keys = Entry.second;

    std::vector<clang::tooling::Replacement> Current;
    unsigned LastEnd = 0;
    auto Flush = [&]() {
      if (Current.size() > 1)
        Clusters.push_back(std::move(Current));
      Current.clear();
      LastEnd = 0;
    };

    for (const clang::tooling::Replacement &K : Keys) {
      if (Current.empty()) {
        Current.push_back(K);
        LastEnd = K.getOffset() + K.getLength();
        continue;
      }
      if (K.getOffset() < LastEnd) {
        Current.push_back(K);
        LastEnd = std::max(LastEnd, K.getOffset() + K.getLength());
      } else {
        Flush();
        Current.push_back(K);
        LastEnd = K.getOffset() + K.getLength();
      }
    }
    Flush();
  }

  // Pin cluster-list order by (file, min-offset) ascending.
  llvm::sort(Clusters, [](const std::vector<clang::tooling::Replacement> &A,
                          const std::vector<clang::tooling::Replacement> &B) {
    if (A.front().getFilePath() != B.front().getFilePath())
      return A.front().getFilePath() < B.front().getFilePath();
    return A.front().getOffset() < B.front().getOffset();
  });

  return Clusters;
}

/// Emit one stderr line per conflict cluster.
///
/// Precondition: `Clusters` is sorted by (file, min-offset) ascending —
/// guaranteed by buildConflictClusters, the only place Clusters is built.
void emitConflictClusterLines(
    const std::vector<std::vector<clang::tooling::Replacement>> &Clusters) {
  for (const auto &Cluster : Clusters) {
    llvm::errs() << llvm::formatv(ConflictClusterSummary, Cluster.size(),
                                  Cluster.front().getFilePath(),
                                  Cluster.front().getOffset())
                 << "\n";
  }
}

/// Canonicalize a path — a Replacement's `FilePath` or a TU's
/// `MainSourceFile` — to one absolute spelling, so that every path naming
/// the same file compares equal as a string.
///
/// Fallback chain:
///   1. `llvm::sys::fs::real_path` — resolves symlinks and `..` segments and
///      yields an absolute path. Only succeeds if the file exists on disk.
///   2. `llvm::sys::fs::make_absolute` — succeeds for non-existent paths
///      too, resolving a relative path against the merger's working
///      directory. Its live consumer is `MainSourceFile`, which nothing
///      existence-checks: a relative main file that is not on disk still
///      gets an absolute spelling in the merged document. A Replacement
///      whose raw spelling does not exist is dropped before this function
///      sees it, and its diagnostic names that raw spelling, so for
///      `FilePath` this step is reached only when real_path fails on a file
///      that does exist.
///   3. Raw `FilePath` — last-resort fallback if both of the above fail
///      (no usable working directory).
///
/// An empty path is returned unchanged, before the chain runs: it is the
/// spelling for "no file", and `make_absolute` would otherwise turn it into
/// the working directory itself. This matters for `MainSourceFile: ''`
/// (the value when the inputs disagree), which nothing existence-checks. A
/// Replacement with an empty FilePath is dropped by the existence check on
/// its raw spelling regardless, so for FilePath this is merely consistent.
///
/// Relative paths resolve against the merger's working directory, not the
/// directory the compiler ran in when it recorded the FilePath. A caller
/// that runs the merger elsewhere than the build ran gets a path that
/// resolves to the wrong file or to no file at all.
std::string canonicalizePath(llvm::StringRef FilePath) {
  if (FilePath.empty())
    return std::string();
  llvm::SmallString<256> Buf;
  if (!llvm::sys::fs::real_path(FilePath, Buf))
    return std::string(Buf);
  Buf.assign(FilePath.begin(), FilePath.end());
  if (!llvm::sys::fs::make_absolute(Buf))
    return std::string(Buf);
  return FilePath.str();
}

/// Return `R` with its FilePath replaced by `canonicalizePath(FilePath)`;
/// offset, length and text are untouched.
clang::tooling::Replacement
canonicalizeFilePath(const clang::tooling::Replacement &R) {
  return clang::tooling::Replacement(canonicalizePath(R.getFilePath()),
                                     R.getOffset(), R.getLength(),
                                     R.getReplacementText());
}

/// Spell an already-canonical absolute path as a `file://` URI for SARIF.
///
/// Precondition: `FilePath` came through canonicalizeFilePath, so it is
/// absolute wherever the fallback chain's step 1 or 2 succeeded. The URI is
/// syntactically valid even in the step-3 case, matching the SARIF
/// requirement's "absolute" promise loosely.
std::string toFileUri(llvm::StringRef FilePath) {
  return "file://" + llvm::sys::path::convert_to_slash(FilePath);
}

/// Emit a SARIF document at `Path` listing every conflict cluster.
///
/// `Clusters` SHALL be pre-sorted by `(file, min-offset)` ascending by the
/// caller; this emitter walks them in order to populate
/// `runs[0].results[]`. Within each cluster, `relatedLocations[]` is
/// sorted locally by `(byteLength, candidate-text)` ascending per the
/// "SARIF conflict report" requirement.
///
/// Even when `Clusters` is empty, this writes a well-formed SARIF
/// document with `runs[0].results: []`. The file's presence is the
/// "merger ran with conflict reporting requested" signal.
llvm::Error emitConflictSarif(
    llvm::StringRef Path,
    llvm::ArrayRef<std::vector<clang::tooling::Replacement>> Clusters) {
  llvm::json::Array Results;
  Results.reserve(Clusters.size());

  for (const auto &Cluster : Clusters) {
    const clang::tooling::Replacement &Min = Cluster.front();
    std::string Uri = toFileUri(Min.getFilePath());

    // Re-sort cluster members locally by (byteLength, text) ascending.
    std::vector<clang::tooling::Replacement> Sorted(Cluster.begin(),
                                                    Cluster.end());
    llvm::sort(Sorted, [](const clang::tooling::Replacement &A,
                          const clang::tooling::Replacement &B) {
      if (A.getLength() != B.getLength())
        return A.getLength() < B.getLength();
      return A.getReplacementText() < B.getReplacementText();
    });

    llvm::json::Array RelatedLocations;
    RelatedLocations.reserve(Sorted.size());
    for (size_t I = 0; I < Sorted.size(); ++I) {
      const clang::tooling::Replacement &K = Sorted[I];
      RelatedLocations.push_back(llvm::json::Object{
          {"id", static_cast<int64_t>(I + 1)},
          {"physicalLocation",
           llvm::json::Object{
               {"artifactLocation", llvm::json::Object{{"uri", Uri}}},
               {"region",
                llvm::json::Object{
                    {"byteOffset", static_cast<int64_t>(K.getOffset())},
                    {"byteLength", static_cast<int64_t>(K.getLength())}}}}},
          {"message",
           llvm::json::Object{{"text", llvm::formatv(CandidateEditMessage,
                                                     K.getReplacementText())
                                           .str()}}}});
    }

    std::string MessageText =
        llvm::formatv(ConflictSarifMessage, Cluster.size(), Uri,
                      Min.getOffset())
            .str();

    Results.push_back(llvm::json::Object{
        {"ruleId", "clang-reforge-replacement-conflict"},
        {"level", "error"},
        {"message", llvm::json::Object{{"text", MessageText}}},
        {"locations",
         llvm::json::Array{llvm::json::Object{
             {"physicalLocation",
              llvm::json::Object{
                  {"artifactLocation", llvm::json::Object{{"uri", Uri}}},
                  {"region", llvm::json::Object{{"byteOffset",
                                                 static_cast<int64_t>(
                                                     Min.getOffset())}}}}}}}},
        {"relatedLocations", std::move(RelatedLocations)}});
  }

  llvm::json::Value Doc = llvm::json::Object{
      {"version", "2.1.0"},
      {"$schema", "https://json.schemastore.org/sarif-2.1.0.json"},
      {"runs",
       llvm::json::Array{llvm::json::Object{
           {"tool",
            llvm::json::Object{
                {"driver",
                 llvm::json::Object{{"name", ToolName},
                                    {"version", CLANG_VERSION_STRING}}}}},
           {"results", std::move(Results)}}}}};

  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_Text);
  if (EC)
    return llvm::createStringError(EC,
                                   llvm::formatv(CannotWriteFile, Path).str());
  // Pretty-print with indent 2 via the json::Value format_provider.
  OS << llvm::formatv("{0:2}", Doc) << "\n";
  OS.flush();
  if (OS.has_error())
    return llvm::createStringError(OS.error(),
                                   llvm::formatv(WriteErrorOnFile, Path).str());
  return llvm::Error::success();
}

/// Returns whether `Path`'s parent directory exists, so a bad `-o` or
/// `--sarif-conflicts-out` path can be rejected before any merge work runs.
/// A `Path` with no directory component (e.g. a bare file name) is treated
/// as valid — it names a file in the current directory. This is a
/// best-effort check, not a substitute for handling the real open() failure:
/// it cannot catch permission errors or a race between the check and the
/// eventual write.
bool parentDirectoryExists(llvm::StringRef Path) {
  llvm::StringRef Parent = llvm::sys::path::parent_path(Path);
  return Parent.empty() || llvm::sys::fs::is_directory(Parent);
}

} // namespace

namespace llvm {
namespace yaml {
/// Specialized MappingTraits to describe how a MergedReplacements is
/// (de)serialized. Mirrors MappingTraits<TranslationUnitReplacements> in
/// ReplacementsYaml.h exactly, key-for-key, for wire compatibility.
template <> struct MappingTraits<MergedReplacements> {
  static void mapping(IO &Io, MergedReplacements &Doc) {
    Io.mapRequired("MainSourceFile", Doc.MainSourceFile);
    Io.mapRequired("Replacements", Doc.Replacements);
  }
};
} // namespace yaml
} // namespace llvm

int main(int argc, const char **argv) {
  llvm::InitLLVM X(argc, argv);
  cl::HideUnrelatedOptions(MergeCategory);
  cl::ParseCommandLineOptions(
      argc, argv,
      "clang-ssaf-src-edit-merge: merge per-TU TranslationUnitReplacements "
      "YAML files for one link unit into a single merged YAML. Does not "
      "write source files; the apply step is the caller's responsibility.\n");

  // Validate the command-line parameters that can be checked without
  // reading any input, so a bad -o or --sarif-conflicts-out path is rejected
  // before the (potentially expensive) merge work below runs.
  if (!parentDirectoryExists(OutputFile)) {
    llvm::errs() << ToolName << ": "
                 << llvm::formatv(CannotWriteFile, OutputFile) << "\n";
    return 1;
  }
  if (!SarifConflictsOut.empty() && !parentDirectoryExists(SarifConflictsOut)) {
    llvm::errs() << ToolName << ": "
                 << llvm::formatv(CannotWriteFile, SarifConflictsOut) << "\n";
    return 1;
  }

  // Read all inputs, canonicalizing every path on the way in: each
  // Replacement's FilePath and the TU's MainSourceFile. Every comparison
  // below — the pre-dedup set, the per-file buckets, the conflict clusters,
  // computeMainSourceFile's agreement test, the stderr and SARIF reports and
  // the merged output itself — keys on the path as a string, and only a
  // canonical string makes two spellings of one file compare equal.
  //
  // Whether a Replacement's file exists is decided here too, on the raw
  // spelling and before it is replaced. A Replacement targeting a file that
  // doesn't exist can never be applied, so it is excluded from the merged
  // output; testing the raw spelling keeps canonicalization from creating
  // existence — realpath(3) on macOS resolves 'foo.cpp/' and 'foo.cpp/.' to
  // the regular file foo.cpp, and make_absolute resolves '' to the working
  // directory, all of which exist. Each Replacement's own spelling decides
  // its own fate: an input that spells one file both ways keeps the edit
  // that spelled it correctly and loses the one that did not.
  //
  // The two spellings serve different jobs. The canonical one is the key:
  // everything that compares or emits paths uses it. The raw one is the
  // diagnosis: what did not exist is what the producer wrote, it is the only
  // string the reader can search their YAML for, and the canonical form of
  // a spelling like 'foo.cpp/' names a file that does exist — beside a
  // surviving edit to it, that message would contradict the document it
  // accompanies. One line per distinct raw spelling.
  std::vector<clang::tooling::TranslationUnitReplacements> TUs;
  TUs.reserve(InputFiles.size());
  std::set<std::string> MissingSpellings;
  for (const std::string &Path : InputFiles) {
    clang::tooling::TranslationUnitReplacements TU;
    if (!readInput(Path, TU))
      return 1;
    TU.MainSourceFile = canonicalizePath(TU.MainSourceFile);
    std::vector<clang::tooling::Replacement> Kept;
    Kept.reserve(TU.Replacements.size());
    for (const clang::tooling::Replacement &R : TU.Replacements) {
      if (!llvm::sys::fs::exists(R.getFilePath())) {
        MissingSpellings.insert(R.getFilePath().str());
        continue;
      }
      Kept.push_back(canonicalizeFilePath(R));
    }
    TU.Replacements = std::move(Kept);
    TUs.push_back(std::move(TU));
  }
  for (const std::string &F : MissingSpellings)
    llvm::errs() << ToolName << ": " << llvm::formatv(MissingReplacementFile, F)
                 << "\n";

  // Pre-deduplicate identical replacements across all input TUs.
  //
  // This loop keeps a running set of every (file, offset, length, text)
  // tuple already kept across all TUs and drops
  // any later Replacement that matches one already kept, so each distinct
  // Replacement is considered exactly once below. The first occurrence (in
  // input-file order, then within-file order) wins; later duplicates are
  // byte-identical to it, so which one is "first" is observationally moot.
  // `file` here is the canonical path, so two TUs that spelled one shared
  // header differently collapse here just as two that spelled it alike do.
  {
    std::set<clang::tooling::Replacement> SeenKeys;
    for (auto &TU : TUs) {
      std::vector<clang::tooling::Replacement> Unique;
      Unique.reserve(TU.Replacements.size());
      for (const clang::tooling::Replacement &R : TU.Replacements) {
        if (SeenKeys.insert(R).second)
          Unique.push_back(R);
      }
      TU.Replacements = std::move(Unique);
    }
  }

  // Split every surviving-candidate Replacement by file. Zero-length
  // insertions go straight into SurvivorsByFile — they can never overlap
  // anything, so they're never at risk of being dropped. Length > 0 entries
  // go into InputKeysByFile, the input to buildConflictClusters, which is
  // the sole authority on which of them conflict.
  std::map<std::string, std::set<clang::tooling::Replacement>> SurvivorsByFile;
  std::map<std::string, std::set<clang::tooling::Replacement>> InputKeysByFile;
  for (const auto &TU : TUs) {
    for (const auto &R : TU.Replacements) {
      if (R.getLength() == 0)
        SurvivorsByFile[R.getFilePath().str()].insert(R);
      else
        InputKeysByFile[R.getFilePath().str()].insert(R);
    }
  }

  // Build conflict clusters — the sole authority on both what gets dropped
  // and what gets reported. There is no separate merge step to disagree
  // with it.
  std::vector<std::vector<clang::tooling::Replacement>> Clusters =
      buildConflictClusters(InputKeysByFile);

  // Every Replacement that's a member of a (size > 1) cluster is dropped;
  // everything else in InputKeysByFile survives into SurvivorsByFile.
  std::set<clang::tooling::Replacement> ClusterMembers;
  for (const auto &Cluster : Clusters)
    for (const clang::tooling::Replacement &K : Cluster)
      ClusterMembers.insert(K);
  for (auto &Entry : InputKeysByFile)
    for (const clang::tooling::Replacement &R : Entry.second)
      if (!ClusterMembers.count(R))
        SurvivorsByFile[Entry.first].insert(R);

  // Flatten SurvivorsByFile into the merged output. Iterating a std::map of
  // std::sets yields (file, then offset/length/text) order deterministically,
  // regardless of argv or input-file order.
  MergedReplacements OutDoc;
  OutDoc.MainSourceFile = computeMainSourceFile(TUs);
  for (auto &Entry : SurvivorsByFile)
    for (const clang::tooling::Replacement &R : Entry.second)
      OutDoc.Replacements.push_back(R);

  // Emit stderr cluster lines. Clusters was sorted by (file, min-offset)
  // ascending inside buildConflictClusters.
  emitConflictClusterLines(Clusters);

  // When --sarif-conflicts-out=<path> was supplied, write the SARIF
  // document. An empty Clusters still produces a well-formed SARIF with
  // results: [] — the file's presence is the signal that conflict
  // reporting was requested. Flag-omitted skips emission entirely; no file
  // is created at any path.
  if (!SarifConflictsOut.empty()) {
    if (llvm::Error E = emitConflictSarif(SarifConflictsOut, Clusters)) {
      llvm::errs() << ToolName << ": " << llvm::toString(std::move(E)) << "\n";
      return 1;
    }
  }

  // Write merged YAML (truncate-and-overwrite per spec).
  std::error_code EC;
  llvm::raw_fd_ostream OutStream(OutputFile, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    llvm::errs() << ToolName << ": "
                 << llvm::formatv(CannotWriteOutput, OutputFile, EC.message())
                 << "\n";
    return 1;
  }
  llvm::yaml::Output YAML(OutStream);
  YAML << OutDoc;
  OutStream.flush();
  if (OutStream.has_error()) {
    llvm::errs() << ToolName << ": "
                 << llvm::formatv(WriteErrorOnFile, OutputFile) << "\n";
    return 1;
  }

  return 0;
}
