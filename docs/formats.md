# Representation and file-format notes

This document describes the supplied code, including choices that affect an
experiment. It is not a claim that these implementations reproduce the speed
or every optimization of an author's library. There are no external runtime
dependencies. All multibyte saved integers are little-endian.

## Logical positions and framing

Every representation receives the same `N` logical positions, numbered 0..N−1.
`N` is external to the compressed stream. The last group is zero-padded;
padding is never counted as a present position. Inputs with nonzero padding
are rejected. The supported universe is at most 2^32 bits, including Roaring's
largest 32-bit position. `N=0` and all-zero/all-one inputs are supported.

Within an integer literal, the earliest logical position is the **least
significant bit**. Thus position 0 is `...0001`, not the leftmost bit of a
conventional printed binary integer. In the handout, logical input strings
instead place earliest positions on the left. Do not equate the leftmost
printed logical character with the high bit of this framework's literal.
In particular, CONCISE's encoded position `p=1` flips the low bit, which is the
**first** logical position in our integer convention. The handout's CONCISE
example prints that low bit at the right of its logical group. The convention
changes the displayed position, not the exception mechanism or size.

A `.bmcf` file contains:

| Bytes | Field |
|---|---|
| 8 | ASCII `BMCF0001` (archive version 1) |
| 4 | Representation ID: Raw=1, WAH=2, Roaring=3, EWAH=4, CONCISE=5, PLWAH=6, VAL=7, SBH=8, BBC=9 |
| 4 | Reserved, zero |
| 8 | Logical bitmap length N |
| 8 | Number of bitmaps |
| repeated | 8-byte payload length followed by that bitmap's native payload |

The wrapper is specific to this framework, not part of a published bitmap
format. Its purpose is to retain the logical length and delimit streams.
`compressed_bytes` is **32 + 8 × number_of_bitmaps + sum(payload_lengths)**,
and equals the actual number of saved bytes. Raw uses the same wrapper.
This measures saved space, not resident memory. No checksums are stored;
validation catches structural errors, not all possible bit corruption.

## Raw (`raw`)

Payload: `ceil(N/64)` packed 64-bit words, 8 bytes each. Operations materialize
an output word array and apply the operation to corresponding words. This is
also the independent logical oracle used by the harness.

## WAH (`wah`)

Codeword width is **64**, logical group width **63**. This is wider than the
8-bit illustration in Lecture 3; it uses the same literal/fill mechanism.

* Literal: bit 63 is zero; bits 62..0 hold one logical group.
* Fill: bit 63 is one, bit 62 is the repeated value, bits 61..0 are the
  nonzero number of uniform groups.

The payload is only the sequence of 8-byte codewords. Equal adjacent fills are
merged. Native stream operations consume the smaller remaining span from the
two inputs, combine values, and emit a compressed result. They do not decode
the whole bitmap. Zero-padding in the last group can turn a final one fill
into a literal.

## EWAH (`ewah`)

Literal words contain **64 original bits** and have no flag. Each marker
precedes a fill and then its explicit literals. The 64-bit marker stores:

* bit 0: fill value;
* bits 1..32: number of repeated uniform words K;
* bits 33..63: number of following literal words M.

The payload is markers interleaved with literal words. An empty bitmap uses one
zero marker. This is the marker layout described in
[Git's EWAH bitmap documentation](https://git-scm.com/docs/bitmap-format),
but the native payload here is just the marker/literal stream; it does not
include Git's additional stream framing or its complete index file format.

Operations walk marker spans and literal words directly and emit new markers.
Marker overhead can outweigh the full-width literal saving for some patterns.

## PLWAH (`plwah`)

Configuration: **PLWAH-32, one exception, fixed counter**. A 32-bit word has
one flag; a literal holds 31 logical bits. In a fill the fields from high to
low are `1 | v:1 | p:5 | c:25`. The counter counts uniform groups. `p=0` means
no exception; otherwise **one following group** is added and its low-numbered
bit `p−1` is flipped. A counter does not include that following group.

Example: one bit at global position 93 in N=124 produces `0x82000003`,
meaning three zero groups and then a group with its first low bit set.
Counters that cannot fit are split into multiple fill words. An exceptional
group is attached only to the immediately preceding compatible fill.

This is the fixed-counter configuration explained in the handout. It does
not implement the paper's adaptive counter extension or multiple position
fields. These are optimization/configuration limits, not a restriction on
which logical bitmaps can be represented. AND/OR consume fills and the attached
exceptional groups as compressed spans, then re-encode the result.
Reference: [Deliège and Pedersen, EDBT 2010](https://openproceedings.org/2010/conf/edbt/DeliegeP10.pdf).

## CONCISE (`concise`)

The original 32-bit flag convention is used: **1 for a literal, 0 for a fill**.
Literals hold 31 logical bits. Fill fields are
`0 | v:1 | p:5 | (q−1):25`. A fill contains q groups in total. `p=0` means all
are uniform. Otherwise the **first** group has bit `p−1` flipped and the
remaining q−1 groups are uniform. A maximum-length fill is split.

A one-bit exception may also be represented as a q=1 fill. A following pure
fill of the same value can extend it. A fill preceding an exceptional group
cannot absorb that exception, unlike PLWAH. Stream queries account for the
exception before consuming the rest of a fill. Reference:
[Colantonio and Di Pietro, CONCISE](https://arxiv.org/abs/1004.0403).

## VAL-WAH (`val`)

Each bitmap selects s=15, 30 or 60. Every physical 64-bit word has a **4-bit
header and 60 payload bits**, packing 4, 2 or 1 segments respectively. The
first segment's flag is bit 63, the next flag bit 62, and so on. Unused flags
and payload slots are zero. Segment payloads are packed from the high end of
the 60-bit payload. A fill segment holds a value bit and an (s−1)-bit nonzero
group count; a literal holds s original bits.

A **one-byte selector** precedes the words. Its high nibble is s/15 and low
nibble is this framework's method ID 1 (WAH), giving `0x11`, `0x21`, `0x41`.
The selector byte numbering is a local convention; these payloads are not
claimed to be a third-party VAL-WAH library's file format.

`--val-segment-bits=15|30|60` forces a width. `auto` encodes all three and
chooses the smallest payload, breaking size ties in favor of the smaller s.
Selection is included in construction time. **The paper's λ-based selector is
not implemented.** A size-only selector should not be called a time/space
optimizer. References:
[Guzun et al., ICDE 2014](https://davidtchiu.github.io/research/Papers/guzun-ICDE2014.pdf).

Operations align different input widths to the smaller s. Uniform spans remain
counters, while a current larger literal is split into compatible smaller
pieces. The output uses that smaller width; it is not automatically reselected
after the query. No whole-bitmap conversion is performed.

## SBH (`sbh`)

Seven logical bits form a group. A literal byte has prefix 0 and seven data
bits. A zero fill uses prefix `10` and a one fill `11`. The low six bits store
a run count in base 64, **least significant digit first**. Consecutive bytes
with the same prefix continue that counter, except at a known super-bucket
boundary. The configured range 1..4095 groups needs at most two count bytes.
For example, 70 zero groups encode as `0x86 0x81` (6 + 64×1).

The payload starts with a framework-specific 4-byte unsigned super-bucket size,
followed by the SBH bytes. Fills are split at every region boundary. The last
region may contain fewer groups. Default: 128 groups, 896 original bits. This
implements the handout's two-value fill configuration; run polarity is not
limited to zeros.

AND/OR expand **one super-bucket from each operand**, apply packed word
operations, and re-encode that region before proceeding. All this work and
its temporary allocations are timed. There is no whole-bitmap decompression.
When stored inputs use different region sizes, the output uses the smaller
size; input cursors retain their own boundaries. This implementation does not
provide an auxiliary random-access region index
or tuned SIMD region kernels. References:
[Kim et al., Information Systems 2016](https://doi.org/10.1016/j.is.2016.07.004),
[Kim's thesis](https://s-space.snu.ac.kr/bitstream/10371/123116/1/000000024748.pdf).

## BBC (`bbc`)

This is a byte-stream implementation of the layout in
[EP0650264A1, Tables 1 and 2](https://patents.google.com/patent/EP0650264A1/en).
It supports **all five control families**, including cases omitted from the
handout's introductory example. A gap is a fill of zero or one bytes. Fields
below are shown high bit first:

| Control family | Fields | Meaning |
|---|---|---|
| Short fill + tail | `g:3 v:1 n:4`, g=0..3 | g fill bytes followed by n explicit bytes |
| Extended fill + tail | `100 v:1 n:4` + length | At least four fill bytes, then n explicit bytes |
| Short zero fill + exception | `101 g:2 p:3` | g zero bytes, then one byte with bit p set |
| Extended fill + exception | `110 v:2 p:3` + length | Fill, then one byte with bit p opposite to v; v must be 0 or 1 |
| Short one fill + exception | `111 g:2 p:3` | g one bytes, then one byte with bit p cleared |

For the first two families, n=1..15 is an explicit tail. **n=0** means the
fill is followed by one byte of the **opposite value**, with that byte omitted
from the stream. An all-zero control is the terminator, so no zero-fill,
zero-tail atom is emitted.

An extended length stores the gap length **in bits**. It is byte-aligned,
leaving its lowest three bits to store the number of subsequent length bytes.
The remaining length bits are little-endian. Four fill bytes are 32 bits:
length byte `0x20`. For 300 fill bytes, 2400 bits, the length bytes are
`0x61 0x09` (low field 1 means one additional byte).

Every encoded stream has a `0x00` terminator. Terminal zero bytes can be
implicit because N is retained. A terminal one fill is followed by one implied
zero byte via n=0; the decoder accepts this one zero padding byte beyond N.
Nonzero padding or other data beyond N is rejected.

The greedy encoder combines fills and tails, uses single-bit exception atoms
where applicable, and limits explicit tails to 15 bytes per control. It does
not seek a globally minimum partition of the byte stream. Operations walk
fills, explicit tails and exceptions as spans and emit a compressed result.
This supports arbitrary logical inputs, not only the handout's short-fill case.

## Roaring (`roaring`)

High 16 bits select a chunk; low 16 bits are values within it. Only nonzero
chunks are stored. Containers use sorted arrays (2 bytes per value), bitmaps
(8192 bytes), or run lists (2-byte count plus 4 bytes per run). Construction
and operations select the smallest **container payload**, preferring array at a size tie, and run over bitmap when those two tie
below the array cost. The complete saved size also includes portable headers.

Native payloads use the official
[Roaring portable format](https://github.com/RoaringBitmap/RoaringFormatSpec):
cookies, optional run-container bitmap, sorted keys/cardinalities, offsets
when required, and serialized containers. Run lengths are stored minus one.
The framework's outer archive remains separate from that portable payload.

Important implementation choices:

* Container optimization runs during construction and **after every query**.
  There is no separate `runOptimize()` measurement.
* Array/array and bitmap/bitmap have direct operations; mixed bitmap/array
  operations use the sparse values. Run/run operations merge intervals.
* A run mixed with an array or bitmap currently expands both to one 8192-byte
  container bitmap and re-optimizes the result. That cost is timed.
* Code is scalar C++17, with no SIMD and no CRoaring dependency.

These choices affect time and sometimes the overall size choice. Even where
the saved layout is interoperable, the implementation should not be described
as CRoaring or used to claim CRoaring's performance.
