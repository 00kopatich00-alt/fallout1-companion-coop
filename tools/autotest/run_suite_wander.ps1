# Runs every scripted scenario one after another in the background (hidden windows, no sound) and
# writes ONE short report: autotest_results\<name>\REPORT.txt
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\User\Desktop\FalloutCoop_Dev\run_suite.ps1
#
# Running it costs nothing. When it is done, tell Claude "report saved": reading REPORT.txt is cheap,
# and it holds everything needed (what failed, the interesting log lines, crash/hang files).
param(
    [string]$Name = (Get-Date -Format 'yyyyMMdd_HHmmss'),
    [int]$TimeoutSec = 300,
    [switch]$Show,
    [switch]$All,   # also run the scenarios that already passed before
    [string]$Only = ''   # run just this scenario, e.g. -Only rads_poison
)

$dev = 'C:\Users\User\Desktop\FalloutCoop_Dev'
$suiteOut = "$dev\autotest_results\$Name"
New-Item -ItemType Directory -Force $suiteOut | Out-Null
$report = "$suiteOut\REPORT.txt"

# name, host script, client script (empty client = one game only)
# What a plain run does: the scenarios that have not passed yet (or are new).
$scenarios = @(
    @{ Name = "wander"; Host = "wander_host.txt"; Client = "wander_client.txt" }
)
# Already confirmed in earlier runs; -All adds them back (after big changes, to catch regressions).
if ($All) {
    $scenarios = @(
        @{ Name = 'solo_smoke';      Host = 'solo_smoke.txt';           Client = '' },
        @{ Name = 'pair_connect';    Host = 'pair_connect_host.txt';    Client = 'pair_connect_client.txt' },
        @{ Name = 'perks_fight';     Host = 'perks_fight_host.txt';     Client = 'perks_fight_client.txt' },
        @{ Name = 'cutscene';        Host = 'cutscene_host.txt';        Client = 'cutscene_client.txt' },
        @{ Name = 'weapons';         Host = 'weapons_host.txt';         Client = 'weapons_client.txt' },
        @{ Name = 'death_companion'; Host = 'death_companion_host.txt'; Client = 'death_companion_client.txt' },
        @{ Name = 'death_host';      Host = 'death_host_host.txt';      Client = 'death_host_client.txt' },
        @{ Name = "rads_poison"; Host = "rads_poison_host.txt"; Client = "rads_poison_client.txt" },
        @{ Name = "gestures";    Host = "gestures_host.txt";    Client = "gestures_client.txt" },
        @{ Name = "dialogue";    Host = "dialogue_host.txt";    Client = "dialogue_client.txt" }
    ) + $scenarios
}
if ($Only -ne '') { $scenarios = @($scenarios | Where-Object { $_.Name -eq $Only }) }

# Lines worth reading from a game's debug log (problems, not the normal chatter).
$interesting = '^CRASH|^HANG|EXPLORE|FAILED|glitch|Coop-run:|Coop-ui:|Coop-perk:|Autotest: FAIL|Lost connection|disconnected|Refused a friend|Coop-hit:|stepped on an exit grid|worldmap began|runaway sprite|companion died'

"Autotest suite $Name  ($(Get-Date))" | Set-Content $report -Encoding utf8
"Build: $((Get-Item "$dev\fallout1-coop\build\RelWithDebInfo\fallout-ce.exe").LastWriteTime)" | Add-Content $report -Encoding utf8
"" | Add-Content $report -Encoding utf8

$totalFail = 0
foreach ($s in $scenarios) {
    $sub = "$Name\$($s.Name)"
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$dev\run_autotest.ps1",
        '-HostScript', "$dev\autotest\$($s.Host)", '-Name', $sub, '-TimeoutSec', $TimeoutSec)
    if ($s.Client -ne '') { $args += @('-ClientScript', "$dev\autotest\$($s.Client)") }
    if ($Show) { $args += '-Show' }
    Write-Host "running $($s.Name) ..."
    & powershell @args | Out-Null

    $out = "$dev\autotest_results\$sub"
    "================ $($s.Name) ================" | Add-Content $report -Encoding utf8
    foreach ($side in 'host', 'client') {
        $r = "$out\$side\result.txt"
        if (-not (Test-Path $r)) {
            if ($side -eq 'host' -or $s.Client -ne '') { "[$side] NO result.txt: the game never started the script" | Add-Content $report -Encoding utf8; $totalFail++ }
            continue
        }
        if (Test-Path "$out\xitcode.txt") { "[$side] " + (Get-Content "$out\xitcode.txt") | Add-Content $report -Encoding utf8 }
        if (Test-Path "$out\$side\exitcode.txt") { "[$side] " + (Get-Content "$out\$side\exitcode.txt") | Add-Content $report -Encoding utf8 }
        $lines = Get-Content $r
        $summary = $lines | Where-Object { $_ -match '^SUMMARY' } | Select-Object -Last 1
        if (-not $summary) { $summary = 'SUMMARY missing: the game was killed or crashed before the script ended'; $totalFail++ }
        "[$side] $summary" | Add-Content $report -Encoding utf8
        $lines | Where-Object { $_ -match '^FAIL|^DUMP' } | ForEach-Object { "    $_" } | Add-Content $report -Encoding utf8
        if ($summary -match 'fail=([1-9])') { $totalFail++ }

        # keep the game's own logs next to the results, and quote the interesting lines
        $gameDir = if ($side -eq 'host') { "$dev\game_host" } else { "$dev\game_client" }
        foreach ($f in 'coopnet_debug.log', 'startup.log', 'coop_connection.log') {
            if (Test-Path "$gameDir\$f") { Copy-Item "$gameDir\$f" "$out\$side\$f" -Force }
        }
        foreach ($d in Get-ChildItem "$gameDir\crash_*.dmp", "$gameDir\hang_*.dmp" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-30) }) {
            Copy-Item $d.FullName "$out\$side\" -Force
            "    NEW DUMP: $($d.Name)" | Add-Content $report -Encoding utf8
        }
        if (Test-Path "$out\$side\coopnet_debug.log") {
            $hits = Select-String -Path "$out\$side\coopnet_debug.log" -Pattern $interesting -ErrorAction SilentlyContinue | Select-Object -First 70
            foreach ($h in $hits) { "    log: $($h.Line.Trim())" | Add-Content $report -Encoding utf8 }
            $visual = (Select-String -Path "$out\$side\coopnet_debug.log" -Pattern "Coop-visual:" -ErrorAction SilentlyContinue | Measure-Object).Count
            if ($visual -gt 0) { "    Coop-visual resets: $visual (idle characters left with a pixel offset)" | Add-Content $report -Encoding utf8 }
        }
    }
    "" | Add-Content $report -Encoding utf8
}

"RESULT: $(if ($totalFail -eq 0) { 'ALL SCENARIOS PASSED' } else { "$totalFail problem(s), see above" })" | Add-Content $report -Encoding utf8
Write-Host ""
Write-Host "Done. Report: $report"
Write-Host "Tell Claude: report saved"
