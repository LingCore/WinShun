# Issue #143 live test: an Open dialog of the Windows XP style (WinForms asks
# GetOpenFileName with a hook when AutoUpgradeEnabled is off), with a name in
# its file name box. Run by live-nav.ps1, elevated; ends when it is killed.
#
#   powershell -STA -File dialog.ps1 -Title <caption> -Folder <where it opens> -Name <file name>
param(
    [Parameter(Mandatory)] [string] $Title,
    [Parameter(Mandatory)] [string] $Folder,
    [Parameter(Mandatory)] [string] $Name
)
Add-Type -AssemblyName System.Windows.Forms
# Started with a hidden console, a process's first ShowWindow hides whatever
# it is asked to show: spent on this.
$spent = New-Object System.Windows.Forms.Form
$spent.ShowInTaskbar = $false
$spent.Show(); $spent.Hide()
$dialog = New-Object System.Windows.Forms.OpenFileDialog
$dialog.AutoUpgradeEnabled = $false
$dialog.Title = $Title
$dialog.InitialDirectory = $Folder
$dialog.FileName = $Name
$dialog.CheckFileExists = $false
$dialog.RestoreDirectory = $true
[void]$dialog.ShowDialog()
