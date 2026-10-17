param(
	<#
	Portable benchmark runner for CI and for local use.

	It builds benchmarks/bench_vs_recursive_mutex.cpp through CMake, so there is
	no hard-coded Visual Studio path (the previous version only worked on the
	machine it was written on). Every (scenario, impl) pair runs -Repeats times
	and the MEDIAN total throughput is reported, because one run on a shared
	runner is far too noisy to compare.

	With -Baseline it compares the new speedups against a previously generated
	results file and fails when a scenario regresses by more than -RegressionPct.
	That is the PR gate; pass -RegressionPct 0 on scheduled runs, where runner
	drift should not fail the job.

	The compiler must be on PATH:
	  * msvc  -> run from a Developer PowerShell, or in CI after
	             ilammy/msvc-dev-cmd (cl.exe and ninja on PATH)
	  * mingw -> g++ on PATH
	#>
	[ValidateSet("msvc", "mingw", "both")]
	[string]$Toolchain = "both",
	[double]$Seconds = 3.0,
	[ValidateRange(1, 25)]
	[int]$Repeats = 1,
	[string]$OutDir = "",
	[ValidateSet("full", "none")]
	[string]$BarChart = "full",
	[string]$Baseline = "",
	[double]$RegressionPct = 15,
	# Comma-separated scenario prefixes that the gate may fail on; empty = all.
	[string]$GateScenarios = "",
	[switch]$Quiet
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BenchDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = Join-Path $BenchDir "results" }

$scenarios = @(
	@{ Name = "S1 read-heavy nested"; Mode = "rw"; R = 15; W = 1; ImplRsm = "rsm" },
	@{ Name = "S2 write-heavy nested"; Mode = "rw"; R = 2; W = 12; ImplRsm = "rsm" },
	@{ Name = "S3 balanced nested"; Mode = "rw"; R = 8; W = 8; ImplRsm = "rsm" },
	@{ Name = "S4 mixed ~20% write"; Mode = "mixed"; N = 16; WF = 0.20; ImplRsm = "rsm" },
	@{ Name = "S5 mixed ~5% write"; Mode = "mixed"; N = 16; WF = 0.05; ImplRsm = "rsm" },
	@{ Name = "S6 sharded x8 ~15% write"; Mode = "sharded"; N = 16; WF = 0.15; ImplRsm = "rsm" },
	@{ Name = "S7 exclusive-only overhead"; Mode = "rw"; R = 8; W = 8; ImplRsm = "rsm_exclusive" }
)

function Require-Compiler([string]$kind) {
	$exe = if ($kind -eq "msvc") { "cl" } else { "g++" }
	if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) {
		throw "$exe is not on PATH. For MSVC run from a Developer PowerShell (in CI: ilammy/msvc-dev-cmd); for MinGW put g++ on PATH."
	}
}

function Build-Bench([string]$kind) {
	$buildDir = Join-Path $BenchDir "build-$kind"
	$cfg = @("-S", $Root, "-B", $buildDir, "-DRSM_BUILD_BENCHMARKS=ON", "-DCMAKE_BUILD_TYPE=Release", "-G", "Ninja")
	$cfg += if ($kind -eq "mingw") { @("-DCMAKE_CXX_COMPILER=g++") } else { @("-DCMAKE_CXX_COMPILER=cl") }
	& cmake @cfg | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $kind" }
	& cmake --build $buildDir --target bench_rsm | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $kind" }

	$exe = Get-ChildItem -Path $buildDir -Recurse -Filter "bench_rsm*.exe" |
		Sort-Object LastWriteTime -Descending | Select-Object -First 1
	if (-not $exe) { throw "bench_rsm binary not found under $buildDir" }
	return $exe.FullName
}

function Invoke-Config($exe, $impl, $sc) {
	$args = @("--seconds", "$Seconds", "--impl", $impl, "--scenario", $sc.Name, "--mode", $sc.Mode)
	if ($sc.Mode -eq "rw") { $args += @("--readers", "$($sc.R)", "--writers", "$($sc.W)") }
	else { $args += @("--workers", "$($sc.N)", "--write-fraction", "$($sc.WF)") }

	$samples = New-Object System.Collections.Generic.List[double]
	$last = ""
	for ($i = 0; $i -lt $Repeats; $i++) {
		$output = & $exe @args 2>&1
		if ($LASTEXITCODE -ne 0) {
			throw "bench failed impl=$impl scenario=$($sc.Name) exit=$LASTEXITCODE output=$output"
		}
		$last = [string]$output.Trim()
		$p = $last.Split("|")
		$samples.Add((([double]$p[3]) + ([double]$p[4])) / [double]$p[5])
	}
	$sorted = @($samples | Sort-Object)
	$median = if ($sorted.Count % 2 -eq 1) {
		$sorted[[int][math]::Floor($sorted.Count / 2)]
	}
	else {
		($sorted[$sorted.Count / 2 - 1] + $sorted[$sorted.Count / 2]) / 2.0
	}
	return [pscustomobject]@{ Raw = $last; MedianOpsPerSecond = $median }
}

function Get-CompilerLabel([string]$kind) {
	if ($kind -eq "msvc") {
		$v = (cmd /c "cl 2>&1" | Select-String -Pattern "Version" | Select-Object -First 1).Line
		if ($v) { return ("MSVC (" + ($v -replace "\s+", " ") + ")").Trim() }
		return "MSVC"
	}
	$v = (& g++ --version | Select-Object -First 1)
	return "MinGW-GCC ($v)".Trim()
}

function Read-BaselineSpeedups([string]$path) {
	# Recompute from the raw "<scenario>|<impl>|...|<seconds>" lines that the report
	# keeps in a fenced block, so the parser does not depend on table formatting.
	$speedups = @{}
	if (-not (Test-Path $path)) { return $speedups }
	$current = ""
	$base = 0.0
	foreach ($line in Get-Content -Path $path) {
		if ($line -notmatch "\|") { continue }
		$trimmed = $line.TrimStart()
		if ($trimmed.StartsWith("|") -or $trimmed.StartsWith("#")) { continue }
		$p = $line.Split("|")
		if ($p.Count -lt 6) { continue }
		$total = (([double]$p[3]) + ([double]$p[4])) / [double]$p[5]
		if ($p[0] -ne $current) { $current = $p[0]; $base = $total }
		elseif ($base -gt 0) { $speedups[$current] = $total / $base }
	}
	return $speedups
}

function Write-Report([string]$toolchainName, [string]$compilerLabel, $rows, [string]$outFile, [string]$backend) {
	$order = @()
	foreach ($r in $rows) { if ($order -notcontains $r.Scenario) { $order += $r.Scenario } }

	$md = New-Object System.Collections.Generic.List[string]
	$md.Add("# Benchmark results: $toolchainName")
	$md.Add("")
	$md.Add("- Toolchain: $compilerLabel")
	$md.Add("- Generated by ``benchmarks/run_bench.ps1``: medians of $Repeats run(s), $([math]::Round($Seconds, 1))s per run")
	$commit = $env:GITHUB_SHA
	if ([string]::IsNullOrWhiteSpace($commit)) { $commit = (& git -C $Root rev-parse --short HEAD 2>$null) }
	$md.Add("- Commit: ``$commit``")
	if ($env:GITHUB_RUN_ID) {
		$md.Add("- Runner: GitHub Actions, job $($env:GITHUB_RUN_ID) (shared hardware: expect drift between runs)")
	}
	else {
		$md.Add("- Runner: local")
	}
	$md.Add("- Host hw_concurrency: $([Environment]::ProcessorCount)")
	if (-not [string]::IsNullOrWhiteSpace($backend)) { $md.Add("- Backend: $backend") }
	$md.Add("- Dataset: 1,048,576 x uint32 (~4 MiB); nested reenter depth 3; read window 24 / write window 12")
	$md.Add("")
	$md.Add("> Absolute numbers are specific to the machine; the meaningful column is the speedup vs ``recursive_mutex`` measured in the same run.")
	$md.Add("")
	if ([Environment]::ProcessorCount -lt 8) {
		$md.Add("> **Low-core host ($([Environment]::ProcessorCount) cores).** Shared locking pays off when readers outnumber cores: below ~8 cores the")
		$md.Add("> readers barely overlap and RSM can look slower than ``recursive_mutex``. These numbers are for run-to-run regression on this")
		$md.Add("> machine, not a statement about a many-core desktop.")
		$md.Add("")
	}

	if ($BarChart -ne "none") {
		$md.Add("## Speedup at a glance")
		$md.Add("")
		$md.Add("Bar length is proportional to total-ops/s vs ``recursive_mutex`` (1 block = 0.5x).")
		$md.Add("")
		$md.Add('```')
		foreach ($sc in $order) {
			$baselineRow = $rows | Where-Object { $_.Scenario -eq $sc -and $_.Impl -eq "recursive_mutex" } | Select-Object -First 1
			$rsm = $rows | Where-Object { $_.Scenario -eq $sc -and $_.Impl -ne "recursive_mutex" } | Select-Object -First 1
			if (-not $baselineRow -or -not $rsm -or $baselineRow.MedianOpsPerSecond -le 0) { continue }
			$sp = $rsm.MedianOpsPerSecond / $baselineRow.MedianOpsPerSecond
			$blocks = [string]::new([char]0x2588, [math]::Max(0, [int][math]::Round($sp * 2)))
			$md.Add(("{0}{1,7:N2}x  {2}" -f $sc.PadRight(26), $sp, $blocks))
		}
		$md.Add('```')
		$md.Add("")
	}

	$md.Add("## Per-scenario verdict")
	$md.Add("")
	$md.Add("| Scenario | Baseline ops/s | RSM ops/s | Speedup | Verdict |")
	$md.Add("|---|---:|---:|---:|---|")
	foreach ($sc in $order) {
		$baselineRow = $rows | Where-Object { $_.Scenario -eq $sc -and $_.Impl -eq "recursive_mutex" } | Select-Object -First 1
		$rsm = $rows | Where-Object { $_.Scenario -eq $sc -and $_.Impl -ne "recursive_mutex" } | Select-Object -First 1
		if (-not $baselineRow -or -not $rsm) { continue }
		$sp = if ($baselineRow.MedianOpsPerSecond -gt 0) { $rsm.MedianOpsPerSecond / $baselineRow.MedianOpsPerSecond } else { 0 }
		$verdict = if ($sp -ge 1.05) { "faster" } elseif ($sp -ge 0.95) { "parity" } else { "slower" }
		$md.Add("| $sc | $([int64]$baselineRow.MedianOpsPerSecond) | $([int64]$rsm.MedianOpsPerSecond) | **$([math]::Round($sp, 2))x** | $verdict |")
	}
	$md.Add("")
	$md.Add("<details><summary>Raw lines (last run of each pair)</summary>")
	$md.Add("")
	$md.Add('```')
	foreach ($r in $rows) { $md.Add($r.Raw) }
	$md.Add('```')
	$md.Add("</details>")
	$md.Add("")

	New-Item -ItemType Directory -Force -Path (Split-Path $outFile) | Out-Null
	# UTF-8 without BOM, independent of the PowerShell version.
	$text = ($md -join "`n") + "`n"
	[System.IO.File]::WriteAllText($outFile, $text, (New-Object System.Text.UTF8Encoding($false)))
	if (-not $Quiet) { Write-Host "Wrote $outFile" }
}

function Invoke-Toolchain([string]$kind) {
	Require-Compiler $kind
	$label = Get-CompilerLabel $kind
	if (-not $Quiet) { Write-Host "Building bench_rsm ($kind) ..." }
	$exe = Build-Bench $kind

	# Two families of file on purpose:
	#   results/<toolchain>.md     - hand-measured on a real workstation, the
	#                                representative numbers quoted in the README;
	#   results/ci-<toolchain>.md  - the 4-core GitHub runner, kept only so run
	#                                to run drift can be detected and gated on
	#                                identical hardware.
	$name = if ($kind -eq "msvc") { "msvc" } else { "mingw-gcc" }
	if ($env:GITHUB_ACTIONS -eq "true") { $name = "ci-$name" }
	# Which backend these numbers come from. On MinGW it changed from
	# std::shared_mutex/winpthreads to the native SRWLOCK, and the two are not
	# comparable, so the report has to say which one it measured.
	$backendLine = (& $exe --backend 2>&1 | Select-Object -First 1)
	if (-not $Quiet) { Write-Host "backend: $backendLine" }

	$outFile = Join-Path $OutDir "$name.md"
	$rows = New-Object System.Collections.Generic.List[object]
	foreach ($sc in $scenarios) {
		if (-not $Quiet) { Write-Host ">> [$kind] $($sc.Name)" }
		$baselineRow = Invoke-Config $exe "recursive_mutex" $sc
		$impl = Invoke-Config $exe $sc.ImplRsm $sc
		$rows.Add([pscustomobject]@{ Scenario = $sc.Name; Impl = "recursive_mutex"; Raw = $baselineRow.Raw; MedianOpsPerSecond = $baselineRow.MedianOpsPerSecond })
		$rows.Add([pscustomobject]@{ Scenario = $sc.Name; Impl = $sc.ImplRsm; Raw = $impl.Raw; MedianOpsPerSecond = $impl.MedianOpsPerSecond })
	}

	$backendText = if ($backendLine -match "^backend\|([^|]+)\|(\d+)$") {
		"$($Matches[1]), $($Matches[2]) B"
	}
	else { $backendLine }
	Write-Report $(if ($kind -eq "msvc") { "MSVC" } else { "MinGW-GCC" }) $label $rows $outFile $backendText

	if (-not [string]::IsNullOrWhiteSpace($Baseline)) {
		$old = Read-BaselineSpeedups $Baseline
		$new = Read-BaselineSpeedups $outFile
		$bad = @()
		$gated = @()
		if (-not [string]::IsNullOrWhiteSpace($GateScenarios)) {
			$gated = @($GateScenarios.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ })
		}
		foreach ($sc in $new.Keys) {
			if (-not $old.ContainsKey($sc) -or $old[$sc] -le 0) { continue }
			$isGated = ($gated.Count -eq 0) -or (@($gated | Where-Object { $sc.StartsWith($_) }).Count -gt 0)
			$delta = ($new[$sc] - $old[$sc]) / $old[$sc] * 100.0
			$state = if (-not $isGated) { "not gated" } elseif ($RegressionPct -gt 0 -and $delta -le -$RegressionPct) { "REGRESSION" } elseif ($delta -ge $RegressionPct) { "faster" } else { "within threshold" }
			if (-not $Quiet) {
				$deltaText = if ($delta -ge 0) { "+" + $delta.ToString("N1") } else { $delta.ToString("N1") }
				"{0,-32} {1,7:N2}x -> {2,7:N2}x  ({3,6}%)  {4}" -f $sc, $old[$sc], $new[$sc], $deltaText, $state | Write-Host
			}
			if ($isGated -and $RegressionPct -gt 0 -and $delta -le -$RegressionPct) { $bad += $sc }
		}
		if ($bad.Count -gt 0) {
			throw ("benchmark regression beyond {0}%: {1}" -f $RegressionPct, ($bad -join ", "))
		}
		if (-not $Quiet) { Write-Host "Baseline comparison: nothing beyond $RegressionPct%" }
	}
}

if ($Toolchain -eq "mingw" -or $Toolchain -eq "both") { Invoke-Toolchain "mingw" }
if ($Toolchain -eq "msvc" -or $Toolchain -eq "both") { Invoke-Toolchain "msvc" }

if (-not $Quiet) { Write-Host "ALL DONE" }
