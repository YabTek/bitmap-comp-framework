
param(
    [ValidateSet('plwah','concise','val','sbh','bbc')]
    [string]$AdditionalFormat = 'plwah',

    [ValidateSet('quick','full')]
    [string]$Preset = 'full',

    [switch]$Recreate
)

$ErrorActionPreference = 'Stop'
$exe = '.\build\generate_dataset.exe'

if (-not (Test-Path $exe)) {
    throw 'Build generate_dataset.cpp first.'
}

$root = "data\assignment1\$Preset"
$entries = New-Object 'System.Collections.Generic.List[object]'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

$lengths = if ($Preset -eq 'full') {
    @(65537, 262144, 1048579)
} else {
    @(65537)
}

$seeds = if ($Preset -eq 'full') {
    @(42, 2026)
} else {
    @(42)
}

$specs = @(
    @{pattern='sparse_scattered'; densities=@(0.01,0.10)},
    @{pattern='sparse_clustered'; densities=@(0.01,0.10)},
    @{pattern='dense_scattered'; densities=@(0.90,0.99)},
    @{pattern='dense_clustered'; densities=@(0.90,0.99)},
    @{pattern='mixed'; densities=@(0.50)}
)

if ($Preset -eq 'quick') {
    $specs = @(
        @{pattern='sparse_scattered'; densities=@(0.01)},
        @{pattern='sparse_clustered'; densities=@(0.01)},
        @{pattern='dense_scattered'; densities=@(0.90)},
        @{pattern='dense_clustered'; densities=@(0.90)},
        @{pattern='mixed'; densities=@(0.50)}
    )
}

function RoundInt([double]$value) {
    return [long][Math]::Round(
        $value, 0,
        [MidpointRounding]::AwayFromZero
    )
}

function Generate-One(
    $name, $options, $family, $pattern,
    $focusCase, $n, $k, $t, $seed, $density, $level
) {
    $dir = "$root\generated\$name"

    if ($Recreate -or -not (Test-Path "$dir\metadata.txt")) {
        $call = @(
            "--out=$dir",
            "--num-bits=$n",
            "--seed=$seed"
        ) + $options

        if ($Recreate) {
            $call += '--force=1'
        }

        & $exe @call

        if ($LASTEXITCODE -ne 0) {
            throw "Generator failed for $name"
        }
    }

    $entries.Add([pscustomobject]@{
        dataset = $name
        directory = $dir
        family = $family
        pattern = $pattern
        focus_format = $AdditionalFormat
        focus_case = $focusCase
        num_bits = $n
        density_requested = $density
        k_A = $k
        k_B = $k
        overlap_t = $t
        overlap_level = $level
        seed = $seed
        clusters = 8
        blocks = 8
        mixed_contrast = 0.2
    })
}

# General experiment cases
foreach ($n in $lengths) {
    foreach ($spec in $specs) {
        foreach ($density in $spec.densities) {
            $k = RoundInt ([double]$n * [double]$density)

            $pText = ([double]$density).ToString('0.00',$inv)
            $pName = $pText.Replace('.','p')

            $min = [Math]::Max(0L, 2L*$k - [long]$n)
            $max = $k

            if ($spec.pattern -eq 'mixed') {
                $overlaps = @(
                    (RoundInt (0.6 * $k)),
                    (RoundInt (0.8 * $k)),
                    $k
                )
            } else {
                $overlaps = @(
                    $min,
                    (RoundInt (($min+$max)/2.0)),
                    $max
                )
            }

            if ($Preset -eq 'quick') {
                $overlaps = @($overlaps[1])
            }

            foreach ($seed in $seeds) {
                for ($j=0; $j -lt $overlaps.Count; $j++) {
                    $t = [long]$overlaps[$j]

                    $name = "N${n}_$($spec.pattern)_p${pName}_s${seed}_t${t}"

                    $options = @(
                        "--pattern=$($spec.pattern)",
                        "--density=$pText",
                        "--overlap=$t",
                        '--clusters=8',
                        '--blocks=8',
                        '--mixed-contrast=0.20'
                    )

                    Generate-One `
                        $name $options 'core' $spec.pattern '' `
                        $n $k $t $seed $pText $j
                }
            }
        }
    }
}

# Experiments targeting the selected fifth format.
$periods = @{
    plwah = 248
    concise = 248
    val = 240
    sbh = 1792
    bbc = 128
}

$onesPerPeriod = @{
    plwah = 2
    concise = 2
    val = 68
    sbh = 896
    bbc = 64
}

$caseNames = if ($AdditionalFormat -in @('plwah','concise')) {
    @('eligible','ineligible')
} else {
    @('aligned','shifted')
}

$repeatCounts = if ($Preset -eq 'full') {
    @(1000,2000)
} else {
    @(100)
}

$focusSeeds = if ($Preset -eq 'full') {
    @(42,2026)
} else {
    @(42)
}

foreach ($repeats in $repeatCounts) {
    $n = [long]$periods[$AdditionalFormat] * $repeats
    $k = [long]$onesPerPeriod[$AdditionalFormat] * $repeats

    $lower = [Math]::Max(0L, 2L*$k - $n)

    $tValues = if ($Preset -eq 'full') {
        @(
            (RoundInt (($lower+$k)/2.0)),
            $k
        )
    } else {
        @($k)
    }

    foreach ($seed in $focusSeeds) {
        for ($j=0; $j -lt $tValues.Count; $j++) {
            $t = [long]$tValues[$j]

            foreach ($case in $caseNames) {
                $shift = if ($AdditionalFormat -eq 'bbc') {
                    3
                } else {
                    7
                }

                $options = @(
                    '--pattern=focused',
                    "--focus-format=$AdditionalFormat",
                    "--focus-case=$case",
                    "--shift=$shift",
                    "--overlap=$t"
                )

                $name = "focus_${AdditionalFormat}_${case}_N${n}_s${seed}_t${t}"

                Generate-One `
                    $name $options 'focused' 'focused' `
                    $case $n $k $t $seed `
                    ([string]([double]$k/$n)) $j
            }
        }
    }
}

# Correctness and boundary cases
foreach ($n in @(65537,65539)) {
    foreach ($pattern in @('empty','full')) {
        $density = if ($pattern -eq 'empty') {
            0.0
        } else {
            1.0
        }

        $k = if ($density -eq 0.0) {
            0L
        } else {
            [long]$n
        }

        $patternName = if ($density -eq 0) {
            'sparse_scattered'
        } else {
            'dense_scattered'
        }

        $options = @(
            "--pattern=$patternName",
            "--density=$([double]$density)",
            "--overlap=$k"
        )

        Generate-One `
            "edge_${pattern}_N${n}" $options `
            'edge' $pattern '' $n $k $k 42 $density 'edge'
    }

    foreach ($case in @('shifted','identical','disjoint')) {
        $options = @(
            '--pattern=boundary',
            "--boundary-case=$case"
        )

        $k = if ($n -eq 65537) {
            11L
        } else {
            12L
        }

        $t = if ($case -eq 'identical') {
            $k
        } elseif ($case -eq 'disjoint') {
            0L
        } else {
            8L
        }

        Generate-One `
            "edge_boundary_${case}_N${n}" $options `
            'edge' 'boundary' $case `
            $n $k $t 42 '' 'edge'
    }
}

New-Item -ItemType Directory -Force $root | Out-Null

$manifest = "$root\manifest.csv"

$entries | Export-Csv `
    -Path $manifest `
    -NoTypeInformation `
    -Encoding UTF8

Write-Host "Prepared $($entries.Count) dataset pairs"
Write-Host "Manifest: $manifest"
