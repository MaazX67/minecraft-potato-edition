param(
  [Parameter(Mandatory=$true)][string]$Exe,
  [Parameter(Mandatory=$true)][string]$OutputRoot,
  [string]$Tag = 'native-validation'
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$OutputRoot = (Resolve-Path $OutputRoot).Path
$out = Join-Path $OutputRoot ($Tag + '-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class NativeSmokeInput {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, UIntPtr w, IntPtr l);
}
"@
function Post($message, $w = 0, $l = 0) {
  if ($message -ge 0x100 -and $message -le 0x20E) { $message += 0x8000 }
  if (![NativeSmokeInput]::PostMessageW($script:h, $message, [UIntPtr]::new([uint64]$w), [IntPtr]::new([int64]$l))) { throw 'Window message failed' }
}
function Key($key, $duration = 100) {
  Post 0x100 $key
  Start-Sleep -Milliseconds $duration
  Post 0x101 $key
  Start-Sleep -Milliseconds 180
}
function TypeText([string]$text) {
  foreach ($ch in $text.ToCharArray()) { Post 0x102 ([int]$ch) }
  Start-Sleep -Milliseconds 180
}
function Click($x, $y, $right = $false) {
  $lp = ($y -shl 16) -bor $x
  Post 0x200 0 $lp
  Start-Sleep -Milliseconds 80
  if ($right) { Post 0x204 2 $lp } else { Post 0x201 1 $lp }
  Start-Sleep -Milliseconds 100
  if ($right) { Post 0x205 0 $lp } else { Post 0x202 0 $lp }
  Start-Sleep -Milliseconds 180
}
function Check([bool]$condition, [string]$message) {
  if (!$condition) { throw "FAIL: $message; captures: $out" }
  Write-Host "PASS: $message"
}
function Capture($name, $screen) {
  $known = @(Get-ChildItem (Join-Path $out 'screenshots') -Filter '*.bmp' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)
  Key 0x71
  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  do {
    $bmp = Get-ChildItem (Join-Path $out 'screenshots') -Filter '*.bmp' -ErrorAction SilentlyContinue |
      Where-Object { $known -notcontains $_.Name } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($bmp -and (Test-Path ($bmp.FullName + '.json'))) { break }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  if (!$bmp -or !(Test-Path ($bmp.FullName + '.json'))) { throw "Missing fresh screenshot/state for $name" }
  $image = [System.Drawing.Bitmap]::FromFile($bmp.FullName)
  try { $image.Save((Join-Path $out ($name + '.png')), [System.Drawing.Imaging.ImageFormat]::Png) } finally { $image.Dispose() }
  $script:state = Get-Content ($bmp.FullName + '.json') -Raw | ConvertFrom-Json
  Copy-Item ($bmp.FullName + '.json') (Join-Path $out ($name + '.json'))
  Check ($script:state.screen -eq $screen) "$name reaches screen $screen"
  $script:p.Refresh()
  Write-Host "$name - $($script:p.MainWindowTitle)"
}
function Launch($launchArgs) {
  $script:p = Start-Process -FilePath $Exe -ArgumentList ('--automation ' + $launchArgs) -WorkingDirectory $out -PassThru
  for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    $script:p.Refresh()
    if ($script:p.HasExited) { throw "Game exited during startup: $($script:p.ExitCode)" }
    $script:h = $script:p.MainWindowHandle
    if ($script:h -ne [IntPtr]::Zero) { break }
  }
  if ($script:h -eq [IntPtr]::Zero) { throw 'Game window unavailable' }
  Start-Sleep -Seconds 10
  $script:p.Refresh()
  $script:h = $script:p.MainWindowHandle
  Write-Host "window=$($script:h) title=$($script:p.MainWindowTitle)"
}
function CloseGame {
  Post 0x10
  if (!$script:p.WaitForExit(15000)) { throw 'Test process did not close normally' }
  Check ($script:p.ExitCode -eq 0) 'game closes without crashing'
}
try {
  Launch '--autostart --inventory-test --debug'
  Capture '00-ground' 4
  Check ([Math]::Abs($state.yaw) -lt 0.001) 'fixture camera is not altered by desktop mouse input'
  Check ($state.assetVersion -ne 'unknown') 'installed Java asset version is identified'
  Key 0x45
  Capture '01-inventory' 5
  Click 539 518
  Click 719 270
  Capture '02-planks-preview' 5
  Check ($state.result.item -eq 'oak_planks' -and $state.result.count -eq 4) 'log previews four planks'
  Click 831 290
  Click 831 290
  Capture '03-no-duplicate' 5
  Check ($state.cursor.item -eq 'oak_planks' -and $state.cursor.count -eq 4 -and $state.result.count -eq 0) 'repeated result click does not duplicate items'
  Click 719 270 $true
  Click 755 270 $true
  Click 719 306 $true
  Click 755 306 $true
  Capture '04-table-preview' 5
  Check ($state.result.item -eq 'crafting_table' -and $state.result.count -eq 1) 'four planks preview a crafting table'
  Click 831 290
  Click 611 518
  Key 0x45
  Click 683 384 $true
  Capture '05-table-open' 5
  Check ($state.table -eq $true) 'right-click opens the placed table'
  Click 575 518
  foreach ($cell in @(0,1,2,3,5,6,7,8)) {
    Click (583 + ($cell % 3) * 36) (268 + [Math]::Floor($cell / 3) * 36) $true
  }
  Capture '06-furnace-preview' 5
  Check ($state.result.item -eq 'furnace' -and $state.result.count -eq 1) 'cobblestone ring previews one furnace'
  Click 771 304
  Click 771 304
  Click 647 518
  Capture '07-furnace-consumed' 5
  Check ($state.slots[30].item -eq 'furnace' -and $state.slots[30].count -eq 1 -and $state.result.count -eq 0) 'taking furnace consumes ingredients once'
  Check (@($state.craft | Where-Object { $_.count -ne 0 }).Count -eq 0) 'all furnace ingredients are consumed'
  Key 0x45
  Key 0x33
  Post 0x100 0x10
  Click 683 384 $true
  Post 0x101 0x10
  Capture '08-placement-consumed' 4
  Check ($state.slots[29].count -eq 0) 'placing the crafted table consumes its survival stack'
  Key 0x45
  Post 0x100 0x10
  Click 647 518
  Post 0x101 0x10
  Capture '09-shift-transfer' 5
  Check ($state.slots[30].count -eq 0 -and $state.slots[0].item -eq 'furnace' -and $state.slots[0].count -eq 1) 'shift-click transfers the furnace to storage'
  Click 539 402
  Key 0x1B
  Capture '10-close-cursor' 4
  Check ($state.cursor.count -eq 0 -and $state.slots[27].item -eq 'furnace') 'closing inventory returns cursor items without loss'
  for ($angle = 1; $angle -le 4; $angle++) {
    Post 0x8001 628 0
    Start-Sleep -Milliseconds 250
    Capture ('11-turn-' + $angle) 4
  }
  CloseGame
  Write-Host "CAPTURES=$out"
} finally {
  if ($script:p -and !$script:p.HasExited) {
    [NativeSmokeInput]::PostMessageW($script:p.MainWindowHandle, 0x10, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $null = $script:p.WaitForExit(15000)
  }
}
