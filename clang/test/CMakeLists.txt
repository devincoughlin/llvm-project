# Test runner infrastructure for Clang. This configures the Clang test trees
# for use by Lit, and delegates to LLVM's lit test handlers.

llvm_canonicalize_cmake_booleans(
  BUILD_SHARED_LIBS
  CLANG_BUILD_EXAMPLES
  CLANG_BUILT_STANDALONE
  CLANG_DEFAULT_PIE_ON_LINUX
  CLANG_ENABLE_STATIC_ANALYZER
  CLANG_PLUGIN_SUPPORT
  CLANG_SPAWN_CC1
  CLANG_ENABLE_CIR
  CLANG_ENABLE_OBJC_REWRITER
  CLANG_LINK_CLANG_DYLIB
  CLANG_USE_XCSELECT
  ENABLE_BACKTRACES
  LLVM_BYE_LINK_INTO_TOOLS
  LLVM_CAS_ENABLE_REMOTE_CACHE
  LLVM_ENABLE_PLUGINS
  LLVM_ENABLE_ZLIB
  LLVM_ENABLE_ZSTD
  LLVM_ENABLE_PER_TARGET_RUNTIME_DIR
  LLVM_ENABLE_THREADS
  LLVM_ENABLE_ONDISK_CAS
  LLVM_ENABLE_REVERSE_ITERATION
  LLVM_INCLUDE_EXAMPLES
  LLVM_LINK_LLVM_DYLIB
  LLVM_WITH_Z3
  PPC_LINUX_DEFAULT_IEEELONGDOUBLE
  LLVM_TOOL_LLVM_DRIVER_BUILD
  LLVM_INCLUDE_SPIRV_TOOLS_TESTS
  )

# Run tests requiring Z3 headers only if LLVM was built with Z3
# and the headers are available while building Clang -- the latter may
# not be the case when building standalone against installed LLVM.
set(TEST_WITH_Z3_MOCK 0)
if(LLVM_WITH_Z3 AND Z3_FOUND AND CMAKE_SYSTEM_NAME MATCHES "Linux")
  llvm_add_library(
    MockZ3SolverCheck
    MODULE Analysis/z3/Inputs/MockZ3_solver_check.cpp
    DISABLE_LLVM_LINK_LLVM_DYLIB)

  if(TARGET MockZ3SolverCheck)
    list(APPEND CLANG_TEST_DEPS
      MockZ3SolverCheck)
    target_include_directories(
      MockZ3SolverCheck
      PRIVATE ${Z3_INCLUDE_DIR})
    set(TEST_WITH_Z3_MOCK 1)
  endif()
endif()

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
  PATHS
  "LLVM_SOURCE_DIR"
  "LLVM_BINARY_DIR"
  "LLVM_TOOLS_DIR"
  "LLVM_LIBS_DIR"
  "SHLIBDIR"
  "LLVM_LIT_TOOLS_DIR"
  "LLVM_EXTERNAL_LIT"
  "CLANG_BINARY_DIR"
  "CLANG_SOURCE_DIR"
  "CURRENT_TOOLS_DIR"
  "CMAKE_LIBRARY_OUTPUT_DIRECTORY"
  )

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/Unit/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.cfg.py
  PATHS
  "LLVM_SOURCE_DIR"
  "LLVM_BINARY_DIR"
  "LLVM_TOOLS_DIR"
  "LLVM_LIBS_DIR"
  "CLANG_BINARY_DIR"
  "SHLIBDIR"
  )

option(CLANG_TEST_USE_VG "Run Clang tests under Valgrind" OFF)
if(CLANG_TEST_USE_VG)
  set(CLANG_TEST_EXTRA_ARGS ${CLANG_TEST_EXTRA_ARGS} "--vg")
endif ()

list(APPEND CLANG_TEST_DEPS
  apinotes-test
  c-index-test
  cache-build-session
  clang
  clang-cas-test
  clang-fuzzer-dictionary
  clang-resource-headers
  clang-format-check-format
  clang-tblgen
  clang-offload-bundler
  clang-refactor-test
  clang-import-test
  clang-refactor
  clang-diff
  clang-installapi
  clang-scan-deps
  clang-linker-wrapper
  clang-nvlink-wrapper
  clang-offload-bundler
  clang-ssaf-analyzer
  clang-ssaf-format
  clang-ssaf-linker
  clang-ssaf-src-edit-merge
  clang-sycl-linker
  diagtool
  hmaptool
  )

if(CLANG_ENABLE_CIR)
  list(APPEND CLANG_TEST_DEPS
    cir-opt
    cir-translate
    )
  add_subdirectory(CIR/lib)
endif()

if(CLANG_ENABLE_STATIC_ANALYZER)
  list(APPEND CLANG_TEST_DEPS
    clang-check
    clang-extdef-mapping
    )
endif()

# clang-apply-replacements lives in clang-tools-extra, so it only exists when
# that project is enabled. The SSAF source-edit tests gate on the
# `clang-apply-replacements` lit feature, which
# Analysis/Scalable/source-edit-generation/lit.local.cfg adds by looking for
# the binary in the tools directory -- so without this dependency a fresh
# `ninja check-clang` silently runs those tests as UNSUPPORTED and reports
# coverage it does not have.
if(TARGET clang-apply-replacements)
  list(APPEND CLANG_TEST_DEPS
    clang-apply-replacements
    )
endif()

if(CLANG_BUILD_EXAMPLES AND CLANG_PLUGIN_SUPPORT)
  list(APPEND CLANG_TEST_DEPS
    Attribute
    AnnotateFunctions
    CallSuperAttr
    PluginsOrder
    PrintFunctionNames
    LLVMPrintFunctionNames
    )
endif ()

if(LLVM_INCLUDE_EXAMPLES AND NOT WIN32 AND NOT CYGWIN)
  list(APPEND CLANG_TEST_DEPS
    Bye
    )
endif()

if(LLVM_INCLUDE_SPIRV_TOOLS_TESTS)
  list(APPEND CLANG_TEST_DEPS
    spirv-dis
    spirv-val
    spirv-as
    spirv-link
    )
endif()    

if( NOT CLANG_BUILT_STANDALONE )
  list(APPEND CLANG_TEST_DEPS
    llvm-config
    FileCheck count not
    CASPluginTest
    dsymutil
    llc
    llvm-ar
    llvm-as
    llvm-bcanalyzer
    llvm-cas
    llvm-cas-dump
    llvm-cat
    llvm-cxxfilt
    llvm-dis
    llvm-dwarfdump
    llvm-ifs
    llvm-link
    llvm-lto2
    llvm-mc
    llvm-modextract
    llvm-nm
    llvm-objcopy
    llvm-objdump
    llvm-pdbutil
    llvm-profdata
    llvm-rc
    llvm-readelf
    llvm-readobj
    llvm-readtapi
    llvm-strip
    llvm-symbolizer
    llvm-windres
    llvm-offload-binary
    obj2yaml
    opt
    split-file
    yaml2obj
    )

  if(TARGET lld)
    list(APPEND CLANG_TEST_DEPS lld)
  endif()

  if(TARGET llvm-lto)
    list(APPEND CLANG_TEST_DEPS llvm-lto)
  endif()
  if(TARGET llvm-remote-cache-test)
    list(APPEND CLANG_TEST_DEPS llvm-remote-cache-test)
  endif()
endif()

if(CLANG_ENABLE_STATIC_ANALYZER)
  if(CLANG_PLUGIN_SUPPORT AND LLVM_ENABLE_PLUGINS) # Determine if we built them
    list(APPEND CLANG_TEST_DEPS
      SampleAnalyzerPlugin
      CheckerDependencyHandlingAnalyzerPlugin
      CheckerOptionHandlingAnalyzerPlugin
      )
  endif()
endif()

if(CLANG_PLUGIN_SUPPORT AND LLVM_ENABLE_PLUGINS AND NOT WIN32)
  list(APPEND CLANG_TEST_DEPS
    SSAFExamplePlugin
    SSAFTestTransformationPlugin
    )
  add_subdirectory(Analysis/Scalable/source-edit-generation/Plugins)
endif()

if (HAVE_CLANG_REPL_SUPPORT)
  list(APPEND CLANG_TEST_DEPS
    clang-repl
    )
endif()

# Copy gen_ast_dump_json_test.py to the clang build dir. This allows invoking
# it without having to pass the --clang= argument
configure_file(AST/gen_ast_dump_json_test.py
  ${CLANG_BINARY_DIR}/bin/gen_ast_dump_json_test.py COPYONLY)

add_custom_target(clang-test-depends DEPENDS ${CLANG_TEST_DEPS})
set_target_properties(clang-test-depends PROPERTIES FOLDER "Clang/Tests")

add_lit_testsuite(check-clang "Running the Clang regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  #LIT ${LLVM_LIT}
  DEPENDS ${CLANG_TEST_DEPS}
  ARGS ${CLANG_TEST_EXTRA_ARGS}
  )

add_lit_testsuites(CLANG ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${CLANG_TEST_DEPS}
  FOLDER "Clang tests/Suites"
)

# Add a legacy target spelling: clang-test
add_custom_target(clang-test)
add_dependencies(clang-test check-clang)
set_target_properties(clang-test PROPERTIES FOLDER "Clang/Tests")

# FIXME: This logic can be removed once all buildbots have moved
# debuginfo-test from clang/test to llvm/projects or monorepo.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/debuginfo-tests)
  message(WARNING "Including debuginfo-tests in clang/test is deprecated.  Move to llvm/projects or use monorepo.")
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/debuginfo-tests/CMakeLists.txt)
    add_subdirectory(debuginfo-tests)
  endif()
endif()

add_subdirectory(Analysis/LifetimeSafety)

# The ParameterEscape negative corpus, validated against AddressSanitizer
# rather than against this analysis.  Not part of check-clang: it builds and
# runs programs, and it needs a compiler with an ASan runtime, which a clang
# configured without compiler-rt does not have.
#
# So the compiler is probed for rather than assumed.  The clang this build
# produces is preferred and cannot be tested here -- it does not exist at
# configure time -- so it is used whenever compiler-rt is being built, and
# otherwise a host compiler that can actually link `-fsanitize=address` is
# found by compiling a one-line program with each candidate.  Set
# CLANG_SSAF_ESCAPE_CORPUS_CLANG to override.
#
# The script refuses to run at all when the compiler it ends up with cannot
# report a stack-use-after-scope, so a machine with no ASan anywhere fails
# loudly instead of validating nothing.
#
# Not a genex in the cache: a `$<TARGET_FILE:clang>` stored in a CACHE STRING
# shows up as literal genex text in ccmake and is destroyed by any user edit.
set(CLANG_SSAF_ESCAPE_CORPUS_CLANG "" CACHE STRING
  "Compiler used to build the ParameterEscape escape-corpus programs. \
Empty means: the in-tree clang when compiler-rt is enabled, else a probed host compiler.")

if(CLANG_SSAF_ESCAPE_CORPUS_CLANG)
  set(_ssaf_corpus_clang "${CLANG_SSAF_ESCAPE_CORPUS_CLANG}")
elseif("compiler-rt" IN_LIST LLVM_ENABLE_RUNTIMES OR
       "compiler-rt" IN_LIST LLVM_ENABLE_PROJECTS)
  set(_ssaf_corpus_clang "$<TARGET_FILE:clang>")
else()
  set(_ssaf_corpus_clang "")
  set(_ssaf_asan_probe "${CMAKE_CURRENT_BINARY_DIR}/ssaf-asan-probe.c")
  file(WRITE "${_ssaf_asan_probe}" "int main(void) { return 0; }\n")
  foreach(_cand "${CMAKE_C_COMPILER}" "clang" "cc")
    if(_cand AND NOT _ssaf_corpus_clang)
      find_program(_ssaf_cand_path NAMES "${_cand}")
      if(_ssaf_cand_path)
        execute_process(
          COMMAND "${_ssaf_cand_path}" -fsanitize=address -x c
                  "${_ssaf_asan_probe}"
                  -o "${CMAKE_CURRENT_BINARY_DIR}/ssaf-asan-probe.out"
          RESULT_VARIABLE _ssaf_probe_rc
          OUTPUT_QUIET ERROR_QUIET)
        if(_ssaf_probe_rc EQUAL 0)
          set(_ssaf_corpus_clang "${_ssaf_cand_path}")
        endif()
      endif()
      unset(_ssaf_cand_path CACHE)
    endif()
  endforeach()
  if(NOT _ssaf_corpus_clang)
    # Nothing here can link ASan.  Point the target at the in-tree clang so it
    # reports the missing runtime itself rather than reporting nothing.
    set(_ssaf_corpus_clang "$<TARGET_FILE:clang>")
  endif()
endif()

add_custom_target(check-clang-ssaf-escape-corpus
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/../utils/ssaf/validate_escape_corpus.py"
          "${_ssaf_corpus_clang}"
          "${CMAKE_CURRENT_SOURCE_DIR}/Analysis/Scalable/ParameterEscape/Negative"
  DEPENDS clang
  COMMENT "Validating the ParameterEscape negative corpus under AddressSanitizer"
  USES_TERMINAL)
set_target_properties(check-clang-ssaf-escape-corpus PROPERTIES FOLDER "Clang/Tests")
