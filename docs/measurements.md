# Measurement contract

## Timed work

Dataset reading, query parsing, CSV output, cardinality calculation and decoded
verification are outside query time. Each timed query folds operands in the
order written, producing a result in the backend's representation. It includes
all intermediate work, temporary allocation/destruction, encoding and output
allocation required by that call. Only destruction of the **returned** result
occurs after the timer. This is a materialized-result benchmark, not a
cardinality-only query benchmark.

SBH includes bounded-region expansion and re-encoding. Roaring includes
container optimization. VAL output uses the smaller input width and does not
run a new automatic width selection. The other stream encoders merge fills
while writing. State these policies when presenting results.

`--warmup=W` runs W untimed calls before **each** query's repetitions.
Verification can affect cache state even though it is not timed; warmup happens
after the reference calculation. This is an in-memory, warmed-query experiment,
not a cold-storage or out-of-core experiment. `--verify=0` still calculates
reference-derived operand/overlap metadata outside the timer; it skips decoded
comparisons and archive checks. It is not a no-reference-memory mode.

`build_time_ms` is the median of constructions using fresh backend objects.
Previous backend destruction and file I/O are outside that timer. All build
samples are recorded in `.run.txt`. Construction samples are repeated in rows
for convenience, not independent observations per row. Archive loads are a
separate mode: `build_time_ms=0`, `build_repetitions=0`, with `load_time_ms`
reported. Never interpret zero build time in a loaded run as construction cost.

## Space

Saved dataset bytes are obtained from the actual serialization format, checked
against `serialize().size()` during verification. The wrapper is shared by all
representations. Compression ratio uses framed Raw divided by framed encoded
size. Payload columns allow the effect of shared framing to be inspected
separately. Format-specific selectors, portable Roaring headers, counter bytes,
BBC terminators and physical last-word padding are part of native payload size.
There is no resident-memory or peak-memory measurement.

A query result has no outer dataset wrapper. `result_payload_bytes` is its
native payload size. An all-zero BBC result may therefore occupy only its
terminator even when its logical universe is large; N is retained externally.

## Representation counters

The seven counter columns describe the **stored input dataset**, summed over
bitmaps; they are constant within one benchmark run. They are explanatory
counts, not CPU performance counters and not comparable units across formats.
They can be obtained for one bitmap with `bitmap_inspect`.

| Format | `literal_units` | `fill_units` | `exception_units` | `control_units` |
|---|---|---|---|---|
| Raw | 0 | 0 | 0 | 0 |
| WAH | Literal codewords | Fill codewords | 0 | 0 |
| EWAH | Literal words | Nonzero fill spans in markers | 0 | Markers |
| PLWAH / CONCISE | Literal codewords | Fill codewords | Fill words carrying an exception | 0 |
| VAL | Literal segments | Fill segments | 0 | Physical 64-bit words |
| SBH | Literal bytes | Fill counters | 0 | Bytes used for fill counters |
| BBC | Explicit tail bytes | Explicit nonzero-length fill spans | Single-bit exception atoms | Atom headers, including terminator |
| Roaring | 0 | 0 | 0 | 0 |

Roaring uses `array_containers`, `bitmap_containers`, `run_containers`; the other
formats report zero in those columns. A BBC opposite-byte special case is not
a single-bit exception and is not counted in `exception_units`. Its implicit
trailing zero extent is not a stored fill. SBH's 4-byte region setting and
VAL's selector are included in payload size, but not in `control_units`.

## Planning an experiment

Use identical logical inputs and identical query files for all backends. Keep
N and k matched when investigating value pattern or alignment. For two inputs
record a=|A|, b=|B| and t=|A∩B|. Feasible overlaps satisfy
`max(0,a+b−N) ≤ t ≤ min(a,b)`. AND cardinality is t; OR cardinality is a+b−t.
For more operands `intersection_cardinality` is their common intersection.
The generator should check its own intended parameters against the actual data.

Separate construction, space, AND and OR results. Repeat whole runs, vary the
backend order, record medians and a measure of variation, and keep the seeds,
query files, generator parameters and scripts. Record the CPU, memory, OS,
compiler, Release settings, framework revision and format options. Do not mix
`BENCH_NATIVE=ON` and generic builds in one comparison. On shared or throttled
machines, repeat after conditions stabilize.

The example dataset and random query generator test the pipeline; they are not
a designed comparison of density, run placement, alignment or overlap. The
choice of controlled cases, analysis and explanations remains your project.
