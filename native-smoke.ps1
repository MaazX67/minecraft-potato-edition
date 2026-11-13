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
  if (![NativeSmokeInput]::PostMessageW($script:h, $message, [UIntPtr]::new([uint64]([int64]$w -band 4294967295)), [IntPtr]::new([int64]$l))) { throw 'Window message failed' }
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
  Key 0x71 350
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
  Launch '--show-worlds'
  Capture 'empty-world-list' 11
  Check ($state.worldCount -eq 0 -and $state.selectedWorld -eq -1) 'empty Select World list has no playable selection'
  Click 683 736
  Capture 'empty-create-form' 1
  Key 0x1B
  Capture 'empty-create-cancel' 11
  CloseGame
  Launch '--autostart --inventory-test --save-test --debug'
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
  Key 0x1B
  Capture '12-before-save' 3
  $beforeSave = $state
  Click 683 296
  Capture '13-saved-title' 0
  Check (@(Get-ChildItem (Join-Path $out 'saves') -Recurse -Filter '*.potato').Count -eq 1) 'Save and Quit creates one world save'
  CloseGame
  Launch '--show-worlds --debug'
  Capture '14-world-list' 11
  Check ($state.worldCount -eq 1 -and $state.selectedWorld -eq 0) 'saved world appears in Select World after restart'
  Click 413 736
  Start-Sleep -Seconds 5
  Key 0x1B
  Capture '15-after-load' 3
  Check (!$state.resumePending -and $state.worldName -eq 'Save regression' -and $state.worldSeed -eq 1337) 'world name and seed reload without spawn reset'
  Check (($state.slots | ConvertTo-Json -Compress) -eq ($beforeSave.slots | ConvertTo-Json -Compress)) 'inventory reloads with exactly the saved stacks'
  Check ($state.fixtureBlock -eq $beforeSave.fixtureBlock -and $state.fixtureAbove -eq $beforeSave.fixtureAbove) 'placed world blocks survive process restart'
  Check ([Math]::Abs($state.yaw - $beforeSave.yaw) -lt 0.001 -and [Math]::Abs($state.pitch - $beforeSave.pitch) -lt 0.001) 'camera rotation survives process restart'
  Check ([Math]::Abs($state.position[0] - $beforeSave.position[0]) -lt 0.01 -and [Math]::Abs($state.position[2] - $beforeSave.position[2]) -lt 0.01) 'player horizontal position survives process restart'
  Check ($state.health -eq $beforeSave.health -and $state.hunger -eq $beforeSave.hunger) 'health and hunger survive process restart'
  $saveFile = Get-ChildItem (Join-Path $out 'saves') -Recurse -Filter '*.potato' | Select-Object -First 1
  $savedHash = (Get-FileHash $saveFile.FullName).Hash
  New-Item -ItemType Directory -Path ($saveFile.FullName + '.tmp') | Out-Null
  Click 683 296
  Capture '16-save-failed' 3
  Check ($state.actionBar -like '*save failed*') 'failed save leaves the world open and reports the error'
  Check ((Get-FileHash $saveFile.FullName).Hash -eq $savedHash) 'failed save leaves the last good save unchanged'
  Remove-Item ($saveFile.FullName + '.tmp')
  Key 0x1B
  Key 0x45
  Click 539 518
  Capture '17-held-cursor' 5
  $held = $state.cursor
  Check ($held.item -eq 'furnace' -and $held.count -eq 1) 'shutdown fixture holds one furnace on the cursor'
  CloseGame
  Launch '--show-worlds --debug'
  Click 413 736
  Start-Sleep -Seconds 5
  Capture '18-held-cursor-reload' 5
  Check ($state.cursor.item -eq $held.item -and $state.cursor.count -eq $held.count -and $state.slots[27].count -eq 0) 'window-close save restores held items without duplicating them'
  Key 0x1B
  CloseGame
  $original = [System.IO.File]::ReadAllBytes($saveFile.FullName)
  $damaged = $original.Clone()
  $offset = -1
  for ($i = 0; $i -lt $damaged.Length - 8; $i++) {
    if ($damaged[$i] -eq 80 -and $damaged[$i+1] -eq 84 -and $damaged[$i+2] -eq 69 -and $damaged[$i+3] -eq 68) { $offset = $i; break }
  }
  Check ($offset -ge 0) 'save contains a versioned block-edit stream'
  $damaged[$offset+4] = 99
  [System.IO.File]::WriteAllBytes($saveFile.FullName, $damaged)
  try {
    Launch '--show-worlds --debug'
    Click 413 736
    Capture '19-damaged-save' 11
    Check ($state.actionBar -like '*Could not load world*') 'unsupported block edits are rejected without entering the world'
    CloseGame
  } finally { [System.IO.File]::WriteAllBytes($saveFile.FullName, $original) }
  Launch '--show-worlds'
  Click 683 736
  for ($i = 0; $i -lt 9; $i++) { Post 0x102 8 }
  Start-Sleep -Milliseconds 180
  TypeText 'Creation regression'
  Click 683 72
  Click 683 146
  TypeText '424242'
  Click 423 72
  Click 683 206
  Click 683 282
  Capture '20-creation-form' 1
  Check ($state.seed -eq '424242' -and $state.draftDifficulty -eq 3) 'world form preserves seed and draft difficulty across tabs'
  Click 683 620
  Start-Sleep -Seconds 5
  Capture '21-created-world' 4
  Check ($state.worldName -eq 'Creation regression' -and $state.worldSeed -eq 424242 -and $state.mode -eq 1 -and $state.difficulty -eq 3) 'Create New World applies name, seed, Creative mode and Hard difficulty'
  Key 0x1B
  Click 683 296
  CloseGame
  Launch '--show-worlds'
  Capture '22-two-worlds' 11
  Check ($state.worldCount -eq 2 -and $state.selectedWorld -eq 0) 'new world is listed first without overwriting the earlier save'
  Click 413 736
  Start-Sleep -Seconds 5
  Capture '23-created-world-reload' 4
  Check ($state.worldName -eq 'Creation regression' -and $state.worldSeed -eq 424242 -and $state.mode -eq 1 -and $state.difficulty -eq 3) 'created world reopens with its persisted creation settings'
  Key 0x1B
  Click 683 296
  CloseGame
  for ($i = 0; $i -lt 4; $i++) {
    $copyDir = Join-Path (Join-Path $out 'saves') ('world-page-fixture-' + $i)
    New-Item -ItemType Directory -Path $copyDir | Out-Null
    Copy-Item $saveFile.FullName (Join-Path $copyDir 'world.potato')
  }
  Launch '--show-worlds'
  Capture '24-six-worlds' 11
  Check ($state.worldCount -eq 6 -and $state.worldPage -eq 0) 'world list includes all six saves'
  Click 1023 632
  Capture '25-world-page-two' 11
  Check ($state.worldPage -eq 1) 'Next page reveals saved worlds beyond the first five'
  Click 683 110
  Capture '26-selected-last-world' 11
  Check ($state.selectedWorld -eq 5) 'page-two row selects the correct global world index'
  Click 343 632
  Capture '27-world-page-one' 11
  Check ($state.worldPage -eq 0) 'Previous page returns to the first five entries'
  CloseGame
  Launch '--autostart --save-test --freeze-time --debug'
  Post 0x8002 6000
  Start-Sleep -Milliseconds 500
  Capture 'sky-noon-terrain' 4
  Check ($state.timelineLoaded -and $state.sunLoaded -and $state.moonLoaded) 'installed Java timeline and celestial textures load'
  Check ($state.worldTime -eq 6000 -and $state.daylight -eq 1 -and $state.stars -eq 0) 'noon has full daylight and no stars'
  Post 0x8001 0 -620
  Start-Sleep -Milliseconds 500
  Capture 'sky-noon-sun' 4
  Post 0x8001 0 620
  Post 0x8002 18000
  Start-Sleep -Milliseconds 500
  Capture 'sky-midnight-terrain' 4
  Check ([Math]::Abs($state.daylight - 0.24) -lt 0.001 -and $state.stars -eq 0.5) 'midnight uses Java night light and star brightness'
  Post 0x8001 -628 -160
  Post 0x8002 12500
  Start-Sleep -Milliseconds 500
  Capture 'sky-sunset' 4
  Post 0x8001 1256 0
  Post 0x8002 23500
  Start-Sleep -Milliseconds 500
  Capture 'sky-sunrise' 4
  Post 0x8001 -628 -620
  $moonHashes = @()
  for ($phase = 0; $phase -lt 8; $phase++) {
    Post 0x8002 ($phase * 24000 + 18000)
    Start-Sleep -Milliseconds 350
    Capture ('sky-moon-phase-' + $phase) 4
    Check ($state.moonPhase -eq $phase -and $state.moonLoaded) "moon phase $phase selects its installed texture"
    $phaseImage = [Drawing.Bitmap]::FromFile((Join-Path $out ('sky-moon-phase-' + $phase + '.png')))
    $crop = $phaseImage.Clone([Drawing.Rectangle]::new(500, 240, 366, 288), $phaseImage.PixelFormat)
    $stream = [IO.MemoryStream]::new()
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
      $crop.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
      $moonHashes += [BitConverter]::ToString($hash.ComputeHash($stream.ToArray()))
    } finally { $hash.Dispose(); $stream.Dispose(); $crop.Dispose(); $phaseImage.Dispose() }
  }
  Check (@($moonHashes | Select-Object -Unique).Count -eq 8) 'all eight moon phases produce distinct rendered pixels'
  Post 0x8002 138000
  Key 0x1B
  Post 0x8003 0
  Start-Sleep -Seconds 2
  Capture 'sky-paused-clock' 3
  Check ($state.worldTime -eq 138000) 'Pause freezes world time even when the clock is enabled'
  Key 0x1B
  Start-Sleep -Seconds 2
  Capture 'sky-running-clock' 4
  Check ($state.worldTime -gt 138020 -and $state.worldTime -lt 138080) 'running clock advances at twenty ticks per second'
  Key 0x45
  $timeBeforeInventory = $state.worldTime
  Start-Sleep -Seconds 2
  Capture 'sky-inventory-clock' 5
  Check ($state.worldTime -gt ($timeBeforeInventory + 20)) 'inventory does not stop the celestial clock'
  Key 0x1B
  Post 0x8003 1
  Post 0x8002 138000
  Key 0x1B
  Click 683 296
  Capture 'sky-save-title' 0
  CloseGame
  Launch '--show-worlds --freeze-time'
  Click 413 736
  Start-Sleep -Seconds 5
  Capture 'sky-time-reloaded' 4
  Check ($state.worldTime -eq 138000 -and $state.moonPhase -eq 5) 'world time and moon phase survive process restart exactly'
  CloseGame
  $skySave = Get-ChildItem (Join-Path $out 'saves') -Recurse -Filter '*.potato' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  $skyBytes = [IO.File]::ReadAllBytes($skySave.FullName)
  $formatOffset = [Text.Encoding]::ASCII.GetString($skyBytes).IndexOf('format') + 6
  Check ($formatOffset -ge 6 -and $skyBytes[$formatOffset] -eq 2) 'new world save has format two'
  $legacyBytes = $skyBytes.Clone()
  $legacyBytes[$formatOffset] = 1
  [IO.File]::WriteAllBytes($skySave.FullName, $legacyBytes)
  try {
    Launch '--show-worlds --freeze-time'
    Click 413 736
    Start-Sleep -Seconds 5
    Capture 'sky-legacy-save' 4
    Check ($state.worldTime -eq 0) 'previous format-one worlds still load and start at day zero'
    CloseGame
  } finally { [IO.File]::WriteAllBytes($skySave.FullName, $skyBytes) }
  $badTimeBytes = $skyBytes.Clone()
  $timeOffset = [Text.Encoding]::ASCII.GetString($badTimeBytes).IndexOf('worldTime') + 9
  Check ($timeOffset -ge 9 -and $badTimeBytes[$timeOffset] -eq 0xCE) 'time fixture uses an unsigned thirty-two-bit tick count'
  $badTimeBytes[$timeOffset] = 0xD2
  for ($i = 1; $i -le 4; $i++) { $badTimeBytes[$timeOffset + $i] = 0xFF }
  [IO.File]::WriteAllBytes($skySave.FullName, $badTimeBytes)
  try {
    Launch '--show-worlds --freeze-time'
    Click 413 736
    Capture 'sky-invalid-time' 11
    Check ($state.actionBar -like '*Could not load world*') 'negative saved world time is rejected without replacing the live world'
    CloseGame
  } finally { [IO.File]::WriteAllBytes($skySave.FullName, $skyBytes) }
  Write-Host "CAPTURES=$out"
} finally {
  if ($script:p -and !$script:p.HasExited) {
    [NativeSmokeInput]::PostMessageW($script:p.MainWindowHandle, 0x10, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $null = $script:p.WaitForExit(15000)
  }
}
