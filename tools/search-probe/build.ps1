# 构建 Win顺 搜索诊断工具：dist/search-probe/WinShun搜索诊断.exe
# 用系统自带的 .NET Framework 4 csc（C# 5），生成的 exe 在 Windows 10/11 上直接运行。
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$out = Join-Path $root 'dist\search-probe'
New-Item -ItemType Directory -Force $out | Out-Null
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$exe = Join-Path $out 'WinShun搜索诊断.exe'
& $csc /nologo /codepage:65001 /optimize+ /platform:x64 /target:exe `
    /reference:System.Web.Extensions.dll /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll `
    "/out:$exe" (Join-Path $PSScriptRoot 'SearchProbe.cs')
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Write-Host "Built $exe"
