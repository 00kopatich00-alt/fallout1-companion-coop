# Runs scripted self-tests: one game (-HostScript only) or a host and a client (-HostScript and -ClientScript).
# The game presses its own keys and clicks (see docs/AUTOTEST.md), so nothing needs focus and nobody has to touch the PCs.
# Results and screenshots go to autotest_results\<name>\host and \client; the summary is printed at the end.
param(
    [Parameter(Mandatory=$true)][string]$HostScript,
    [string]$ClientScript = '',
    [string]$Name = (Get-Date -Format 'yyyyMMdd_HHmmss'),
    [int]$TimeoutSec = 300,
    [switch]$NoCopy,
    [switch]$HostCdb,  # run the host under the debugger (cdb): the stack of a crash goes to host\cdb.txt
    [switch]$Show,  # show the game windows (the default is a background run: hidden windows, no sound)
    [string]$HostPos = '0,40',      # where the host window goes with -Show (x,y), for a side-by-side recording
    [string]$ClientPos = '980,40'   # and the client window
)
$dev = 'C:\Users\User\Desktop\FalloutCoop_Dev'
$exe = "$dev\fallout1-coop\build\RelWithDebInfo\fallout-ce.exe"
$out = "$dev\autotest_results\$Name"
New-Item -ItemType Directory -Force "$out\host" | Out-Null
if ($ClientScript -ne '') { New-Item -ItemType Directory -Force "$out\client" | Out-Null }

# Only this test's own windows: stop earlier ones from the game folders, nothing else.
foreach ($p in Get-Process fallout-ce -ErrorAction SilentlyContinue) {
    if ($p.Path -like "$dev\game_host\*" -or $p.Path -like "$dev\game_client\*") { Stop-Process -Id $p.Id -Force }
}
Start-Sleep 1
if (-not $NoCopy) {
    Copy-Item $exe "$dev\game_host\fallout-ce.exe" -Force
    Copy-Item $exe "$dev\game_client\fallout-ce.exe" -Force
}
$hiddenArg = if ($Show) { @() } else { @("--autotest-hidden") }
if (-not $Show) { $env:SDL_AUDIODRIVER = "dummy" }   # no sound while you work
$procs = @()
$hostScriptPath = (Resolve-Path $HostScript).Path
$posArgHost = if ($Show) { @("--autotest-pos=$HostPos") } else { @() }
$posArgClient = if ($Show) { @("--autotest-pos=$ClientPos") } else { @() }
$hostArgs = @("--coop-debug", "--coop-name=HOST", "--autotest=`"$hostScriptPath`"", "--autotest-out=`"$out\host`"", "--autotest-timeout=$($TimeoutSec * 1000)") + $hiddenArg + $posArgHost
if ($HostCdb) {
    $env:_NT_SYMBOL_PATH = "$dev\fallout1-coop\build\RelWithDebInfo"
    $cdb = "$env:LOCALAPPDATA\Microsoft\WindowsApps\cdbX64.exe"
    $cdbArgs = @("-g", "-G", "-logo", "`"$out\host\cdb.txt`"", "-cf", "`"$dev\cdb_cmds.txt`"", "`"$dev\game_host\fallout-ce.exe`"") + $hostArgs
    $procs += Start-Process -FilePath $cdb -WorkingDirectory "$dev\game_host" -PassThru -ArgumentList $cdbArgs
} else {
    $procs += Start-Process -FilePath "$dev\game_host\fallout-ce.exe" -WorkingDirectory "$dev\game_host" -PassThru -ArgumentList $hostArgs
}
if ($ClientScript -ne '') {
    $clientScriptPath = (Resolve-Path $ClientScript).Path
    $clientArgs = @("--coop-debug", "--coop-name=CLIENT", "--autotest=`"$clientScriptPath`"", "--autotest-out=`"$out\client`"", "--autotest-timeout=$($TimeoutSec * 1000)") + $hiddenArg + $posArgClient
    $procs += Start-Process -FilePath "$dev\game_client\fallout-ce.exe" -WorkingDirectory "$dev\game_client" -PassThru -ArgumentList $clientArgs
}
Start-Sleep 6
if (-not $Show) { foreach ($p in $procs) { try { $p.PriorityClass = "BelowNormal" } catch { } } }   # keep the computer responsive (a recording needs full speed)
$deadline = (Get-Date).AddSeconds($TimeoutSec + 30)
while ((Get-Date) -lt $deadline -and ($procs | Where-Object { -not $_.HasExited })) { Start-Sleep 2 }
$sides = @('host', 'client')
$sideNames = @('host', 'client')
for ($i = 0; $i -lt $procs.Count; $i++) {
    if ($procs[$i].HasExited) {
        ("{0} exit code: {1} (0x{1:X})" -f $sideNames[$i], $procs[$i].ExitCode) | Set-Content "$out\$($sideNames[$i])\exitcode.txt"
    }
}
foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Write-Host "killed a game that did not finish (pid $($p.Id))" } }

foreach ($side in 'host', 'client') {
    $r = "$out\$side\result.txt"
    if (Test-Path $r) {
        Write-Host "=== $side ==="
        Get-Content $r
        Write-Host "screenshots: $((Get-ChildItem "$out\$side\*.bmp" -ErrorAction SilentlyContinue).Count) in $out\$side"
    } elseif ($side -eq 'host' -or $ClientScript -ne '') {
        Write-Host "=== ${side}: no result.txt (the game did not start the script) ==="
    }
}

# BMP -> PNG so the screenshots open anywhere (and in the chat).
Add-Type -AssemblyName System.Drawing
foreach ($bmp in Get-ChildItem "$out\*\*.bmp" -ErrorAction SilentlyContinue) {
    try {
        $img = [System.Drawing.Image]::FromFile($bmp.FullName)
        $img.Save([IO.Path]::ChangeExtension($bmp.FullName, 'png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item $bmp.FullName
    } catch { }
}
