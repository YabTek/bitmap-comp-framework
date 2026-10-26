
import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path


def median(values):
    return statistics.median(values)


def iqr(values):
    """Calculate the interquartile range."""
    if len(values) < 2:
        return 0.0

    q1, _, q3 = statistics.quantiles(
        values, n=4, method="inclusive"
    )

    return q3 - q1


def write_csv(path, rows):
    """Write result rows into a CSV file."""
    if not rows:
        return

    with path.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(
            file,
            fieldnames=list(rows[0])
        )

        writer.writeheader()
        writer.writerows(rows)


def number(row, column, default=0):
    """Read an optional numeric CSV field."""
    value = row.get(column, "")

    if value in (None, ""):
        return default

    return float(value)


def plot_comparison(summaries, output_dir):
    """Plot matched sparse scattered and clustered datasets."""

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("Matplotlib is not installed.")
        print("CSV summaries were still generated.")
        return

    scattered = [
        row for row in summaries
        if row["pattern"] == "sparse_scattered"
    ]

    clustered_keys = {
        (
            row["N"],
            row["density"],
            row["t"],
            row["seed"]
        )
        for row in summaries
        if row["pattern"] == "sparse_clustered"
    }

    # Find a comparison with the same N, density, overlap and seed.
    matching = next(
        (
            row for row in scattered
            if (
                row["N"],
                row["density"],
                row["t"],
                row["seed"]
            ) in clustered_keys
        ),
        None
    )

    if matching is None:
        print("No matched sparse pair found.")
        print("Skipping comparison charts.")
        return

    n = matching["N"]
    density = matching["density"]
    overlap = matching["t"]
    seed = matching["seed"]

    selected = [
        row for row in summaries
        if (
            row["N"] == n
            and row["density"] == density
            and row["t"] == overlap
            and row["seed"] == seed
            and row["pattern"] in {
                "sparse_scattered",
                "sparse_clustered"
            }
        )
    ]

    patterns = [
        "sparse_scattered",
        "sparse_clustered"
    ]

    charts = [
        (
            "size_bytes",
            "AND",
            "storage.png",
            "Saved size (bytes)"
        ),
        (
            "median_query_ns",
            "AND",
            "and_time.png",
            "AND median time (ns)"
        ),
        (
            "median_query_ns",
            "OR",
            "or_time.png",
            "OR median time (ns)"
        )
    ]

    for field, operation, filename, ylabel in charts:

        values = {
            (row["pattern"], row["variant"]): row[field]
            for row in selected
            if row["operation"] == operation
        }

        # Only compare formats measured for both patterns.
        variants = sorted(
            set(
                variant
                for pattern, variant in values
                if pattern == patterns[0]
            )
            &
            set(
                variant
                for pattern, variant in values
                if pattern == patterns[1]
            )
        )

        if not variants:
            print(f"No complete comparison for {filename}")
            continue

        fig, ax = plt.subplots(figsize=(10, 5))

        bar_width = 0.38

        for j, pattern in enumerate(patterns):

            positions = [
                i + (j - 0.5) * bar_width
                for i in range(len(variants))
            ]

            heights = [
                values[(pattern, variant)]
                for variant in variants
            ]

            ax.bar(
                positions,
                heights,
                width=bar_width,
                label=pattern
            )

        ax.set_xticks(
            range(len(variants)),
            variants
        )

        ax.set_ylabel(ylabel)

        ax.set_title(
            f"Matched inputs: N={n}, "
            f"density={density}, "
            f"overlap={overlap}, seed={seed}"
        )

        ax.legend()
        fig.tight_layout()

        fig.savefig(
            output_dir / filename,
            dpi=150
        )

        plt.close(fig)


def analyze(results_dir):
    """Read benchmark CSV files and generate analysis results."""

    # Locate the dataset manifest.
    manifest_path = (
        Path("data")
        / "assignment1"
        / results_dir.name
        / "manifest.csv"
    )

    if not manifest_path.exists():
        raise FileNotFoundError(
            f"Missing {manifest_path}. "
            "Run this script from the repository root."
        )

    # Read dataset metadata.
    with manifest_path.open(
        newline="",
        encoding="utf-8-sig"
    ) as file:

        manifest = {
            row["dataset"]: row
            for row in csv.DictReader(file)
        }

    groups = defaultdict(list)
    files_read = 0

    # Read results from all dataset directories.
    for csv_path in sorted(results_dir.glob("*/*.csv")):

        dataset = csv_path.parent.name

        if dataset not in manifest:
            raise ValueError(
                f"Dataset {dataset} is not in the manifest"
            )

        with csv_path.open(
            newline="",
            encoding="utf-8-sig"
        ) as file:

            rows = list(csv.DictReader(file))

        if not rows:
            raise ValueError(
                f"Empty benchmark output: {csv_path}"
            )

        for row in rows:

            # Only accept verified benchmark results.
            if row.get("verified", "").lower() not in {
                "1", "true", "yes"
            }:
                raise ValueError(
                    f"Unverified measurement in {csv_path}"
                )

            if row.get("dataset") != dataset:
                raise ValueError(
                    f"Dataset mismatch in {csv_path}"
                )

            if row.get("query_type") not in {"AND", "OR"}:
                raise ValueError(
                    f"Unexpected query operation in {csv_path}"
                )

            if row.get("query_width") != "2":
                raise ValueError(
                    f"Expected two-operand query in {csv_path}"
                )

            key = (
                dataset,
                csv_path.stem,
                row["query_type"]
            )

            groups[key].append(row)

        files_read += 1

    if not groups:
        raise ValueError(
            f"No benchmark CSV results found in {results_dir}"
        )

    # Calculate per-dataset statistics.
    summaries = []

    for (dataset, variant, operation), rows in sorted(
        groups.items()
    ):

        info = manifest[dataset]
        first = rows[0]

        query_times = [
            float(row["query_time_ns"])
            for row in rows
        ]

        build_times = [
            float(row["build_time_ms"])
            for row in rows
        ]

        summaries.append({
            "dataset": dataset,
            "family": info["family"],
            "pattern": info["pattern"],
            "focus_case": info["focus_case"],
            "N": int(info["num_bits"]),
            "density": info["density_requested"],
            "k_A": int(info["k_A"]),
            "k_B": int(info["k_B"]),
            "t": int(info["overlap_t"]),
            "overlap_level": info["overlap_level"],
            "seed": info["seed"],
            "variant": variant,
            "backend": first["backend"],
            "operation": operation,

            # Storage and construction measurements.
            "size_bytes": int(first["compressed_bytes"]),
            "payload_bytes": int(
                number(first, "payload_bytes")
            ),
            "build_median_ms": median(build_times),

            # Query performance.
            "median_query_ns": median(query_times),
            "iqr_query_ns": iqr(query_times),
            "min_query_ns": min(query_times),
            "max_query_ns": max(query_times),
            "repetitions": len(query_times),

            # Result cardinality.
            "result_cardinality": int(
                first["result_cardinality"]
            ),

            # Structural statistics.
            "literal_units": int(
                number(first, "literal_units")
            ),
            "fill_units": int(
                number(first, "fill_units")
            ),
            "exception_units": int(
                number(first, "exception_units")
            ),
            "control_units": int(
                number(first, "control_units")
            ),
            "array_containers": int(
                number(first, "array_containers")
            ),
            "bitmap_containers": int(
                number(first, "bitmap_containers")
            ),
            "run_containers": int(
                number(first, "run_containers")
            )
        })

    # Create analysis output directory.
    output_dir = results_dir / "analysis"
    output_dir.mkdir(
        parents=True,
        exist_ok=True
    )

    write_csv(
        output_dir / "summary_by_dataset.csv",
        summaries
    )

    # Aggregate results across independent random seeds.
    by_setting = defaultdict(list)

    for row in summaries:

        key = (
            row["family"],
            row["pattern"],
            row["focus_case"],
            row["N"],
            row["density"],
            row["overlap_level"],
            row["variant"],
            row["operation"]
        )

        by_setting[key].append(row)

    aggregates = []

    for key, items in sorted(by_setting.items()):

        (
            family,
            pattern,
            focus_case,
            n,
            density,
            overlap,
            variant,
            operation
        ) = key

        query_medians = [
            item["median_query_ns"]
            for item in items
        ]

        aggregates.append({
            "family": family,
            "pattern": pattern,
            "focus_case": focus_case,
            "N": n,
            "density": density,
            "overlap_level": overlap,
            "variant": variant,
            "operation": operation,
            "seed_count": len(items),

            "size_median_bytes": median([
                item["size_bytes"]
                for item in items
            ]),

            "build_median_ms": median([
                item["build_median_ms"]
                for item in items
            ]),

            "query_median_ns": median(query_medians),
            "query_seed_min_ns": min(query_medians),
            "query_seed_max_ns": max(query_medians)
        })

    write_csv(
        output_dir / "summary_aggregated_by_seeds.csv",
        aggregates
    )

    # Generate comparison graphs.
    plot_comparison(
        summaries,
        output_dir
    )

    print(f"Analyzed {files_read} CSV files.")
    print(f"Summaries and charts: {output_dir}")


if __name__ == "__main__":

    parser = argparse.ArgumentParser(
        description="Analyze bitmap compression benchmarks."
    )

    parser.add_argument(
        "results_directory",
        type=Path
    )

    args = parser.parse_args()

    analyze(args.results_directory)
