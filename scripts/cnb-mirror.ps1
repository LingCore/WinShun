<#
.SYNOPSIS
    Mirror a GitHub release of LingCore/WinShun to CNB (cnb.cool, Tencent),
    where people in mainland China download faster: the same tag on the
    same commit, the same files under the same names, at
    https://cnb.cool/lcstp/WinShun/-/releases/download/<tag>/<file>.

    lcstp.com lists that address first among each package's download
    mirrors, so the in-app update tries CNB before the GitHub proxies and
    GitHub itself (the app checks every file against GitHub's SHA-256, so a
    mirror can only be slow or missing, never wrong). Delta packages are
    found beside the full one (src/app/UpdateDelta.cpp), so they must sit in
    the same CNB release under GitHub's names, which this script keeps.

    Steps: push the tag (and GitHub's main) to CNB; create the release there
    or bring its name and notes up to date; upload every asset CNB lacks or
    holds at another size; then, without the token, download every file back
    from cnb.cool and compare its SHA-256 with GitHub's. Run it again at any
    time: what is already there is kept.

    The token (repo-code + repo-release read-write, lcstp/WinShun only) lives
    in Windows Credential Manager as git's credential for cnb.cool, user cnb
    (docs/releasing.md, the CNB section); CNB_TOKEN overrides it.

.EXAMPLE
    ./scripts/cnb-mirror.ps1                          # GitHub's latest release
    ./scripts/cnb-mirror.ps1 -Tag v0.41.0
    ./scripts/cnb-mirror.ps1 -Tag v0.41.0 -From dist  # its files are here already
#>
param(
    [string] $Tag, # default: GitHub's latest release
    [string] $From, # a folder that already holds the files (each checked against GitHub's SHA-256)
    [string] $Repo = 'lcstp/WinShun',
    [switch] $SkipDownloadCheck # only compare sizes (HEAD), do not download the files back
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$github = 'LingCore/WinShun'
$api = "https://api.cnb.cool/$Repo/-"
$site = "https://cnb.cool/$Repo"
# Never a credential dialog: missing credentials are an error here.
$env:GCM_INTERACTIVE = 'never'
$env:GIT_TERMINAL_PROMPT = '0'

function Get-CnbToken {
    if ($env:CNB_TOKEN) { return $env:CNB_TOKEN }
    $answer = "protocol=https`nhost=cnb.cool`nusername=cnb`n`n" | git credential fill 2>$null
    $line = @($answer | Where-Object { $_ -like 'password=*' })[0]
    if (-not $line) { throw 'No CNB token: store it as git''s credential for https://cnb.cool, user cnb (docs/releasing.md), or set CNB_TOKEN.' }
    $line.Substring(9)
}

function Get-HttpStatus($ErrorRecord) {
    $response = $ErrorRecord.Exception.Response
    if ($response) { [int] $response.StatusCode } else { 0 }
}

function Invoke-Cnb([string] $Method, [string] $Url, $Body) {
    if ($Url -notmatch '^https://') { $Url = 'https://api.cnb.cool/' + $Url.TrimStart('/') }
    $request = @{ Method = $Method; Uri = $Url; UseBasicParsing = $true
        Headers = @{ Authorization = "Bearer $token"; Accept = 'application/json' } }
    if ($null -ne $Body) {
        # Bytes, so the notes' Chinese arrives as UTF-8 in any PowerShell.
        $request.ContentType = 'application/json; charset=utf-8'
        $request.Body = [Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 5 -Compress))
    }
    Invoke-RestMethod @request
}

function Get-Sha256([string] $File) { (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant() }

# Upload one file: ask for a one-time upload address, PUT the bytes there,
# then confirm (only a confirmed upload belongs to the release). A failed
# try starts over with a new address.
function Send-Asset([string] $File, [string] $Name, [string] $ReleaseId) {
    $size = (Get-Item -LiteralPath $File).Length
    for ($try = 1; ; $try++) {
        try {
            $slot = Invoke-Cnb POST "$api/releases/$ReleaseId/asset-upload-url" @{ asset_name = $Name; size = $size; overwrite = $true; ttl = 0 }
            if (-not $slot.upload_url -or -not $slot.verify_url) { throw 'CNB gave no upload address.' }
            curl.exe -sS --fail -T $File -H 'Content-Type: application/octet-stream' $slot.upload_url | Out-Null
            if ($LASTEXITCODE) { throw "the upload failed (curl exit $LASTEXITCODE)" }
            Invoke-Cnb POST $slot.verify_url $null | Out-Null
            return
        } catch {
            if ($try -ge 3) { throw "${Name}: $($_.Exception.Message)" }
            Write-Warning "${Name}: $($_.Exception.Message); trying again"
        }
    }
}

# What anyone downloads, without the token: the size, or every byte's hash.
function Test-Public($Asset, [string] $Work) {
    $url = "$site/-/releases/download/$Tag/$([uri]::EscapeDataString($Asset.name))"
    if ($SkipDownloadCheck) {
        $headers = curl.exe -sSI --fail $url 2>$null
        if ($LASTEXITCODE) { return $false }
        $length = @($headers | Select-String '^content-length:\s*(\d+)' | ForEach-Object { [long] $_.Matches[0].Groups[1].Value })[-1]
        return $length -eq $Asset.size
    }
    $copy = Join-Path $Work ('public-' + $Asset.name)
    try {
        curl.exe -sSL --fail -o $copy $url 2>$null
        return -not $LASTEXITCODE -and (Get-Sha256 $copy) -eq $Asset.sha
    } finally {
        Remove-Item -LiteralPath $copy -Force -ErrorAction SilentlyContinue
    }
}

$token = Get-CnbToken

# The release as GitHub has it: the files, their SHA-256, the commit.
if (-not $Tag) {
    $Tag = gh release view --repo $github --json tagName --jq .tagName
    if ($LASTEXITCODE -or -not $Tag) { throw 'Could not read GitHub''s latest release.' }
}
$releaseJson = gh release view $Tag --repo $github --json tagName,name,body,isDraft,isPrerelease,assets
if ($LASTEXITCODE) { throw "No GitHub release $Tag." }
$release = $releaseJson | ConvertFrom-Json
if ($release.isDraft) { throw "$Tag is still a draft on GitHub." }
$assets = foreach ($a in $release.assets) {
    if ($a.digest -notmatch '^sha256:([0-9a-f]{64})$') { throw "GitHub gives no SHA-256 for $($a.name)." }
    [pscustomobject] @{ name = $a.name; size = [long] $a.size; sha = $Matches[1] }
}
$commit = gh api "repos/$github/commits/$Tag" --jq .sha
if ($LASTEXITCODE -or $commit -notmatch '^[0-9a-f]{40}$') { throw "Could not read the commit of $Tag." }
$latest = gh api "repos/$github/releases/latest" --jq .tag_name
if ($LASTEXITCODE) { throw 'Could not read GitHub''s latest release.' }

# CNB attaches a release to a tag of its own repository: push the tag, and
# GitHub's main so the code there stays current.
git -C $root fetch --quiet origin '+refs/heads/main:refs/remotes/origin/main' "+refs/tags/${Tag}:refs/tags/${Tag}"
if ($LASTEXITCODE) { throw "Could not fetch $Tag from GitHub." }
if ((git -C $root rev-parse "$Tag^{commit}") -ne $commit) { throw "The local $Tag is not GitHub's $commit." }
# No user name in the address: with one, CNB answers "Repository Not Found".
$remote = "https://cnb.cool/$Repo.git"
git -C $root push --quiet $remote "refs/tags/${Tag}:refs/tags/${Tag}"
if ($LASTEXITCODE) { throw "Pushing $Tag to CNB failed." }
git -C $root push --quiet $remote 'refs/remotes/origin/main:refs/heads/main'
if ($LASTEXITCODE) { Write-Warning 'Pushing main to CNB failed (the release does not need it).' }

# The notes without the hidden lines meant for the app (push, delta).
$notes = (($release.body -split '\r?\n') | Where-Object { $_ -notmatch '^\s*<!--\s*winshun-' }) -join "`n"
$notes = $notes.TrimEnd() + "`n`n---`n`nGitHub: https://github.com/$github/releases/tag/$Tag"
$fields = @{ name = $release.name; body = $notes; draft = $false; prerelease = [bool] $release.isPrerelease
    make_latest = $(if ($Tag -eq $latest) { 'true' } else { 'false' }) } # a string, not a boolean

function Get-CnbRelease {
    try { Invoke-Cnb GET "$api/releases/tags/$([uri]::EscapeDataString($Tag))" $null }
    catch { if ((Get-HttpStatus $_) -eq 404) { $null } else { throw } }
}
$cnb = Get-CnbRelease
if ($cnb) {
    Invoke-Cnb PATCH "$api/releases/$($cnb.id)" $fields | Out-Null
} else {
    Invoke-Cnb POST "$api/releases" ($fields + @{ tag_name = $Tag; target_commitish = $commit }) | Out-Null
    $cnb = Get-CnbRelease
    if (-not $cnb) { throw "CNB did not create the release $Tag." }
}

$work = Join-Path $root "build\cnb-mirror\$Tag"
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    # A local copy of one asset, checked against GitHub's SHA-256.
    function Get-LocalAsset($Asset) {
        if ($From) {
            $file = Join-Path $From $Asset.name
            if ((Test-Path -LiteralPath $file) -and (Get-Sha256 $file) -eq $Asset.sha) { return $file }
        }
        $file = Join-Path $work $Asset.name
        if (-not ((Test-Path -LiteralPath $file) -and (Get-Sha256 $file) -eq $Asset.sha)) {
            gh release download $Tag --repo $github --pattern $Asset.name --dir $work --clobber
            if ($LASTEXITCODE -or (Get-Sha256 $file) -ne $Asset.sha) { throw "$($Asset.name) from GitHub does not match its SHA-256." }
        }
        $file
    }

    foreach ($asset in $assets) {
        $there = @($cnb.assets | Where-Object { $_.name -eq $asset.name })
        if ($there.Count -eq 1 -and [long] $there[0].size -eq $asset.size) { continue }
        Write-Host "Uploading $($asset.name) ($([math]::Round($asset.size / 1MB, 1)) MB)"
        Send-Asset (Get-LocalAsset $asset) $asset.name $cnb.id
    }

    # Everything again as a stranger would get it; a file that differs is
    # uploaded once more (CNB holding an older upload under the same name).
    $failed = @()
    foreach ($asset in $assets) {
        if (Test-Public $asset $work) { Write-Host "ok  $($asset.name)"; continue }
        Write-Host "Uploading $($asset.name) again: the public copy differs"
        Send-Asset (Get-LocalAsset $asset) $asset.name $cnb.id
        if (Test-Public $asset $work) { Write-Host "ok  $($asset.name)" } else { $failed += $asset.name }
    }
    if ($failed) { throw "Not right on CNB after a second upload: $($failed -join ', ')" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    $parent = Split-Path $work
    if ((Test-Path -LiteralPath $parent) -and -not (Get-ChildItem -LiteralPath $parent -Force)) { Remove-Item -LiteralPath $parent -Force }
}
Write-Host "CNB mirror of ${Tag}: $($assets.Count) files at $site/-/releases/tag/$Tag"
