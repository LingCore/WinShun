# What a file manager does to a share while something reads it in the background:
# opens one folder at a time and lists it, a few times a second. Reports how long
# each took. With -Threads, starts wsbench reading the whole share that many
# folders at a time (-BenchArgs '--tried': as many as pay, as with someone at the
# computer) and probes until it is done.
param(
    [Parameter(Mandatory)][string]$Share,      # \\host\share, with the tree of setup.sh
    [int]$Threads = 0,                          # 0: no background pass
    [int]$BufferKB = 64,
    [int]$Seconds = 20,                         # without a pass: how long to probe
    [int]$EveryMs = 200,
    [string]$Label = 'run',
    [string]$Distro = 'wsnas',
    [switch]$Cold,                              # drop the server's caches first
    [string[]]$BenchArgs = @(),
    [string]$Bench = (Join-Path $PSScriptRoot '..\..\..\..\build\release\wsbench.exe'),
    [string]$OutDir = (Join-Path ([IO.Path]::GetTempPath()) 'ws-share-load'),
    [string]$QtBin = 'C:\Qt\6.12.0\msvc2022_64\bin'
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$env:WSL_UTF8 = 1
$env:PATH = "$QtBin;$env:PATH"
New-Item -ItemType Directory -Force $OutDir | Out-Null
if ($Cold) { wsl -d $Distro -e sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' }

$pass = $null
$passOut = Join-Path $OutDir "bench-$Label.txt"
if ($Threads -gt 0) {
    $arguments = @('--share', $Share, '--threads', $Threads, '--rounds', 1, '--buffer', $BufferKB) + $BenchArgs
    $pass = Start-Process -FilePath $Bench -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $passOut
}
$random = [Random]::new(20261126)
$times = [Collections.Generic.List[double]]::new()
$rows = [Collections.Generic.List[object]]::new()
$watch = [Diagnostics.Stopwatch]::StartNew()
$one = [Diagnostics.Stopwatch]::new()
while ($true) {
    if ($pass) { if ($pass.HasExited) { break } } elseif ($watch.Elapsed.TotalSeconds -ge $Seconds) { break }
    $dir = '{0}\dept{1:00}\project{2:00}' -f $Share, $random.Next(1, 21), $random.Next(1, 61)
    $one.Restart()
    $n = [IO.Directory]::GetFileSystemEntries($dir).Length
    $one.Stop()
    if ($n -ne 100) { throw "listed $n entries of $dir" }
    $times.Add($one.Elapsed.TotalMilliseconds)
    $rows.Add([pscustomobject]@{ at = [int]$watch.Elapsed.TotalMilliseconds; ms = [int]$one.Elapsed.TotalMilliseconds })
    Start-Sleep -Milliseconds $EveryMs
}
$took = $watch.Elapsed.TotalSeconds
$sorted = $times.ToArray(); [Array]::Sort($sorted)
function At([double]$q) { if ($sorted.Length -eq 0) { return 0 } $sorted[[Math]::Min($sorted.Length - 1, [int][Math]::Floor($q * $sorted.Length))] }
$passLine = if ($pass) { (Get-Content $passOut | Select-String '^round 1').Line } else { '' }
'{0,-22} threads {1,2}  buffer {2,2} KB  pass {3,6:N1} s  probes {4,4}  median {5,7:N1}  p90 {6,7:N1}  p99 {7,8:N1}  max {8,8:N1} ms  >500ms {9}  >2s {10}' -f `
    $Label, $Threads, $BufferKB, $took, $sorted.Length, (At 0.5), (At 0.9), (At 0.99), $sorted[-1],
    @($sorted | Where-Object { $_ -gt 500 }).Count, @($sorted | Where-Object { $_ -gt 2000 }).Count
if ($passLine) { "    $passLine" }
$rows | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $OutDir "probe-$Label.csv")
