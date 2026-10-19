# Bitmap compression framework

A C++17 starter framework for **Benchmarking Bitmap Compression Methods**.
It supplies nine bitmap representations, AND/OR queries, exact saved sizes,
correctness checks, and CSV measurements. No external libraries are required.

**Your work:** implement the dataset generator, design controlled experiments,
automate the runs, and explain the results. You do not have to implement the
compression formats. Compare Raw, WAH, EWAH, Roaring and your group's additional
format: PLWAH, CONCISE, VAL-WAH, SBH or BBC. The
[assignment handout](https://annbolotina.github.io/teaching/ATDA/handouts/assignment_1_formats.pdf)
provides explanations and research questions; the course slides specify the
submission requirements.

## Build and check

Requirements: CMake 3.20+, a C++17 compiler, and a 64-bit environment. Tested with
Apple Clang 16 and LLVM Clang 21 on macOS; the code also targets GCC and MSVC,
but those platforms have not been tested for this release.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

On macOS/Linux, `scripts/build.sh` performs the build and tests. For a
multi-configuration generator such as Visual Studio, executables are under
`build/Release/`; use that path instead of `build/` in the commands below.
Always use a Release build for measurements.

```sh
scripts/run_example.sh
```

This runs all nine backends on the committed example dataset with verification.
Each should report `0 mismatches`. Results go to `results/example/`.
The small example checks the pipeline; it is not a substitute for your own
experiment datasets. It intentionally does not answer the research questions.

## Available representations

| `--backend` | Configuration supplied | How AND/OR are computed |
|---|---|---|
| `raw` | Packed 64-bit words | Word loops |
| `wah` | 64-bit codewords, 63-bit logical groups | Compressed fill/literal merge |
| `ewah` | 64-bit literals and separate markers | Compressed marker/literal merge |
| `roaring` | Array, bitmap and run containers | Container operations |
| `plwah` | 32-bit, one exception, fixed 25-bit counter | Compressed merge with exception decoding |
| `concise` | 32-bit, exception in first group | Compressed merge with exception decoding |
| `val` | VAL-WAH, packed 64-bit words; 15/30/60-bit segments | Aligned compressed segment merge |
| `sbh` | 7-bit groups, bounded super-buckets | Expand one region per operand, combine, re-encode |
| `bbc` | Patent layout, all five control families | Compressed fill/tail/exception merge |

These are portable scalar implementations for this course, not bindings to
production libraries. The format choices and operation paths are documented in
[docs/formats.md](docs/formats.md). Timings describe **these implementations**;
they do not establish a universal ranking of published formats or libraries.
Construction includes encoding and the configured optimization policy.

## Your dataset generator

Write `tools/generate_dataset.cpp` and add its target at the marked place in
`CMakeLists.txt`. Helpers in `common/bitmap_io.hpp` write this directory format:

```text
metadata.txt
bitmap_000.bin
bitmap_001.bin
...
```

Example metadata:

```text
num_bits=100000
num_bitmaps=6
word_bits=64
format=raw_uint64_le
kind=uniform
params=p=0.01
density=0.01
seed=1
```

The first four fields are required. `num_bits` is in 0..2^32 and
`num_bitmaps` must be positive. Optional `kind`, `params`, `density`, and `seed`
are copied to the results. Missing density is written as −1 in CSV, and a
missing seed is blank. Whitespace around fields is ignored. `density`, if
provided, must be finite and in [0,1]; omit it if it is not a meaningful summary.
Unknown metadata keys may record additional generator parameters, but they are
not copied into CSV automatically. Duplicate keys are rejected.

Each bitmap contains `ceil(num_bits/64)` **little-endian** 64-bit words.
Position `b` is bit `b % 64` of word `b / 64`. Bits beyond `num_bits` in the last
word must be zero. File lengths and padding are checked: the reader does not
silently fix a faulty generator. `num_bits=0` uses a zero-byte bitmap file.

Use `write_metadata()`, `write_bitmap()`, `bitmap_filename()` and the helpers
in `common/bits.hpp`. `write_metadata()` creates the dataset directory.
Expose parameters and a seed through the command line; record enough to
reproduce the exact data. There is deliberately no supplied generator for the
experimental distributions, nor a completed experiment sweep or plotting code.

## Queries and benchmark

A query file contains one operation, operand count, and list of zero-based
bitmap IDs per line. Empty lines and lines starting with `#` are ignored.

```text
AND 2 0 1
OR 3 1 2 3
```

Unary queries and repeated IDs are valid. A query must contain at least one
operand; all IDs must exist. Random query generation uses distinct IDs within
each query:

```sh
build/generate_queries --out=queries/example.txt --num-bitmaps=6 \
    --num-queries=200 --min-width=2 --max-width=4 --seed=43

build/bitmap_bench --backend=ewah --dataset=data/example \
    --queries=queries/example.txt --out=results/ewah.csv \
    --repetitions=7 --warmup=2 --build-repetitions=3 --verify=1
```

`--verify=1` is the default. It checks construction, save/load, and every timed
query against Raw **outside the timed interval**, including decoded positions
and cardinality. Any failure exits nonzero. Only use output from successful
runs. `--verify=0` disables those comparisons; repeat verification for each new
dataset or code change before using unverified measurements.

Use the *same query file and dataset* across representations. Record the exact
input cardinalities and overlap rather than inferring them from global density.
Random query generation chooses operands; it does not control their overlap.

```sh
# VAL: force a legal segment length, or select the smallest saved payload
build/bitmap_bench --backend=val --val-segment-bits=auto \
    --dataset=data/example --queries=queries/example.txt --out=results/val.csv

# SBH: number of 7-bit groups per region, 1..4095 (default 128)
build/bitmap_bench --backend=sbh --sbh-groups=128 \
    --dataset=data/example --queries=queries/example.txt --out=results/sbh.csv
```

VAL defaults to 15 bits. `auto` tries all three widths during construction;
this is a size selector, not the paper's λ-based selector. Its cost is included
in build time. SBH's default region covers 896 original bits. Different settings
are different experiments: record them.

## Save, load, inspect

```sh
build/bitmap_bench --backend=roaring --dataset=data/example \
    --queries=queries/example.txt --out=results/build.csv --save=results/data.bmcf

build/bitmap_bench --backend=roaring --dataset=data/example \
    --queries=queries/example.txt --out=results/load.csv --load=results/data.bmcf

build/bitmap_inspect --backend=plwah --dataset=data/example --bitmap=0 \
    --max-bytes=64 --max-positions=16
```

Loading uses selectors stored in the archive; construction flags do not
re-encode loaded bitmaps. The original dataset is still required to check
identity and serve as the reference. `--verify=0` checks dimensions but does not
check that an archive represents the same sets as that dataset.

`bitmap_inspect` prints logical positions, byte counts, representation counters,
and a bounded hex dump of one bitmap's actual payload. The dump is in **file
byte order**, so multibyte codewords appear little-endian. It is useful when
checking an encoding calculation. `--help` lists each tool's options.

## Results and interpretation

Every query repetition is one CSV row. A companion `.csv.run.txt` records the
compiler, build configuration, warmup, construction samples, and allocation
policy. Record your machine and OS separately.

* `query_time_ns`: the AND/OR call, including allocation and encoding of the
  materialized result. Input reading, verification, cardinality, and final
  result destruction are outside the timer.
* `build_time_ms`: median of `--build-repetitions` constructions on fresh
  backends. Repeated in every row, so do not treat those rows as independent
  build measurements. The companion file preserves the individual samples.
* `compressed_bytes`: exact size of the **saved whole dataset**, including the
  shared 32-byte archive header, an 8-byte length per bitmap, and all native
  payloads. `raw_bytes` uses the same framing.
* `compression_ratio = raw_bytes / compressed_bytes`: greater than 1 means
  smaller than saved Raw; below 1 means larger.
* `payload_bytes` and `raw_payload_bytes`: sizes excluding the shared archive
  framing. They still include format-specific headers, selectors and padding.
  `result_payload_bytes` describes the returned query result.
* `query_ids`, `input_cardinalities`, `intersection_cardinality`: actual
  operands and their shared set positions. Lists use semicolons within a CSV
  field. For binary queries the last field is `t = |A ∩ B|`.
* `input_mode`, `load_time_ms`: distinguish construction from archive loading.
  Loaded runs have zero build time and zero build repetitions; zero means
  "not measured here", not free construction.
* `literal_units`, `fill_units`, `exception_units`, `control_units`, and
  container counts describe the **input dataset**. Their units differ across
  formats; see [docs/measurements.md](docs/measurements.md).

Compare medians and variation over repeated runs; keep AND and OR separate.
Warmup is a number of untimed calls before measuring **each query**, not a
Java-specific framework. It does not guarantee that all data fits in cache.
For reproducibility, vary backend order, keep seeds and scripts, and report
settings and optimization policy. More detail is in
[docs/measurements.md](docs/measurements.md).

## Development checks

```sh
# Clang/GCC: optional address and undefined-behavior checks
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DBENCH_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Tests cover all nine representations, randomized AND/OR comparisons, partial
groups, container/region/counter boundaries, known encoded examples, malformed
archives, round trips, and the command-line pipeline. The repository contains
no third-party implementation code. References and the exact implementation
scope are listed in [docs/formats.md](docs/formats.md). The release validation
record is in [docs/validation.md](docs/validation.md).

`scripts/clean.sh` removes only the default `build/` directory. Add `--outputs`
to also remove `queries/` and `results/`. It never deletes dataset directories.
`BUILD_DIR` overrides the build location for the build and example scripts;
`EXAMPLE_OUTPUT_DIR` overrides the example output directory.
