# Runs one scenario pair (autotest\<Scenario>_host.txt / _client.txt) hidden, and writes SHORT.txt.
#   powershell -NoProfile -ExecutionPolicy Bypass -File run_quick.ps1 -Scenario rads_both
param([Parameter(Mandatory=$true)][string]$Scenario, [int]$TimeoutSec = 400)
$dev = 'C:\Users\User\Desktop\FalloutCoop_Dev'
$name = $Scenario + "_" + (Get-Date -Format 'MMdd_HHmm')
$suite = Get-Content "$dev\run_suite3s.ps1" -Raw
$suite = $suite.Replace('Name = "explore_short"; Host = "explore_short_host.txt"; Client = "explore_short_client.txt"', "Name = `"$Scenario`"; Host = `"${Scenario}_host.txt`"; Client = `"${Scenario}_client.txt`"")
Set-Content "$dev\run_suite_quick_tmp.ps1" $suite -Encoding utf8
& "$dev\run_suite_quick_tmp.ps1" -Name $name -TimeoutSec $TimeoutSec
$out = "$dev\autotest_results\$name"
$lines = Select-String -Path "$out\REPORT.txt" -Pattern 'exit code|SUMMARY|^    FAIL|^CRASH|^HANG|NEW DUMP' | ForEach-Object { $_.Line.Trim() }
$lines | Select-Object -First 25 | Set-Content "$out\SHORT.txt" -Encoding utf8
Get-ChildItem "$out\*\*\coopnet_debug.log" | ForEach-Object { Select-String -Path $_.FullName -Pattern 'Coop-effects: (carried|radiation)|put back on|companion died|game over|NOTE party member|EXPLORE done|Coop-desync: pid|DUMP companion|data was missing|Autotest: NOTE|Autotest: FAIL|Your friend (dis)?connected|Lost connection|joined during a fight|missed the start|companion combat turn started|sent COMBAT_ACTION|remote screen began|remote screen ended|barter|driver=|Coop-talk|never opened|one talking|cannot reach|requested dialogue|walks to talk|Coop-remote|Coop-barter|sent dialogue command|Coop-dlg|Coop-hit|Coop-perk|Coop-trait|put the client|QUEST-STATE|received XP|GVAR' | Select-Object -First 90 | ForEach-Object { "  " + $_.Line.Trim() } } | Add-Content "$out\SHORT.txt" -Encoding utf8
Write-Host ""; Get-Content "$out\SHORT.txt"
Write-Host ""; Write-Host "Tell Claude: $Scenario done"
