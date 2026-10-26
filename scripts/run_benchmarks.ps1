
param(
    [ValidateSet('plwah','concise','val','sbh','bbc')]
    [string]$AdditionalFormat = 'plwah',

    [ValidateSet('quick','full')]
    [string]$Preset = 'full',

    [int]$Repetitions = 7,
    [int]$Warmup = 2,
    [int]$BuildRepetitions = 3,
    [int]$LimitDatasets = 0,

    [switch]$Overwrite
)

$ErrorActionPreference = 'Stop'

# Check measurement settings.
if (
    $Repetitions -le 0 -or
    $BuildRepetitions -le 0 -or
    $Warmup -lt 0
) {
    throw 'Invalid repetition settings.'
}

$bench = '.\build\bitmap_bench.exe'
$manifest = "data\assignment1\$Preset\manifest.csv"

if (-not (Test-Path $bench)) {
    throw "Missing executable: $bench"
}

if (-not (Test-Path $manifest)) {
    throw "Missing manifest: $manifest"
}

# Create the same two queries for every representation.
$queryFile = "queries\assignment1_pair.txt"

New-Item -ItemType Directory -Force queries | Out-Null

Set-Content `
    -Path $queryFile `
    -Encoding ascii `
    -Value @(
        'AND 2 0 1',
        'OR 2 0 1'
    )

# Output directory.
$root = "results\assignment1\$Preset"

New-Item -ItemType Directory -Force $root | Out-Null

# Four required baseline representations.
$backends = @(
    'raw',
    'wah',
    'ewah',
    'roaring'
)

# Add the fifth selected representation.
# VAL-WAH and SBH can be tested with multiple settings.
$additionalVariants = switch ($AdditionalFormat) {
    'val' {
        @('val15','val30','val60','valauto')
    }

    'sbh' {
        @('sbh32','sbh128','sbh256')
    }

    default {
        @($AdditionalFormat)
    }
}

$backends += $additionalVariants

# Load the dataset collection.
$rows = @(Import-Csv -Path $manifest)

# Optionally benchmark only the first few datasets.
if ($LimitDatasets -gt 0) {
    $rows = @(
        $rows | Select-Object -First $LimitDatasets
    )
}

if ($rows.Count -eq 0) {
    throw "Manifest has no datasets"
}

$executed = 0

foreach ($idx in 0..($rows.Count - 1)) {
    $row = $rows[$idx]

    # Change execution order between datasets to reduce
    # systematic first/last backend timing bias.
    $order = @()

    for ($j = 0; $j -lt $backends.Count; $j++) {
        $order += $backends[
            ($idx + $j) % $backends.Count
        ]
    }

    foreach ($variant in $order) {

        $backendName = if ($variant -match '^val') {
            'val'
        } elseif ($variant -match '^sbh') {
            'sbh'
        } else {
            $variant
        }

        $out = "$root\$($row.dataset)\$variant.csv"

        # Skip existing results unless overwrite is requested.
        if ((Test-Path $out) -and (-not $Overwrite)) {
            continue
        }

        $flags = @(
            "--backend=$backendName",
            "--dataset=$($row.directory)",
            "--queries=$queryFile",
            "--out=$out",
            "--repetitions=$Repetitions",
            "--warmup=$Warmup",
            "--build-repetitions=$BuildRepetitions",
            '--verify=1'
        )

        # Configuration for VAL-WAH.
        if ($variant -match '^val(15|30|60|auto)$') {
            $flags += "--val-segment-bits=$($Matches[1])"
        }

        # Configuration for SBH.
        if ($variant -match '^sbh(32|128|256)$') {
            $flags += "--sbh-groups=$($Matches[1])"
        }

        # Call professor's benchmark executable.
        & $bench @flags

        if ($LASTEXITCODE -ne 0) {
            throw (
                "Benchmark failed for dataset " +
                "$($row.dataset), format $variant"
            )
        }

        $executed++
    }
}

Write-Host "Created $executed benchmark CSV files"
Write-Host "Results directory: $root"
Write-Host "Analyze results using:"
Write-Host "python scripts\analyze_results.py $root"
