param(
	[ValidateSet("mingw", "msvc", "both")]
	[string]$Toolchain = "both",
	[double]$Seconds = 3.0
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BenchDir = $PSScriptRoot
Set-Location $BenchDir

$scenarios = @(
	@{ Name = "S1 read-heavy nested"; Mode = "rw"; R = 15; W = 1; ImplRsm = "rsm" },
	@{ Name = "S2 write-heavy nested"; Mode = "rw"; R = 2; W = 12; ImplRsm = "rsm" },
	@{ Name = "S3 balanced nested"; Mode = "rw"; R = 8; W = 8; ImplRsm = "rsm" },
	@{ Name = "S4 mixed ~20% write"; Mode = "mixed"; N = 16; WF = 0.20; ImplRsm = "rsm" },
	@{ Name = "S5 mixed ~5% write"; Mode = "mixed"; N = 16; WF = 0.05; ImplRsm = "rsm" },
	@{ Name = "S6 sharded x8 ~15% write"; Mode = "sharded"; N = 16; WF = 0.15; ImplRsm = "rsm" },
	@{ Name = "S7 exclusive-only overhead"; Mode = "rw"; R = 8; W = 8; ImplRsm = "rsm_exclusive" }
)

function Build-Mingw {
	g++ -std=c++17 -O2 -pthread ".\bench_vs_recursive_mutex.cpp" "-I$Root\include" -o ".\bench_rsm_mingw.exe"
	if ($LASTEXITCODE -ne 0) { throw "mingw build failed" }
}

function Build-Msvc {
	cmd /c "call `"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 >nul && cd /d `"$BenchDir`" && cl /nologo /std:c++17 /O2 /EHsc /MD /I`"$Root\include`" bench_vs_recursive_mutex.cpp /Fe:bench_rsm_msvc.exe"
	if ($LASTEXITCODE -ne 0) { throw "msvc build failed" }
}

function Invoke-Config($exe, $impl, $sc) {
	$args = @("--seconds", "$Seconds", "--impl", $impl, "--scenario", $sc.Name, "--mode", $sc.Mode)
	if ($sc.Mode -eq "rw") {
		$args += @("--readers", "$($sc.R)", "--writers", "$($sc.W)")
	}
	else {
		$args += @("--workers", "$($sc.N)", "--write-fraction", "$($sc.WF)")
	}
	$output = & $exe @args 2>&1
	if ($LASTEXITCODE -ne 0) {
		throw "bench failed impl=$impl scenario=$($sc.Name) exit=$LASTEXITCODE output=$output"
	}
	return [string]$output.Trim()
}

function Write-Report($toolchainName, $exe, $outFile) {
	$rows = New-Object System.Collections.Generic.List[string]
	foreach ($sc in $scenarios) {
		Write-Host ">> [$toolchainName] $($sc.Name) / recursive_mutex"
		$rows.Add((Invoke-Config $exe "recursive_mutex" $sc))
		Write-Host ">> [$toolchainName] $($sc.Name) / $($sc.ImplRsm)"
		$rows.Add((Invoke-Config $exe $sc.ImplRsm $sc))
	}

	$md = New-Object System.Collections.Generic.List[string]
	$md.Add("# Benchmark results: $toolchainName")
	$md.Add("")
	$md.Add("- Dataset: 1,048,576 x uint32 (~4 MiB)")
	$md.Add("- Nested reenter depth: 3 (outer ``try_*`` + 2)")
	$md.Add("- Read window 24 / write window 12")
	$md.Add("- Duration/config: ${Seconds}s")
	$md.Add("- Host hw_concurrency: $([Environment]::ProcessorCount)")
	$md.Add("- Launch mode: one process per (scenario, impl) pair")
	$md.Add("")
	$md.Add("| Scenario | Impl | R/W or N | Read ops | Write ops | Total ops/s | Read ops/s | Write ops/s | Total speedup |")
	$md.Add("|----------|------|----------|----------|-----------|-------------|------------|-------------|---------------|")

	$last = ""
	$base = 0.0
	foreach ($line in $rows) {
		$p = $line.Split("|")
		$scName = $p[0]; $impl = $p[1]; $rw = $p[2]
		$read = [double]$p[3]; $write = [double]$p[4]; $sec = [double]$p[5]
		if ($scName -ne $last) {
			$last = $scName
			$base = ($read + $write) / $sec
		}
		$total = ($read + $write) / $sec
		$rr = $read / $sec
		$wr = $write / $sec
		$sp = if ($base -gt 0) { $total / $base } else { 0 }
		$md.Add("| $scName | ``$impl`` | $rw | $([int64]$read) | $([int64]$write) | $([int]$total) | $([int]$rr) | $([int]$wr) | $([math]::Round($sp, 2))x |")
	}

	$md.Add("")
	$md.Add("Raw lines:")
	$md.Add("")
	$md.Add('```')
	foreach ($line in $rows) { $md.Add($line) }
	$md.Add('```')

	New-Item -ItemType Directory -Force -Path (Split-Path $outFile) | Out-Null
	$md | Set-Content -Path $outFile -Encoding UTF8
	Write-Host "Wrote $outFile"
}

if ($Toolchain -eq "mingw" -or $Toolchain -eq "both") {
	Build-Mingw
	Write-Report "MinGW-GCC" ".\bench_rsm_mingw.exe" ".\results\mingw-gcc.md"
}
if ($Toolchain -eq "msvc" -or $Toolchain -eq "both") {
	Build-Msvc
	Write-Report "MSVC" ".\bench_rsm_msvc.exe" ".\results\msvc.md"
}

Write-Host "ALL DONE"
