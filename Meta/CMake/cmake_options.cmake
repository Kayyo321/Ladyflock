option(BUILD_SHARED_LIBS "Build shared libraries instead of static libraries" ON)

option(ENABLE_COMPILETIME_FORMAT_CHECK "Enable compiletime format string checks" ON)
option(ENABLE_UNDEFINED_SANITIZER "Enable undefined behavior sanitizer testing in gcc/clang" OFF)
option(UNDEFINED_BEHAVIOR_IS_FATAL "Make undefined behavior sanitizer errors non-recoverable" OFF)

option(ENABLE_ALL_THE_DEBUG_MACROS "Enable all debug macros to validate they still compile" OFF)

option(INCLUDE_WASM_SPEC_TESTS "Download and include the WebAssembly spec testsuite" OFF)
option(BUILD_BROWSER_TESTS_ONLY "Only build the tests that run the browser's own executables" OFF)

# The optimizing JIT for JavaScript generates code for x86-64 and AArch64 on Linux and macOS, and for x86-64 on Windows.
# Elsewhere, LibJS runs JavaScript in its interpreter alone.
if (((LINUX AND NOT ANDROID) OR APPLE) AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|aarch64|arm64|ARM64)$")
    set(js_jit_default ON)
elseif (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(js_jit_default ON)
else()
    set(js_jit_default OFF)
endif()
option(ENABLE_JS_JIT "Enable the optimizing JIT for JavaScript" ${js_jit_default})

set(LADYBIRD_CACHE_DIR "${PROJECT_BINARY_DIR}/../caches" CACHE PATH "Location of shared cache of downloaded files")
option(ENABLE_NETWORK_DOWNLOADS "Allow downloads of required files. If OFF, required files must already be present in LADYBIRD_CACHE_DIR" ON)

option(ENABLE_CLANG_PLUGINS "Enable building with the Clang plugins" OFF)
option(ENABLE_CLANG_PLUGINS_INVALID_FUNCTION_MEMBERS "Enable detecting invalid function types as members of GC-allocated objects" OFF)

if ((LINUX AND NOT ANDROID) OR BSD)
    set(freedesktop_files_default ON)
else()
    set(freedesktop_files_default OFF)
endif()

option(ENABLE_GUI_TARGETS "Enable building GUI targets" ON)
option(ENABLE_LIBPRIVACY "Enable the Ladyflock LibPrivacy policy engine" ON)
option(LADYBIRD_BUILD_QT "Build and bundle Qt with vcpkg on Linux" OFF)
option(ENABLE_INSTALL_HEADERS "Enable installing headers" ON)
option(ENABLE_INSTALL_FREEDESKTOP_FILES "Enable installing .desktop and .service files" ${freedesktop_files_default})
option(LADYBIRD_ENABLE_CPPTRACE "Enable use of cpptrace as the default library for stacktraces. If not available falls back to backtrace.h" ON)
option(LADYBIRD_ENABLE_UPDATE_CHECK "Check for newer releases of Ladyflock" OFF)
option(LADYBIRD_ENABLE_SPARKLE "Update the macOS app with Sparkle" OFF)
if (NOT LADYBIRD_ENABLE_UPDATE_CHECK)
    # Sparkle only installs the releases that the update check finds.
    set(LADYBIRD_ENABLE_SPARKLE OFF)
endif()
option(LADYBIRD_GENERATE_DSYM "Generate dSYM bundles for binaries and libraries (macOS only)" OFF)
option(LADYBIRD_GENERATE_SYMBOL_FILES "Build with debug info and write the symbols of the installed executables to separate files" OFF)
if (LADYBIRD_GENERATE_SYMBOL_FILES AND MSVC)
    message(FATAL_ERROR "LADYBIRD_GENERATE_SYMBOL_FILES is not supported with MSVC-style toolchains")
endif()
option(LADYBIRD_BASELINE_CPU_TARGET "Use a common CPU baseline for portable binaries and ccache sharing" OFF)

set(LADYFLOCK_VERSION "" CACHE STRING "Version of a Ladyflock release, such as 0.1.0 or 2026.48.0")
set(LADYFLOCK_RELEASE_STAGE "Alpha" CACHE STRING "The stage of development that Ladyflock releases are in, if any, such as Alpha or Beta")
set(LADYFLOCK_RELEASE_CHANNEL "" CACHE STRING "Release channel for Ladyflock builds, if any")
set(LADYBIRD_COMPATIBILITY_VERSION "" CACHE STRING "Ladybird engine compatibility version for User-Agent and WebDriver (defaults to PROJECT_VERSION)")
set(LADYBIRD_VERSION "" CACHE STRING "Deprecated alias for LADYFLOCK_VERSION when LADYFLOCK_VERSION is unset")
# The app ID is shared by the browser and the fixed desktop integration filenames.
unset(LADYFLOCK_APPLICATION_ID CACHE)
set(LADYFLOCK_APPLICATION_ID "io.github.kayyo321.Ladyflock")
set(LADYFLOCK_UPDATE_FEED_URL "" CACHE STRING "URL of the Ladyflock releases.json update feed")
set(LADYFLOCK_RELEASE_NOTES_BASE_URL "https://github.com/Kayyo321/Ladyflock/releases/tag" CACHE STRING "Base URL for Ladyflock release notes pages")
set(LADYFLOCK_REPORTS_ENDPOINT "" CACHE STRING "Optional Ladyflock crash and web compatibility report collector endpoint")

set(LADYFLOCK_RELEASE_VERSION "${LADYFLOCK_VERSION}")
if (LADYFLOCK_RELEASE_VERSION STREQUAL "" AND NOT LADYBIRD_VERSION STREQUAL "")
    set(LADYFLOCK_RELEASE_VERSION "${LADYBIRD_VERSION}")
endif()

set(LADYFLOCK_BROWSER_VERSION "${PROJECT_VERSION}")
if (NOT LADYFLOCK_RELEASE_VERSION STREQUAL "")
    set(LADYFLOCK_BROWSER_VERSION "${LADYFLOCK_RELEASE_VERSION}")
endif()
set(LADYBIRD_BROWSER_VERSION "${LADYFLOCK_BROWSER_VERSION}")

set(LADYBIRD_EFFECTIVE_COMPATIBILITY_VERSION "${PROJECT_VERSION}")
if (NOT LADYBIRD_COMPATIBILITY_VERSION STREQUAL "")
    set(LADYBIRD_EFFECTIVE_COMPATIBILITY_VERSION "${LADYBIRD_COMPATIBILITY_VERSION}")
endif()

# Development builds have a bundle identifier of their own, so that macOS does not take them for copies of the release.
set(LADYBIRD_BUNDLE_IDENTIFIER "${LADYFLOCK_APPLICATION_ID}.Development")
if (NOT LADYFLOCK_RELEASE_VERSION STREQUAL "")
    set(LADYBIRD_BUNDLE_IDENTIFIER "${LADYFLOCK_APPLICATION_ID}")
endif()

# The stage of development that releases are in, if any, such as Alpha or Beta.
set(LADYBIRD_RELEASE_STAGE "${LADYFLOCK_RELEASE_STAGE}")

string(TOLOWER "${LADYFLOCK_REPORTS_ENDPOINT}" reports_endpoint_lower)
if (reports_endpoint_lower MATCHES "^https?://([^/?#@]*@)?([^/?#:]*\\.)?ladybird\\.org\\.?([/?#:]|$)")
    message(FATAL_ERROR "LADYFLOCK_REPORTS_ENDPOINT must not point to ladybird.org; configure a Ladyflock-owned collector or leave it empty.")
endif()
if (LADYBIRD_ENABLE_UPDATE_CHECK AND NOT LADYFLOCK_RELEASE_VERSION STREQUAL "" AND LADYFLOCK_UPDATE_FEED_URL STREQUAL "")
    message(FATAL_ERROR "LADYBIRD_ENABLE_UPDATE_CHECK requires a Ladyflock-owned LADYFLOCK_UPDATE_FEED_URL for release builds.")
endif()
if (NOT LADYFLOCK_UPDATE_FEED_URL STREQUAL "")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../Utils/validate_release_feed_url.py" "${LADYFLOCK_UPDATE_FEED_URL}"
        RESULT_VARIABLE update_feed_validation_result
        ERROR_VARIABLE update_feed_validation_error
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if (NOT update_feed_validation_result STREQUAL "0")
        message(FATAL_ERROR "Invalid LADYFLOCK_UPDATE_FEED_URL. ${update_feed_validation_error}")
    endif()
endif()

# Like other browsers, our Linux packages keep Ladyflock in a directory of its own.
set(LADYBIRD_PACKAGE_INSTALL_PREFIX "/opt/ladyflock")

option(ENABLE_ADDRESS_SANITIZER "Enable address sanitizer testing in gcc/clang" OFF)
option(ENABLE_MEMORY_SANITIZER "Enable memory sanitizer testing in gcc/clang" OFF)
option(ENABLE_THREAD_SANITIZER "Enable thread sanitizer testing in gcc/clang" OFF)
option(ENABLE_FUZZERS "Build fuzzing targets" OFF)
option(ENABLE_FUZZERS_LIBFUZZER "Build fuzzers using Clang's libFuzzer" OFF)
option(ENABLE_FUZZERS_OSSFUZZ "Build OSS-Fuzz compatible fuzzers" OFF)
option(LADYBIRD_ENABLE_CCACHE "Compile through ccache when it is installed" ON)
set(LAGOM_USE_LINKER "" CACHE STRING "The linker to use (e.g. lld, mold) instead of the system default")
set(LAGOM_LINK_POOL_SIZE "" CACHE STRING "The maximum number of parallel jobs to use for linking")
option(LADYBIRD_ENABLE_LTO "Enable link-time optimization" OFF)
option(ENABLE_LAGOM_COVERAGE_COLLECTION "Enable code coverage instrumentation for lagom binaries in clang" OFF)

if (ENABLE_FUZZERS_LIBFUZZER)
    # With libfuzzer, we need to avoid a duplicate main() linker error giving false negatives
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "Type of target to use for try_compile()" FORCE)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/lagom_install_options.cmake)
