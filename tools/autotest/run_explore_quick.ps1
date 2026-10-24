# 8 minutes of the host travelling the wasteland on its own (hidden windows, the client follows and fights).
# At the end it writes SHORT.txt (a few lines). Tell Claude "explore done" - it only reads SHORT.txt.
$dev = 'C:\Users\User\Desktop\FalloutCoop_Dev'
$name = "explore_" + (Get-Date -Format 'MMdd_HHmm')
& "$dev\run_suite3s.ps1" -Name $name -TimeoutSec 700
$out = "$dev\autotest_results\$name"
$lines = Select-String -Path "$out\REPORT.txt" -Pattern 'exit code|SUMMARY|EXPLORE done|EXPLORE combat|CRASH|HANG|NEW DUMP' | ForEach-Object { $_.Line.Trim() }
$lines | Select-Object -First 25 | Set-Content "$out\SHORT.txt" -Encoding utf8
foreach ($side in 'host','client') {
    $res = Get-ChildItem "$out\*\$side\result.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($res) {
        $done = Select-String -Path $res.FullName -Pattern 'EXPLORE done: (\d+) maps visited \((.*)\), (\d+) combats' | Select-Object -First 1
        if ($done) {
            $maps = ($done.Matches[0].Groups[2].Value -split ', ' | Sort-Object -Unique) -join ', '
            "[$side] maps: $($done.Matches[0].Groups[1].Value) loads, distinct: $maps; fights: $($done.Matches[0].Groups[3].Value)" | Add-Content "$out\SHORT.txt" -Encoding utf8
        }
    }
}
$clog = Get-ChildItem "$out\*\client\coopnet_debug.log" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($clog) {
    $diffLines = (Select-String -Path $clog.FullName -Pattern 'Coop-desync: pid=' | Measure-Object).Count
    $last = Select-String -Path $clog.FullName -Pattern 'Coop-desync: check' | Select-Object -Last 1
    $runaway = (Select-String -Path $clog.FullName -Pattern 'runaway sprite' | Measure-Object).Count
    $offset = (Select-String -Path $clog.FullName -Pattern 'Coop-visual: idle' | Measure-Object).Count
    "[client] drift: $diffLines differences seen; runaway sprites: $runaway; small offset resets: $offset" | Add-Content "$out\SHORT.txt" -Encoding utf8
    if ($last) { "[client] last check: $($last.Line.Trim())" | Add-Content "$out\SHORT.txt" -Encoding utf8 }
    Select-String -Path $clog.FullName -Pattern 'runaway sprite' | Select-Object -First 5 | ForEach-Object { "  " + $_.Line.Trim() } | Add-Content "$out\SHORT.txt" -Encoding utf8
}
Write-Host ""; Get-Content "$out\SHORT.txt"
Write-Host ""; Write-Host "Tell Claude: explore done"
