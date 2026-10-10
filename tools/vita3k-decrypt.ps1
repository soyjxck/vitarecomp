# Decrypt an installed title's SELFs with Vita3K, driven through its own window.
#
# Vita3K decrypts a retail title's modules with the licence the dump carries
# (sce_sys/package/work.bin). That action, "Other > Decrypt All SELF", exists
# only in its Qt interface, so this script opens the interface, right-clicks
# the title in the app list, and picks the action through Windows UI
# Automation. The plaintext modules land in <Vita3K>\cache\decrypted_selfs\
# <TITLEID>\, which tools/import-vita3k.sh then brings into fixtures/.
#
#   powershell -ExecutionPolicy Bypass -File tools\vita3k-decrypt.ps1 -TitleId PCSA00029
#
# The title must already be installed in Vita3K (run Vita3K.exe <dump folder>
# once, or install it from the interface). Needs no firmware.
param(
    [string]$Vita3K = 'F:\Vita3K',
    [Parameter(Mandatory = $true)][string]$TitleId
)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class VitaMouse {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public static void Click(int x, int y, bool right) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(150);
        uint down = right ? 0x08u : 0x02u, up = right ? 0x10u : 0x04u;
        mouse_event(down, 0, 0, 0, UIntPtr.Zero); mouse_event(up, 0, 0, 0, UIntPtr.Zero);
    }
}
"@

$exe = Join-Path $Vita3K 'Vita3K.exe'
if (-not (Test-Path $exe)) { Write-Error "no Vita3K.exe under $Vita3K"; exit 1 }
$proc = Start-Process -FilePath $exe -WorkingDirectory $Vita3K -PassThru
Start-Sleep -Seconds 8

$root  = [System.Windows.Automation.AutomationElement]::RootElement
$byPid = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
$tsChildren = [System.Windows.Automation.TreeScope]::Children
$tsDesc     = [System.Windows.Automation.TreeScope]::Descendants

function Prop($prop, $value) { New-Object System.Windows.Automation.PropertyCondition($prop, $value) }
function Find-Named($scope, $type, $name) {
    $c = New-Object System.Windows.Automation.AndCondition(
        (Prop ([System.Windows.Automation.AutomationElement]::ControlTypeProperty) $type),
        (Prop ([System.Windows.Automation.AutomationElement]::NameProperty) $name))
    return $scope.FindFirst($tsDesc, $c)
}
function Find-MenuItem($name) {
    foreach ($w in $root.FindAll($tsChildren, $byPid)) {
        $e = Find-Named $w ([System.Windows.Automation.ControlType]::MenuItem) $name
        if ($e) { return $e }
    }
    return $null
}
function Center($e) {
    $r = $e.Current.BoundingRectangle
    return @([int]($r.X + $r.Width / 2), [int]($r.Y + $r.Height / 2))
}

# The welcome dialog, on a first run.
foreach ($w in $root.FindAll($tsChildren, $byPid)) {
    if ($w.Current.ClassName -eq 'WelcomeDialog') {
        $ok = $w.FindFirst($tsDesc, (Prop ([System.Windows.Automation.AutomationElement]::AutomationIdProperty) 'QApplication.welcome_dialog.accept'))
        if ($ok) { $ok.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Seconds 3 }
    }
}

$main = $null
foreach ($w in $root.FindAll($tsChildren, $byPid)) { if ($w.Current.ClassName -eq 'MainWindow') { $main = $w } }
if (-not $main) { Write-Error 'Vita3K main window not found'; Stop-Process -Id $proc.Id -Force; exit 1 }
$main.SetFocus()
Start-Sleep -Milliseconds 500

$cell = Find-Named $main ([System.Windows.Automation.ControlType]::DataItem) $TitleId
if (-not $cell) { Write-Error "$TitleId is not in Vita3K's app list; install it first"; Stop-Process -Id $proc.Id -Force; exit 1 }
$xy = Center $cell
[VitaMouse]::Click($xy[0], $xy[1], $false); Start-Sleep -Milliseconds 400
[VitaMouse]::Click($xy[0], $xy[1], $true);  Start-Sleep -Milliseconds 900

$other = Find-MenuItem 'Other'
if (-not $other) { Write-Error "context menu did not open"; Stop-Process -Id $proc.Id -Force; exit 1 }
$oxy = Center $other
[VitaMouse]::Click($oxy[0], $oxy[1], $false); Start-Sleep -Milliseconds 900

$dec = Find-MenuItem 'Decrypt All SELF'
if (-not $dec) { Write-Error "'Decrypt All SELF' not found in the Other menu"; Stop-Process -Id $proc.Id -Force; exit 1 }
$dxy = Center $dec
[VitaMouse]::Click($dxy[0], $dxy[1], $false)

$out = Join-Path $Vita3K "cache\decrypted_selfs\$TitleId"
$deadline = (Get-Date).AddSeconds(90)
do { Start-Sleep -Seconds 3 } while (-not (Test-Path $out) -and (Get-Date) -lt $deadline)
Start-Sleep -Seconds 5
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue

if (Test-Path $out) {
    "decrypted modules in $out"
    Get-ChildItem $out -Recurse -File | ForEach-Object { "  $($_.FullName.Substring($out.Length + 1))  $($_.Length) bytes" }
    "next: tools/import-vita3k.sh '$out' <name>"
} else {
    Write-Error "nothing appeared under $out"
    exit 1
}
