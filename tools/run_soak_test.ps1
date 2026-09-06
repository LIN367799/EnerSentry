# tools/run_soak_test.ps1 - Soak / burn-in test (Phase 4 W6+, slice 25).
#
# Dual-process long-run stability test:
#   * DeviceSimulator --cli runs random_linkloss_stress.json (8 link-loss injections
#     over 300s window, auto-recover) for -Minutes
#   * ens_app --cli full pipeline (--data-dir --blackbox-dir --alarm-rules) for -Minutes-5
# Periodic sampling every 30s -> CSV:
#   time_s, simMemKB, appMemKB, appCpuPct, appHandles, sampleLines, dbSizeKB
#
# Judgement:
#   1) memory convergence: avg(last 10%) - avg(first 10%) < 20MB (no leak)
#   2) sample rate stability: rows/min (last third) >= 60% of (first third) (no decay)
#   3) app alive to the end (no crash / no hang)
# All pass -> PASS, exit 0. Any fail -> FAIL, exit 1.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/run_soak_test.ps1 -Minutes 60
#   Optional: -Scenario xxx.json -SampleSec 30 -OutDir <ascii-path> -BuildDir <path>
# NOTE: all paths ASCII-only (PS 5.1 CJK arg corruption).

param(
    [int]$Minutes = 60,
    [string]$Scenario = "random_linkloss_stress.json",
    [int]$SampleSec = 30,
    [string]$OutDir = "",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"

# Process wait helper (same rationale as tools/run_phase4_acceptance.ps1):
# Wait-Process throws when the target already exited and -ErrorAction cannot
# reliably suppress it -> would abort the soak run under $ErrorActionPreference='Stop'.
# ENCODING CONTRACT: THIS FILE MUST STAY PURE ASCII (no BOM, LF endings), because
# PowerShell 5.1 decodes BOM-less .ps1 as ANSI/GBK and CJK bytes break the parser.
function Wait-Proc {
    param([System.Diagnostics.Process]$Proc, [int]$TimeoutSec, [string]$Label)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $Proc.HasExited) {
        if ($sw.Elapsed.TotalSeconds -ge $TimeoutSec) {
            Write-Host "  [!] timeout ${TimeoutSec}s waiting for $Label (pid $($Proc.Id)) - killing"
            Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
            return $false
        }
        Start-Sleep -Milliseconds 500
    }
    return $true
}

$root = "D:\Study\Qt_host_application_Project\EnerSentry"
$bin  = Join-Path $root "bin\Debug"
if ([string]::IsNullOrEmpty($BuildDir)) {
    $localBuild = Join-Path $root "build\vs2022-debug-local"
    $teamBuild = Join-Path $root "build\vs2022-debug"
    if (Test-Path (Join-Path $localBuild "test_data")) { $BuildDir = $localBuild }
    else { $BuildDir = $teamBuild }
}
$td   = Join-Path $BuildDir "test_data"
$pt   = Join-Path $td "sim_pointtable_sample.json"
$rules = Join-Path $td "alarm_rules_sample.json"
$scen = Join-Path $td "scenarios\$Scenario"

if (-not (Test-Path $bin)) { Write-Host "ERR: $bin not found (run cmake --build first)"; exit 2 }
if (-not (Test-Path $td)) { Write-Host "ERR: $td not found (run cmake configure/build first, or pass -BuildDir)"; exit 2 }
if (-not (Test-Path $scen)) { Write-Host "ERR: scenario not found: $scen"; exit 2 }

if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = Join-Path $root "build\soak_out" }
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null
$simOut = Join-Path $OutDir "sim_out"
$dbOut  = Join-Path $OutDir "db"
$bbOut  = Join-Path $OutDir "bb"
$appLog = Join-Path $OutDir "app_stdout.txt"
$csv    = Join-Path $OutDir "soak.csv"
New-Item -ItemType Directory -Path $simOut, $dbOut, $bbOut | Out-Null

$port = 15041
$simSec = $Minutes * 60 + 10
$appSec = $Minutes * 60 - 5
if ($appSec -lt 20) { $appSec = 20 }

Write-Host "== Soak test =="
Write-Host "  duration: $Minutes min  scenario: $Scenario (link-loss injections)"
Write-Host "  sampling: every ${SampleSec}s -> $csv"

# ---- launch ----
$sim = Start-Process -FilePath (Join-Path $bin "DeviceSimulator.exe") -ArgumentList @(
    "--cli", "$simSec", "--port", "$port", "--pointtable", $pt,
    "--scenario", $scen, "--export-dir", $simOut) -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $OutDir "sim_stdout.txt")

Start-Sleep -Seconds 2

$app = Start-Process -FilePath (Join-Path $bin "ens_app.exe") -ArgumentList @(
    "--cli", "--point-table", $pt, "--host", "127.0.0.1", "--port", "$port",
    "--run-seconds", "$appSec", "--alarm-rules", $rules,
    "--data-dir", $dbOut, "--blackbox-dir", $bbOut) -PassThru -NoNewWindow `
    -RedirectStandardOutput $appLog

# ---- sampling loop ----
"time_s,simMemKB,appMemKB,appCpuPct,appHandles,sampleLines,dbSizeKB" | Out-File -Encoding ascii $csv
$startTime = Get-Date
$prevCpu = 0.0
$prevSampleLines = 0
$totalSec = $Minutes * 60

while ($true) {
    Start-Sleep -Seconds $SampleSec
    $elapsed = ((Get-Date) - $startTime).TotalSeconds
    if ($elapsed -gt $totalSec -or $app.HasExited) { break }

    $simRef = Get-Process -Id $sim.Id -ErrorAction SilentlyContinue
    $appRef = Get-Process -Id $app.Id -ErrorAction SilentlyContinue
    if (-not $simRef -or -not $appRef) { break }

    $simMemKB = [math]::Round($simRef.WorkingSet64 / 1KB)
    $appMemKB = [math]::Round($appRef.WorkingSet64 / 1KB)
    $cpuNow = $appRef.TotalProcessorTime.TotalSeconds
    $appCpuPct = if ($SampleSec -gt 0) { [math]::Round(($cpuNow - $prevCpu) / $SampleSec * 100.0, 1) } else { 0 }
    $prevCpu = $cpuNow
    $appHandles = $appRef.HandleCount
    # NOTE: the redirect file is created immediately but stays EMPTY for most of the
    # run - a child's stdout is a FILE handle (not a console), so stdio is FULLY
    # buffered and only flushes on exit or every 4KB. Get-Content -Raw on a 0-byte
    # file returns $null, and [regex]::Matches($null, ...) throws ArgumentNullException
    # which killed the whole soak under $ErrorActionPreference='Stop' (verified 2026-09-04).
    # sampleLines is therefore informational only; use dbSizeKB growth as the live
    # ingest metric (see analysis below).
    $sampleLines = 0
    if (Test-Path $appLog) {
        $appTxt = Get-Content $appLog -Raw -ErrorAction SilentlyContinue
        if ($appTxt) { $sampleLines = ([regex]::Matches($appTxt, "\[ENS\] pt=")).Count }
    }
    $dbSizeKB = 0
    $dbFile = Get-ChildItem $dbOut -Recurse -Filter "data_*.db" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dbFile) { $dbSizeKB = [math]::Round($dbFile.Length / 1KB) }

    "$([math]::Round($elapsed)),$simMemKB,$appMemKB,$appCpuPct,$appHandles,$sampleLines,$dbSizeKB" | Out-File -Encoding ascii $csv -Append
    Write-Host ("  t={0}s appMem={1}MB cpu={2}% handles={3} samples={4} db={5}KB" -f `
        [math]::Round($elapsed), [math]::Round($appMemKB/1024), $appCpuPct, $appHandles, $sampleLines, $dbSizeKB)
}

# ---- wait both to finish ----
Wait-Proc $sim ($simSec + 60) "simulator" | Out-Null
Wait-Proc $app 60 "ens_app"                | Out-Null

# ---- analysis ----
$fail = @()
if (-not $app.HasExited) { $fail += "app still running after timeout"; Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
if (-not $sim.HasExited) { Stop-Process -Id $sim.Id -Force -ErrorAction SilentlyContinue }

if (Test-Path $csv) {
    $rows = Import-Csv $csv
    if ($rows.Count -ge 6) {
        $n = $rows.Count
        # Window = 20% of rows but at least 2: with 10% of 11 rows the window is a
        # single row, and since row 1 legitimately has sampleLines=0 (stdio buffer not
        # yet flushed) the computed firstRate was 0 -> "lastRate < 0 * 0.6" could NEVER
        # be true, i.e. the decay assertion was unfailable (verified 2026-09-04).
        $win = [math]::Max(2, [int]($n * 0.20))
        $avgFirstMem = ($rows | Select-Object -First $win | Measure-Object -Property appMemKB -Average).Average
        $avgLastMem  = ($rows | Select-Object -Last $win  | Measure-Object -Property appMemKB -Average).Average
        $memGrowthMB = [math]::Round(($avgLastMem - $avgFirstMem) / 1024.0, 1)
        Write-Host "  mem growth (last$win rows - first$win rows): ${memGrowthMB} MB"
        if ($memGrowthMB -gt 20) { $fail += "memory growth ${memGrowthMB}MB > 20MB (possible leak)" }

        $sampleCol = @($rows | ForEach-Object { [int64]$_.sampleLines })
        $totalSamples = $sampleCol[$n - 1]
        $firstRate = $sampleCol[$win - 1]      - $sampleCol[0]
        $lastRate  = $sampleCol[$n - 1]        - $sampleCol[$n - $win - 1]
        Write-Host "  sampleLines total=$totalSamples  first$win rows:+$firstRate  last$win rows:+$lastRate"
        if ($totalSamples -le 0) {
            $fail += "no samples ingested at all (sampleLines stayed 0) - check app/point table/link"
        } elseif ($firstRate -le 0) {
            $fail += "no ingestion in the first window (+$firstRate) - app did not start collecting"
        } elseif ($lastRate -lt ($firstRate * 0.60)) {
            $fail += "sample rate decayed (last +$lastRate < 60% of first +$firstRate)"
        }
    } else {
        $fail += "insufficient samples in CSV ($($rows.Count))"
    }
} else {
    $fail += "no CSV produced"
}

Write-Host "---"
if ($fail.Count -eq 0) {
    Write-Host "== PASS: soak test stable (see $csv) =="
    exit 0
} else {
    Write-Host "== FAIL =="
    $fail | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
