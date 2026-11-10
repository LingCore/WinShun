param([string] $Stop = 'F:\电脑便捷工具\build\issues-266-270-274\i270\guard.stop')
# The window in front while Win+V is pressed: a NORMAL-rights one (the input
# panel does not come up over an administrator's window).
Add-Type -AssemblyName System.Windows.Forms
$form = New-Object System.Windows.Forms.Form
$form.Text = 'ws270 guard (medium)'
$form.Width = 520; $form.Height = 160; $form.StartPosition = 'CenterScreen'
$label = New-Object System.Windows.Forms.Label
$label.Text = 'Win顺 #270 test: please keep hands off keyboard and mouse.'; $label.AutoSize = $true; $label.Top = 30; $label.Left = 20
$form.Controls.Add($label)
$form.Show()
$end = (Get-Date).AddMinutes(15)
while ((Get-Date) -lt $end -and -not (Test-Path $Stop) -and $form.Visible) {
    [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 20
}
$form.Close()
