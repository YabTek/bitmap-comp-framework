# Validation record: version 0.2.0

Checked on 20 October 2026, macOS 14.7, x86-64, CMake 4.1.3.

## Local builds

* Release with Apple Clang 16.0.0: all CTest suites passed.
* Release with LLVM Clang 21.1.6: all CTest suites passed.
* Debug with Apple Clang 16.0.0 and AddressSanitizer +
  UndefinedBehaviorSanitizer: all CTest suites passed, with no sanitizer errors.
* The supplied `build.sh` and `run_example.sh` were run from the delivered
  repository. All nine backends verified 200 queries with five repetitions
  each (9,000 timed query results).

The codec suite performs **361,326 checks**. It covers Raw, WAH, EWAH, Roaring,
CONCISE, PLWAH, VAL-WAH, SBH and BBC: exact decoded positions and cardinalities,
AND/OR, repeated/multiple operands, byte-stable archive round trips, malformed
and mutated archives, and failed-load state preservation. It includes all
65,536 two-byte inputs for BBC, all five BBC control families, both exception
polarities, multi-byte extended lengths, and a 2^32-bit compressed case without
allocating its uncompressed universe. Other cases exercise partial groups,
Roaring chunk boundaries, VAL fill-counter splitting, 25-bit PLWAH/CONCISE
counter splitting, different VAL widths and different SBH region sizes.

The I/O suite checks byte order, exact lengths, padding, metadata precision,
CRLF, query syntax and CSV quoting. CLI tests exercise all nine build/save/load
paths, zero-length universes, invalid arguments and mismatching datasets.

## Independent interoperability checks

These checks used separate development-only libraries, not dependencies or
code bundled in this repository:

* **539 Roaring portable-format checks** against PyRoaring 1.2.0, backed by
  CRoaring v5.2.2. Covered empty, array, bitmap and run containers; mixed
  containers with and without offsets; cardinalities around 4096; keys across
  chunks and position 2^32−1. Streams were read in both directions and checked
  by actual set positions. Third-party streams were also re-saved and decoded
  by CRoaring.
* **2,000 CONCISE stream checks** against the independent
  [lemire/Concise](https://github.com/lemire/Concise) implementation:
  1,000 generated sets, reading in both directions, using the reference word
  iterator. It covered sparse, dense, clustered and mixed input. Downloaded
  `include/concise.h` SHA-256:
  `551b0adb186dc950c4ac7a2f9ac3d7a7713aa23c4a9949a7359188ed473236de`.

The other encodings are checked by documented field examples, boundary cases,
and comparisons with Raw; **independent third-party file interoperability has
not been established for them**. VAL's selector and SBH's region prefix are
explicit local conventions, and the outer archive is framework-specific.

## Limits of this record

No Linux or Windows build has been run locally for this release. A GitHub
Actions workflow is supplied for those platforms, macOS and sanitizers, but
its remote results are not claimed here. Passing tests is evidence of the
checked behavior, not a proof over all inputs or a performance endorsement.
The code is scalar course infrastructure; optimization/configuration limits
are listed in [formats.md](formats.md). No performance ranking was validated.
