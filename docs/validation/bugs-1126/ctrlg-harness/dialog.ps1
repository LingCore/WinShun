param([string]$start, [string]$result)
Add-Type -AssemblyName System.Windows.Forms
$d = New-Object System.Windows.Forms.OpenFileDialog
$d.Title = 'wsctrlg test dialog'
$d.InitialDirectory = $start
$d.RestoreDirectory = $true
$r = $d.ShowDialog()
$text = if ($r -eq 'OK') { $d.FileName } else { 'cancel' }
[IO.File]::WriteAllText($result, $text, (New-Object Text.UTF8Encoding $false))
