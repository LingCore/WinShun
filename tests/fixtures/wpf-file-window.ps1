# A stand-in for a program's own file window drawn with WPF (as ArcGIS Pro's
# "Add Data"; this is not ArcGIS), for tests/tst_dialogrules.cpp: a search
# box at the top, a box for the name at the bottom. Enter in the name box
# writes what it holds to -Result (UTF-8) and empties the box, as a window
# that has gone to the folder typed there does.
param([Parameter(Mandatory)] [string] $Result)

Add-Type -AssemblyName PresentationFramework

[xml] $xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="WsRuleFixture 添加数据" Width="420" Height="220" Left="40" Top="40"
        WindowStartupLocation="Manual" ShowActivated="False" Topmost="False">
  <DockPanel Margin="10">
    <TextBox x:Name="SearchBox" AutomationProperties.AutomationId="SearchBox" DockPanel.Dock="Top" Height="24"/>
    <StackPanel DockPanel.Dock="Bottom" Orientation="Horizontal">
      <TextBlock Text="名称" Width="50" VerticalAlignment="Center"/>
      <TextBox x:Name="NameBox" AutomationProperties.AutomationId="NameBox" Width="320" Height="24"/>
    </StackPanel>
    <ListBox/>
  </DockPanel>
</Window>
'@

$window = [Windows.Markup.XamlReader]::Load((New-Object System.Xml.XmlNodeReader $xaml))
$name = $window.FindName('NameBox')
$name.Add_KeyDown({
    param($sender, $e)
    if ($e.Key -eq [Windows.Input.Key]::Return) {
        [IO.File]::WriteAllText($Result, $sender.Text, [Text.UTF8Encoding]::new($false))
        $sender.Text = ''
        $e.Handled = $true
    }
})
[void] $window.ShowDialog()
