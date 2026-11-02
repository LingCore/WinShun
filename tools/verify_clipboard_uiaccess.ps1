param(
    [string]$BuildDir = 'build/release',
    [string]$QtDir = 'C:/Qt/6.12.0/msvc2022_64',
    [string]$OutputDir = 'build/clipboard-uiaccess-validation',
    [switch]$Admin,
    [string]$ProtectedDir,
    [string]$ManifestHash
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $root $OutputDir }))
$stage = Join-Path $out 'stage'

function Assert-PrivateTarget([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ([IO.Path]::GetDirectoryName($full) -ne $env:ProgramFiles -or
        [IO.Path]::GetFileName($full) -notmatch '^WinShun-Clipboard-QA-[0-9a-f]{32}$') {
        throw "Not a private clipboard QA directory: $full"
    }
    return $full
}

if ($Admin) {
    $target = Assert-PrivateTarget $ProtectedDir
    $task = Split-Path $target -Leaf
    $manifest = Join-Path $out 'files.json'
    try {
        if ((Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -ne $ManifestHash) { throw 'Manifest changed' }
        if (Test-Path -LiteralPath $target) { throw 'QA destination already exists' }
        New-Item -ItemType Directory -Path $target | Out-Null
        $security = New-Object System.Security.AccessControl.DirectorySecurity
        $security.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)')
        Set-Acl -LiteralPath $target -AclObject $security
        Get-ChildItem -LiteralPath $stage | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $target -Recurse }
        & icacls.exe $target /setowner '*S-1-5-32-544' /T /Q | Out-Null
        if ($LASTEXITCODE) { throw 'Could not protect file ownership' }
        foreach ($file in (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json)) {
            $path = [IO.Path]::GetFullPath((Join-Path $target $file.path))
            if (!$path.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid manifest path' }
            if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.hash) { throw "Hash mismatch: $path" }
        }
        $action = New-ScheduledTaskAction -Execute (Join-Path $target 'WinShunService.exe') -Argument "--console --no-bulk --pipe $task"
        $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
        Register-ScheduledTask -TaskName $task -Action $action -Principal $principal -Settings $settings | Out-Null
        Start-ScheduledTask -TaskName $task
        Start-Sleep -Milliseconds 500
        $env:WINSHUN_TEST_PIPE = $task
        # QtTest consumes narrow argv; use an ASCII relative log name even
        # when the checkout path contains Chinese characters.
        $boundary = Start-Process (Join-Path $target 'tst_service.exe') -WorkingDirectory $target -WindowStyle Hidden -PassThru -Wait `
            -ArgumentList @('handshake', 'dialogNavRefusedForOthers', 'clipboardDisplayRefusedForOthers', 'keyListenerForAnyClient',
                '-o', 'service-boundary.txt,txt') -RedirectStandardError (Join-Path $out 'service-boundary-launch.txt')
        if (Test-Path -LiteralPath (Join-Path $target 'service-boundary.txt')) {
            Copy-Item -LiteralPath (Join-Path $target 'service-boundary.txt') -Destination $out
        }
        if ($boundary.ExitCode) { throw "Service boundary regression failed ($($boundary.ExitCode))" }
        Set-Content -LiteralPath (Join-Path $out 'ready.txt') -Value $task -Encoding ascii
        $deadline = (Get-Date).AddMinutes(4)
        while (!(Test-Path -LiteralPath (Join-Path $out 'stop.request')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    } catch {
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $out 'admin-error.txt') -Encoding utf8
    } finally {
        if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) {
            Stop-ScheduledTask -TaskName $task
            Unregister-ScheduledTask -TaskName $task -Confirm:$false
        }
        if (Test-Path -LiteralPath $target) {
            $target = Assert-PrivateTarget $target
            Start-Sleep -Milliseconds 500
            Remove-Item -LiteralPath $target -Recurse -Force
        }
        Set-Content -LiteralPath (Join-Path $out 'cleaned.txt') -Value 'private task and directory removed' -Encoding ascii
    }
    exit
}

if (Test-Path -LiteralPath $out) { throw 'Use a fresh OutputDir for this isolated verification' }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$binary = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }))
foreach ($name in @('WinShun.exe', 'WinShunService.exe', 'tst_service.exe')) { Copy-Item -LiteralPath (Join-Path $binary $name) -Destination $stage }
Copy-Item -LiteralPath "$QtDir/bin/Qt6Test.dll" -Destination $stage
& "$QtDir/bin/windeployqt.exe" --release --qmldir "$root/src/app/qml" --no-translations --no-system-d3d-compiler `
    --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime --skip-plugin-types qmltooling,generic,networkinformation,tls,iconengines `
    --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql "$stage/WinShun.exe" *> (Join-Path $out 'deploy.txt')
if ($LASTEXITCODE) { throw 'Qt deployment failed' }
foreach ($name in @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll')) {
    $runtime = Join-Path $binary $name
    if (Test-Path -LiteralPath $runtime) { Copy-Item -LiteralPath $runtime -Destination $stage }
}
$files = Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
    @{path = $_.FullName.Substring($stage.Length + 1); hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
}
$files | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'files.json') -Encoding utf8
$manifestHash = (Get-FileHash -LiteralPath (Join-Path $out 'files.json') -Algorithm SHA256).Hash
$protected = Join-Path $env:ProgramFiles ('WinShun-Clipboard-QA-' + [Guid]::NewGuid().ToString('N'))
$adminArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $PSCommandPath + '"'), '-Admin',
    '-OutputDir', ('"' + $out + '"'), '-ProtectedDir', ('"' + $protected + '"'), '-ManifestHash', $manifestHash)
Write-Output "Windows approval is needed to run the isolated UIAccess test. Candidate: $stage"
$elevated = Start-Process powershell.exe -ArgumentList $adminArgs -Verb RunAs -WindowStyle Hidden -PassThru
$oldPipe = $env:WINSHUN_PIPE
try {
    $deadline = (Get-Date).AddSeconds(60)
    while (!(Test-Path -LiteralPath (Join-Path $out 'ready.txt')) -and !$elevated.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    if (!(Test-Path -LiteralPath (Join-Path $out 'ready.txt'))) { throw 'Protected test server did not become ready; see admin-error.txt' }
    $env:WINSHUN_PIPE = Get-Content -LiteralPath (Join-Path $out 'ready.txt') -Raw
    $env:WINSHUN_PIPE = $env:WINSHUN_PIPE.Trim()
    Start-Sleep -Milliseconds 500
    $probeArgs = @('--probe-runtime', ('"' + (Join-Path $out 'native') + '"'), '--clipboard-shell-probe', 'native',
        '--clipboard-display-service-probe', '--renderer', 'd3d11', '--settle-ms', '100', '--scale', '1.5')
    $probe = Start-Process (Join-Path $protected 'WinShun.exe') -ArgumentList $probeArgs -WindowStyle Hidden -PassThru
    if (!$probe.WaitForExit(90000)) { $probe.Kill(); throw 'Owned QA process timed out' }
    $probe.Refresh()
    if ($probe.ExitCode) { throw "UIAccess runtime checks failed ($($probe.ExitCode)); see native/clipboard-shell.json" }
    Write-Output 'Native Start + UIAccess clipboard checks passed.'
} finally {
    $env:WINSHUN_PIPE = $oldPipe
    Set-Content -LiteralPath (Join-Path $out 'stop.request') -Value 'done' -Encoding ascii
    $null = $elevated.WaitForExit(10000)
}
