
import argparse
import csv
import pathlib
import struct


def read_words(path, n):
    blob = path.read_bytes()

    expected_bytes = ((n + 63) // 64) * 8

    if len(blob) != expected_bytes:
        raise AssertionError(
            f"{path}: {len(blob)} bytes, expected {expected_bytes}"
        )

    words = struct.unpack(
        "<" + "Q" * (expected_bytes // 8),
        blob
    )

    if n % 64 and words and (words[-1] >> (n % 64)):
        raise AssertionError(
            f"{path}: nonzero padding bits"
        )

    return words


def check_manifest(manifest):
    checked = 0

    with manifest.open(
        newline="",
        encoding="utf-8-sig"
    ) as handle:

        for row in csv.DictReader(handle):
            directory = pathlib.Path(row["directory"])

            metadata = dict(
                line.strip().split("=", 1)
                for line in
                (directory / "metadata.txt").read_text().splitlines()
                if "=" in line
            )

            n = int(row["num_bits"])

            assert int(metadata["num_bits"]) == n
            assert int(metadata["num_bitmaps"]) == 2
            assert metadata["format"] == "raw_uint64_le"
            assert int(metadata["word_bits"]) == 64

            a = read_words(
                directory / "bitmap_000.bin", n
            )

            b = read_words(
                directory / "bitmap_001.bin", n
            )

            ca = sum(x.bit_count() for x in a)
            cb = sum(x.bit_count() for x in b)

            intersection = sum(
                (x & y).bit_count()
                for x, y in zip(a, b)
            )

            expected_a = int(row["k_A"])
            expected_b = int(row["k_B"])
            t = int(row["overlap_t"])

            assert (ca, cb) == (expected_a, expected_b), (
                directory, ca, cb
            )

            assert intersection == t, (
                directory, intersection, t
            )

            assert float(metadata["density"]) >= 0.0

            if row["family"] in ("core", "focused"):
                assert f"k={ca}" in metadata["params"]
                assert f"t={intersection}" in metadata["params"]

            checked += 1

    print(
        f"PASS: validated {checked} dataset pairs; "
        "N/k/t and padding verified"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()

    parser.add_argument(
        "manifest",
        type=pathlib.Path
    )

    args = parser.parse_args()

    check_manifest(args.manifest)
