# The test's eyes inside Windows. Served by the host (tools/e2e/vm.py) and
# run by C:\e2e\boot.ps1 at every logon, as the logged-in user.
#
# It long-polls the host for a question and posts the answer:
#   windows    every top-level window: title, class, process, rectangle
#   elements   buttons, links, check boxes, menu items... in the windows
#              whose title, class or process matches, with their names,
#              rectangles, tick state, and whether they are part of a web
#              page (UI Automation)
#   run        a read-only diagnostic (file hashes, folder listings)
#
# It never clicks, types or invokes anything: every press in this test is
# made by the host through QEMU's keyboard and tablet, after the host has
# taken a screenshot. UI Automation only says where Microsoft's own
# buttons are; the AurOS installer draws its own window and has none.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'    # Invoke-WebRequest's bar costs seconds
$hub = 'http://10.0.2.2:8000'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class E2eUser32 {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

function Post($path, $obj) {
  $json = $obj | ConvertTo-Json -Depth 6 -Compress
  Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$hub$path" -TimeoutSec 30 `
    -Body ([Text.Encoding]::UTF8.GetBytes($json)) -ContentType 'application/json' | Out-Null
}

function Rect($e) {
  $r = $e.Current.BoundingRectangle
  if ($r.IsEmpty -or [double]::IsInfinity($r.Width)) { return $null }
  return @([int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height)
}

function ProcName($id) {
  try { (Get-Process -Id $id -ErrorAction Stop).ProcessName } catch { '' }
}

function TopWindows {
  $fg = [int64][E2eUser32]::GetForegroundWindow()
  $out = @()
  foreach ($w in $AE::RootElement.FindAll($TS::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
    try {
      $c = $w.Current
      $out += [pscustomobject]@{
        name = $c.Name; cls = $c.ClassName; proc = (ProcName $c.ProcessId)
        rect = (Rect $w); hwnd = [int64]$c.NativeWindowHandle
        foreground = ([int64]$c.NativeWindowHandle -eq $fg); offscreen = $c.IsOffscreen
      }
    } catch { }
  }
  return $out
}

function Elements($match, $types, $max) {
  $conds = @()
  foreach ($t in $types) {
    $ct = [System.Windows.Automation.ControlType]::$t
    if ($ct) { $conds += New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $ct) }
  }
  if ($conds.Count -eq 0) { return @() }
  if ($conds.Count -eq 1) { $cond = $conds[0] }
  else { $cond = New-Object System.Windows.Automation.OrCondition(,[System.Windows.Automation.Condition[]]$conds) }
  $out = @()
  foreach ($w in $AE::RootElement.FindAll($TS::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
    try {
      $c = $w.Current
      $p = ProcName $c.ProcessId
      if ($match -and -not ($c.Name -match $match -or $c.ClassName -match $match -or $p -match $match)) { continue }
      # Which of them belong to a web document, and how big it is: a
      # website fills the window, while Edge's own flyouts (downloads,
      # menus) are small documents too. The host uses the size so that a
      # page's own words ("press Keep anyway") are never taken for the
      # browser's buttons.
      $docIds = @{}
      try {
        $dcond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Document)
        foreach ($d in $w.FindAll($TS::Descendants, $dcond)) {
          $dr = Rect $d
          $area = 1
          if ($dr) { $area = [int64]$dr[2] * [int64]$dr[3] }
          foreach ($x in $d.FindAll($TS::Descendants, $cond)) {
            $k = ($x.GetRuntimeId() -join '.')
            if (-not $docIds.ContainsKey($k) -or $docIds[$k] -lt $area) { $docIds[$k] = $area }
          }
        }
      } catch { }
      foreach ($e in $w.FindAll($TS::Descendants, $cond)) {
        try {
          $ec = $e.Current
          if ($ec.IsOffscreen) { continue }
          $r = Rect $e
          if (-not $r -or $r[2] -le 0 -or $r[3] -le 0) { continue }
          $tog = $null
          try { $tog = "$($e.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState)" } catch { }
          $out += [pscustomobject]@{
            name = $ec.Name; type = $ec.ControlType.ProgrammaticName -replace '^ControlType\.', ''
            rect = $r; enabled = $ec.IsEnabled; toggle = $tog
            doc = [int64]$docIds[($e.GetRuntimeId() -join '.')]
            window = $c.Name; proc = $p; aid = $ec.AutomationId
          }
          if ($out.Count -ge $max) { return $out }
        } catch { }
      }
    } catch { }
  }
  return $out
}

$who = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
Post '/e2e/event?name=helper-up' @{
  user = $env:USERNAME; pid = $PID
  elevated = $who.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

while ($true) {
  try {
    $r = Invoke-WebRequest -UseBasicParsing "$hub/e2e/cmd" -TimeoutSec 40
  } catch { Start-Sleep 2; continue }
  if ($r.StatusCode -ne 200) { continue }
  $text = $r.Content
  if ($text -is [byte[]]) { $text = [Text.Encoding]::UTF8.GetString($text) }
  if (-not $text) { continue }
  $c = $text | ConvertFrom-Json
  $res = @{ id = $c.id; ok = $true }
  $t0 = Get-Date
  try {
    switch ($c.op) {
      'windows'  { $res.windows = @(TopWindows) }
      'elements' {
        $max = 400; if ($c.max) { $max = [int]$c.max }
        $res.elements = @(Elements $c.match @($c.types) $max)
      }
      'run'      { $res.output = (Invoke-Expression $c.script 2>&1 | Out-String -Width 220) }
      default    { $res.ok = $false; $res.error = "unknown op $($c.op)" }
    }
  } catch { $res.ok = $false; $res.error = "$_" }
  $res.ms = [int]((Get-Date) - $t0).TotalMilliseconds
  try { Post "/e2e/result?id=$($c.id)" $res } catch { }
}
