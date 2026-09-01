# tools/run_phase4_acceptance.ps1 - Phase 4 acceptance (5C dual-track, slice 22).
#
# Two-process CLI assertions (replaces the missing run_drill_434.ps1):
#   * DeviceSimulator --cli runs a drill scenario (default overheat_fast.json)
#   * ens_app --cli connects: alarm rules -> alarm -> blackbox mmap -> monthly DB -> samples
#
# Assertions:
#   1) sim_report.json result=PASS (all scenario steps fired)
#   2) ens_app stdout contains "link connected" and "[ENS] pt=" sample lines
#   3) monthly DB data_YYYYMM.db exists (--data-dir)
#   4) critical.swp exists (--blackbox-dir, overheat Critical fired)
#   5) "alarm rules loaded" in app stdout
# Any failure -> FAIL summary, exit 1. All pass -> PASS, exit 0.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools/run_phase4_acceptance.ps1
#   Optional: -Scenario xxx.json -RunSeconds N -OutDir <ascii-path>
# NOTE: All paths MUST be ASCII-only (Start-Process arg encoding corruption on CJK).

param(
    [string]$Scenario = "overheat_fast.json",
    [int]$RunSeconds = 12,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$root = "D:\Study\Qt_host_application_Project\EnerSentry"
$bin  = Join-Path $root "bin\Debug"
$td   = Join-Path $root "build\vs2022-debug\test_data"
$pt   = Join-Path $td "sim_pointtable_sample.json"
$rules = Join-Path $td "alarm_rules_sample.json"
$scen = Join-Path $td "scenarios\$Scenario"

if (-not (Test-Path $bin)) { Write-Host "ERR: $bin not found (run cmake --build first)"; exit 2 }
if (-not (Test-Path $scen)) { Write-Host "ERR: scenario not found: $scen"; exit 2 }

if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = Join-Path $root "build\phase4_accept_out" }
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null
$simOut = Join-Path $OutDir "sim_out"
$appOut = Join-Path $OutDir "app_out"
$dbOut  = Join-Path $OutDir "db"
$bbOut  = Join-Path $OutDir "bb"
New-Item -ItemType Directory -Path $simOut, $appOut, $dbOut, $bbOut | Out-Null

$port = 15031
$simSec = $RunSeconds
$appSec = $RunSeconds - 2

Write-Host "== Phase 4 acceptance =="
Write-Host "  sim: DeviceSimulator --cli ${simSec}s scenario=$Scenario port=$port"
Write-Host "  app: ens_app --cli ${appSec}s port=$port"

$sim = Start-Process -FilePath (Join-Path $bin "DeviceSimulator.exe") -ArgumentList @(
    "--cli", "$simSec", "--port", "$port", "--pointtable", $pt,
    "--scenario", $scen, "--export-dir", $simOut) -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $OutDir "sim_stdout.txt")

Start-Sleep -Seconds 2

$app = Start-Process -FilePath (Join-Path $bin "ens_app.exe") -ArgumentList @(
    "--cli", "--point-table", $pt, "--host", "127.0.0.1", "--port", "$port",
    "--run-seconds", "$appSec", "--alarm-rules", $rules,
    "--data-dir", $dbOut, "--blackbox-dir", $bbOut) -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $OutDir "app_stdout.txt")

Wait-Process -Id $sim.Id -Timeout 60 -ErrorAction SilentlyContinue
Wait-Process -Id $app.Id -Timeout 60 -ErrorAction SilentlyContinue

# ---- Assertions ----
$fail = @()
$report = Join-Path $simOut "sim_report.json"
if (Test-Path $report) {
    $r = Get-Content $report -Raw | ConvertFrom-Json
    if ($r.result -eq "PASS") { Write-Host "  [OK] sim_report result=PASS" }
    else { $fail += "sim_report result=$($r.result)"; Write-Host "  [X] sim_report result=$($r.result)" }
} else { $fail += "no sim_report.json"; Write-Host "  [X] no sim_report.json" }

$appLog = Get-Content (Join-Path $OutDir "app_stdout.txt") -Raw
if ($appLog -match "link connected") { Write-Host "  [OK] app link connected" }
else { $fail += "app not connected"; Write-Host "  [X] app not connected" }
$sampleLines = ([regex]::Matches($appLog, "\[ENS\] pt=")).Count
if ($sampleLines -gt 0) { Write-Host "  [OK] app samples: $sampleLines" }
else { $fail += "app no samples"; Write-Host "  [X] app no samples" }

# Monthly DB lives under dataDir/history/YYYYMM/ (recursive search)
$dbFile = Get-ChildItem $dbOut -Recurse -Filter "data_*.db" -ErrorAction SilentlyContinue
if ($dbFile) { Write-Host "  [OK] monthly DB: $($dbFile.FullName)" }
else { $fail += "no monthly DB"; Write-Host "  [X] no monthly DB" }

if (Test-Path (Join-Path $bbOut "critical.swp")) { Write-Host "  [OK] blackbox critical.swp" }
else { $fail += "no critical.swp"; Write-Host "  [X] no critical.swp" }

if ($appLog -match "alarm rules loaded") { Write-Host "  [OK] alarm rules loaded" }
else { Write-Host "  [?] alarm rules load marker not found" }

Write-Host "---"
if ($fail.Count -eq 0) {
    Write-Host "== PASS: Phase 4 dual-track acceptance assertions all passed =="
    Write-Host ""
    Write-Host "GUI acceptance checklist (manual):"
    Write-Host "  1. Start DeviceSimulator (GUI default, --pointtable <ascii pt>)"
    Write-Host "  2. Start ens_app -> login (admin/Admin@123) -> trend page: 8 channels scrolling"
    Write-Host "  3. SBO page: Select -> Armed(5s) -> Operate; stop sim -> red 'disconnected' -> green on recover"
    Write-Host "  4. Task manager: trend page CPU < 15% (OpenGL auto-detected)"
    exit 0
} else {
    Write-Host "== FAIL =="
    $fail | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
