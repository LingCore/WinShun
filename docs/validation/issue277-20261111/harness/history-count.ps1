# How many items Windows' own clipboard history (Win+V) holds: read before and
# after a live test that copies, to notice a loss. Windows PowerShell 5.1
# (WinRT). Prints the number, or -1 when the history is off or unreadable.
$ErrorActionPreference = 'Stop'
try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    [void][Windows.ApplicationModel.DataTransfer.Clipboard, Windows.ApplicationModel.DataTransfer, ContentType = WindowsRuntime]
    $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } |
        Select-Object -First 1
    $operation = [Windows.ApplicationModel.DataTransfer.Clipboard]::GetHistoryItemsAsync()
    $task = $asTask.MakeGenericMethod([Windows.ApplicationModel.DataTransfer.ClipboardHistoryItemsResult]).Invoke($null, @($operation))
    [void]$task.Wait(5000)
    if ($task.Result.Status -ne 'Success') { -1 } else { $task.Result.Items.Count }
} catch {
    -1
}
