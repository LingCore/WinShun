# WinShun.psm1 — the Win顺 (WinShun) plugin SDK for PowerShell (Windows
# PowerShell 5.1 and PowerShell 7). Copy this file next to your main.ps1 and
# `Import-Module "$PSScriptRoot\WinShun.psm1"`. Keep both files as UTF-8
# with a BOM: Windows PowerShell 5.1 reads a bare .ps1 in the ANSI code page.
#
# A resident plugin (runtime.mode = "resident") is one call:
#
#     Import-Module "$PSScriptRoot\WinShun.psm1"
#     Start-WinShunPlugin -Name 'hello' -OnQuery {
#         param($q, $plugin)
#         @(New-WinShunItem -Id 'hi' -Title "你好，$($q.text)" -Copy $q.text)
#     }
#
# A once plugin (a process per query) prints its answer and ends:
#
#     $q = Get-WinShunQuery
#     Send-WinShunReply @(New-WinShunItem -Id 'hi' -Title "你好，$($q.text)")
#
# Protocol: docs/plugins.md. Only JSON-RPC may go to stdout (Write-Host and
# Write-Output are not used here on purpose); Write-WinShunDebug goes to
# stderr, which is plugins.log and the developer mode's pane.

[Console]::OutputEncoding = [Text.Encoding]::UTF8
[Console]::InputEncoding = [Text.Encoding]::UTF8
$script:OutputEncoding = [Text.Encoding]::UTF8
$script:In = [Console]::In
$script:Out = [Console]::Out
$script:NextId = 1
$script:Queued = New-Object System.Collections.Generic.List[string]
$script:Settings = @{}
$script:Locale = 'zh'

function Write-WinShunDebug {
    <# A line to stderr: plugins.log, and the developer mode's log pane. #>
    param([Parameter(ValueFromPipeline = $true)] $Text)
    [Console]::Error.WriteLine([string]$Text)
}

function ConvertTo-WinShunJson($Value) {
    ConvertTo-Json -InputObject $Value -Compress -Depth 12
}

function Send-WinShunLine($Message) {
    $script:Out.WriteLine((ConvertTo-WinShunJson $Message))
    $script:Out.Flush()
}

# ---- items and effects ---------------------------------------------------------

function New-WinShunItem {
    <#
    One result row (docs/plugins.md, "item"). -Path or -Url is the
    declarative default action (Win顺 opens it without asking back);
    otherwise Enter sends an action request (OnAction; once plugins:
    WS_ACTION) -- the default copies -Copy, else the title.
    #>
    param(
        [Parameter(Mandatory = $true)] [string] $Id,
        [Parameter(Mandatory = $true)] [string] $Title,
        [string] $Subtitle,
        [string] $Icon,
        [int] $Score = -1,
        [string] $Copy,
        [string] $Autocomplete,
        [string] $Path,
        [string] $Url,
        [string] $Preview,
        $Data,
        [array] $Actions,
        [switch] $Invalid,
        [string] $Match,
        $Modified,
        $Created,
        [long] $Size = -1,
        [string] $AwaySubtitle # subtitleAway: the second line while the keyboard is on another row (named so that -Sub… is still -Subtitle)
    )
    $row = [ordered]@{ id = $Id; title = $Title }
    if ($Subtitle) { $row.subtitle = $Subtitle }
    if ($AwaySubtitle) { $row.subtitleAway = $AwaySubtitle }
    if ($Icon) { $row.icon = $Icon }
    if ($Score -ge 0) { $row.score = $Score }
    if ($PSBoundParameters.ContainsKey('Copy')) { $row.copy = $Copy }
    if ($Autocomplete) { $row.autocomplete = $Autocomplete }
    if ($Path) { $row.open = @{ path = $Path } } elseif ($Url) { $row.open = @{ url = $Url } }
    if ($Preview) { $row.preview = @{ text = $Preview } }
    if ($null -ne $Data) { $row.data = $Data }
    if ($Actions) { $row.actions = @($Actions) }
    if ($Invalid) { $row.valid = $false }
    if ($Match) { $row.match = $Match }
    if ($null -ne $Modified) { $row.modified = $Modified }
    if ($null -ne $Created) { $row.created = $Created }
    if ($Size -ge 0) { $row.size = $Size }
    $row
}

function New-WinShunAction {
    <# An action of an item; -Key is Enter, Ctrl+Enter or Shift+Enter. #>
    param([Parameter(Mandatory = $true)] [string] $Id, [Parameter(Mandatory = $true)] [string] $Title, [string] $Key = 'Enter')
    @{ id = $Id; title = $Title; key = $Key }
}

function New-WinShunEffect {
    <#
    What an action asks Win顺 to do, in order: -Hide (the window closes;
    the default), -Keep, -Notify text (the footer), -Copy text, -SetQuery
    text, -OpenPath path, -OpenUrl url, -Replace items (a second-level list).

    Win顺 carries out -OpenPath/-OpenUrl (and an item's open). Without the
    "processes" permission it opens only a folder or an ordinary (non-program,
    non-script) file, and only http/https/mailto URLs; anything else is refused
    with a footer notice. Declare "processes" to open a program or a non-web link.
    #>
    param([switch] $Hide, [switch] $Keep, [string] $Notify, [string] $Copy, [string] $SetQuery, [string] $OpenPath,
        [string] $OpenUrl, [array] $Replace)
    if ($Hide) { return @{ type = 'hide' } }
    if ($Keep) { return @{ type = 'keep' } }
    if ($PSBoundParameters.ContainsKey('Notify')) { return @{ type = 'notify'; text = $Notify } }
    if ($PSBoundParameters.ContainsKey('Copy')) { return @{ type = 'copy'; text = $Copy } }
    if ($PSBoundParameters.ContainsKey('SetQuery')) { return @{ type = 'setQuery'; text = $SetQuery } }
    if ($OpenPath) { return @{ type = 'open'; path = $OpenPath } }
    if ($OpenUrl) { return @{ type = 'open'; url = $OpenUrl } }
    if ($PSBoundParameters.ContainsKey('Replace')) { return @{ type = 'replace'; items = @($Replace) } }
    @{ type = 'hide' }
}

# ---- once plugins ---------------------------------------------------------------

function Get-WinShunQuery {
    <# The query a once plugin was started for (the WS_* environment): text, raw, command, trigger, keyword, match. #>
    $match = @()
    if ($env:WS_MATCH) { try { $match = @(ConvertFrom-Json $env:WS_MATCH) } catch { } }
    $filters = [pscustomobject]@{}
    if ($env:WS_FILTERS) { try { $filters = ConvertFrom-Json $env:WS_FILTERS } catch { } }
    [pscustomobject]@{
        text = [string]$env:WS_QUERY; raw = [string]$env:WS_TEXT; command = [string]$env:WS_COMMAND
        trigger = if ($env:WS_TRIGGER) { $env:WS_TRIGGER } else { 'keyword' }
        keyword = [string]$env:WS_KEYWORD; match = $match
        filters = $filters
    }
}

function Get-WinShunSettings {
    <# The plugin's settings (manifest `settings`, with the user's values), as an object. #>
    if ($env:WS_SETTINGS) { try { return ConvertFrom-Json $env:WS_SETTINGS } catch { } }
    [pscustomobject]@{}
}

function Get-WinShunDataDir {
    <# The folder the plugin may write to (<id>.data); made when missing. #>
    $dir = if ($env:WS_DATA_DIR) { $env:WS_DATA_DIR } else { Join-Path $PSScriptRoot 'data' }
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $dir
}

function Test-WinShunAction {
    <# True when the process was started for an action (WS_ACTION), not a query. #>
    [bool]$env:WS_ACTION
}

function Get-WinShunActionRequest {
    <# The action a once plugin was started for. A `files` trigger chosen on Win顺's result menu: trigger 'files', action 'files', files = the chosen paths. #>
    $item = @{}
    if ($env:WS_ITEM) { try { $item = ConvertFrom-Json $env:WS_ITEM } catch { } }
    $files = @()
    if ($env:WS_FILES) { try { $files = @(ConvertFrom-Json $env:WS_FILES) } catch { } }
    $trigger = if ($env:WS_TRIGGER -eq 'files') { 'files' } else { '' }
    [pscustomobject]@{ action = [string]$env:WS_ACTION; item = $item; command = [string]$env:WS_COMMAND; text = [string]$env:WS_QUERY
        trigger = $trigger; files = $files }
}

function Send-WinShunReply {
    <# A once plugin's whole answer: the items (New-WinShunItem), then the process ends. #>
    param([Parameter(Mandatory = $true, Position = 0)] [AllowEmptyCollection()] [array] $Items, [double] $Rerun = 0, [int] $Cache = 0)
    $result = @{ items = @($Items) }
    if ($Rerun -gt 0) { $result.rerun = $Rerun }
    if ($Cache -gt 0) { $result.cache = $Cache }
    Send-WinShunLine $result
}

function Send-WinShunEffects {
    <# A once plugin's answer to an action: effects (New-WinShunEffect) in order. #>
    param([Parameter(Mandatory = $true, Position = 0)] [array] $Effects)
    Send-WinShunLine @{ effects = @($Effects) }
}

# ---- resident plugins -----------------------------------------------------------

function Send-WinShunNotification($Method, $Params) {
    Send-WinShunLine @{ jsonrpc = '2.0'; method = $Method; params = $Params }
}

function Send-WinShunPartial {
    <# Rows shown before the final answer (keyword mode shows them at once). #>
    param([Parameter(Mandatory = $true)] $QueryId, [Parameter(Mandatory = $true)] [AllowEmptyCollection()] [array] $Items, [switch] $Replace)
    Send-WinShunNotification 'query/partial' @{ queryId = $QueryId; items = @($Items); replace = [bool]$Replace }
}

function Write-WinShunLog {
    <# A line into plugins.log, through Win顺 (level: info | warn | error). #>
    param([Parameter(Mandatory = $true, Position = 0)] [string] $Text, [string] $Level = 'info')
    Send-WinShunNotification 'winshun/log' @{ level = $Level; text = $Text }
}

function Send-WinShunNotify {
    <# A line in the launcher's footer. #>
    param([Parameter(Mandatory = $true, Position = 0)] [string] $Text)
    Send-WinShunNotification 'winshun/notify' @{ text = $Text }
}

function Send-WinShunToast {
    <#
    A system notification (needs the "notifications" permission; without it
    the text goes to the launcher's footer). At most one every 2 seconds is
    shown, the newest of those waiting; keep file names and other private
    words out: it stays in the Action Center. -Click: effects for a click on
    it (New-WinShunEffect), e.g. -SetQuery 'kw '.
    #>
    param([Parameter(Mandatory = $true, Position = 0)] [string] $Title, [string] $Text = '', [array] $Click = @())
    Send-WinShunNotification 'winshun/notify' @{ toast = $true; title = $Title; text = $Text; click = @($Click) }
}

function Send-WinShunProgress {
    <# What the "正在等…" line says while a query runs. #>
    param([Parameter(Mandatory = $true)] $QueryId, [Parameter(Mandatory = $true)] [string] $Text)
    Send-WinShunNotification 'winshun/progress' @{ queryId = $QueryId; text = $Text }
}

function Invoke-WinShunTool {
    <#
    One of Win顺's tools (the same names and arguments as its MCP tools:
    search_files, search_content, recent_files, list_folder, get_file_info,
    open_item, show_in_folder, clipboard_history, clipboard_get,
    index_status), as the manifest's permissions allow. Returns the result,
    or an object with .error {code, message}. Lines that come meanwhile are
    kept for after.
    #>
    param([Parameter(Mandatory = $true, Position = 0)] [string] $Name, [hashtable] $Arguments = @{})
    $id = $script:NextId++
    Send-WinShunLine @{ jsonrpc = '2.0'; id = $id; method = 'winshun/tool'; params = @{ name = $Name; arguments = $Arguments } }
    while ($null -ne ($line = $script:In.ReadLine())) {
        try { $m = $line | ConvertFrom-Json } catch { continue }
        if (($null -eq $m.method) -and ($m.id -eq $id)) {
            if ($null -ne $m.error) { return [pscustomobject]@{ error = $m.error } }
            return $m.result
        }
        $script:Queued.Add($line)
    }
    [pscustomobject]@{ error = @{ code = -32000; message = 'Win顺 went away' } }
}

function Get-WinShunPluginSettings { $script:Settings }
function Get-WinShunLocale { $script:Locale }

function Start-WinShunPlugin {
    <#
    Runs a resident plugin until Win顺 says shutdown. -OnQuery { param($q,
    $plugin) } returns an array of items (New-WinShunItem), or a hashtable
    @{ items = ...; rerun = seconds; cache = seconds }. -OnAction { param($a,
    $plugin) } returns effects (New-WinShunEffect); without it Enter copies
    the item's copy text (else its title). -OnSettings { param($settings,
    $plugin) } hears the user's changes; -OnCancel { param($queryId,
    $plugin) } a query Win顺 no longer wants. $plugin has .settings, .locale,
    .theme, .pluginDir, .dataDir.
    #>
    param(
        [Parameter(Mandatory = $true)] [scriptblock] $OnQuery,
        [scriptblock] $OnAction,
        [scriptblock] $OnSettings,
        [scriptblock] $OnCancel,
        [string] $Name = 'plugin'
    )
    $plugin = [pscustomobject]@{ settings = @{}; locale = 'zh'; theme = 'light'; pluginDir = $PSScriptRoot; dataDir = '' }
    while ($true) {
        if ($script:Queued.Count -gt 0) {
            $line = $script:Queued[0]
            $script:Queued.RemoveAt(0)
        } else {
            $line = $script:In.ReadLine()
            if ($null -eq $line) { return }
        }
        if ($line.Trim() -eq '') { continue }
        try { $m = $line | ConvertFrom-Json } catch { Write-WinShunDebug "WinShun.psm1: not JSON: $line"; continue }
        $params = $m.params
        switch ($m.method) {
            'initialize' {
                $plugin.settings = $params.settings
                $plugin.locale = if ($params.locale) { $params.locale } else { 'zh' }
                $plugin.theme = if ($params.theme) { $params.theme } else { 'light' }
                if ($params.pluginDir) { $plugin.pluginDir = $params.pluginDir }
                $plugin.dataDir = [string]$params.dataDir
                $script:Settings = $plugin.settings
                $script:Locale = $plugin.locale
                Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; result = @{ protocolVersion = '1'; name = $Name } }
            }
            'query' {
                $q = [pscustomobject]@{
                    text = [string]$params.text; raw = [string]$params.raw; command = [string]$params.command
                    trigger = [string]$params.trigger; keyword = [string]$params.keyword
                    match = @($params.match); queryId = $params.queryId; limit = if ($params.limit) { $params.limit } else { 200 }
                    filters = if ($params.filters) { $params.filters } else { [pscustomobject]@{} }
                }
                try {
                    $result = & $OnQuery $q $plugin
                } catch {
                    Write-WinShunDebug "WinShun.psm1: query failed: $($_.Exception.Message)"
                    Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; error = @{ code = -32000; message = [string]$_.Exception.Message } }
                    continue
                }
                if ($result -is [hashtable] -and $result.ContainsKey('items')) {
                    $result.items = @($result.items)
                    Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; result = $result }
                } else {
                    Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; result = @{ items = @($result) } }
                }
            }
            'action' {
                $a = [pscustomobject]@{ action = [string]$params.action; item = $params.item; command = [string]$params.command; text = [string]$params.text
                    trigger = [string]$params.trigger; files = @($params.files | Where-Object { $null -ne $_ }) }
                if ($null -eq $OnAction) {
                    $text = if ($null -ne $a.item.copy) { [string]$a.item.copy } else { [string]$a.item.title }
                    $done = if ($plugin.locale -eq 'en') { 'Copied' } else { '已复制' }
                    $effects = @((New-WinShunEffect -Copy $text), (New-WinShunEffect -Notify $done), (New-WinShunEffect -Hide))
                } else {
                    try {
                        $effects = @(& $OnAction $a $plugin)
                    } catch {
                        Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; error = @{ code = -32000; message = [string]$_.Exception.Message } }
                        continue
                    }
                    if ($effects.Count -eq 0) { $effects = @((New-WinShunEffect -Hide)) }
                }
                Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; result = @{ effects = @($effects) } }
            }
            'cancel' { if ($OnCancel) { & $OnCancel $params.queryId $plugin } }
            'settingsChanged' {
                $plugin.settings = $params.settings
                $script:Settings = $plugin.settings
                if ($OnSettings) { & $OnSettings $plugin.settings $plugin }
            }
            'shutdown' {
                Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; result = @{} }
                return
            }
            default {
                if ($null -ne $m.id) { Send-WinShunLine @{ jsonrpc = '2.0'; id = $m.id; error = @{ code = -32601; message = "Method not found: $($m.method)" } } }
            }
        }
    }
}

Export-ModuleMember -Function Write-WinShunDebug, New-WinShunItem, New-WinShunAction, New-WinShunEffect, Get-WinShunQuery,
    Get-WinShunSettings, Get-WinShunDataDir, Test-WinShunAction, Get-WinShunActionRequest, Send-WinShunReply, Send-WinShunEffects,
    Send-WinShunPartial, Write-WinShunLog, Send-WinShunNotify, Send-WinShunToast, Send-WinShunProgress, Invoke-WinShunTool, Get-WinShunPluginSettings,
    Get-WinShunLocale, Start-WinShunPlugin
