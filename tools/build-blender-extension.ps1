param(
    [string]$BlenderPath = 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$OutputDir) { $OutputDir = Join-Path $repo 'build/integrations' }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$runtime = Get-ChildItem -LiteralPath (Split-Path $BlenderPath) -Directory | Where-Object { $_.Name -match '^\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$runtime) { throw 'Blender bundled Python/runtime not found.' }
$python = Join-Path $runtime.FullName 'python/bin/python.exe'
$cli = Join-Path $runtime.FullName 'scripts/addons_core/bl_pkg/cli/blender_ext.py'
$source = Join-Path $repo 'integrations/blender/winshun_file_dialog'
& $python $cli validate $source
if ($LASTEXITCODE -ne 0) { throw 'Blender extension manifest validation failed.' }
& $python $cli build --source-dir $source --output-dir ([IO.Path]::GetFullPath($OutputDir))
if ($LASTEXITCODE -ne 0) { throw 'Blender extension build failed.' }
$manifest = Get-Content "$source/blender_manifest.toml" -Raw
$version = [regex]::Match($manifest, '(?m)^version\s*=\s*"([^"]+)"').Groups[1].Value
$package = Join-Path $OutputDir "winshun_file_dialog-$version.zip"
& $python $cli validate $package
if ($LASTEXITCODE -ne 0) { throw 'Built Blender extension validation failed.' }
Get-FileHash -LiteralPath $package -Algorithm SHA256
