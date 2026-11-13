<#
Clean-machine check of a release zip in Windows Sandbox (Windows 10/11 Pro: "Windows Sandbox" feature turned on).

Inside the sandbox (no P:, no Arma, nothing installed):
  1. the zip is unpacked to the desktop;
  2. first start: screenshot (the setup window is expected);
  3. the viewer is closed and started again with a model of Arma 3 Samples: screenshot;
  4. the viewer's log and settings are copied out.
Results (PNG, logs, run.txt) land in -Out. The sandbox sees the zip and the Samples folder read-only.

Example:
  powershell -ExecutionPolicy Bypass -File tools\sandbox\Run-SandboxTest.ps1 `
      -Zip build\ArmaView-win64.zip -Samples "C:\Steam\steamapps\common\Arma 3 Samples" -Out sandbox_out
#>
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][string]$Samples,   # the Arma 3 Samples folder (with Addons inside)
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Model = "Addons\Test_Character_01\A3_character_example.p3d", # relative to -Samples
    [int]$TimeoutSec = 600
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $env:WINDIR "System32\WindowsSandbox.exe"
if (-not (Test-Path $exe)) { throw "Windows Sandbox is not turned on (Turn Windows features on or off > Windows Sandbox, then restart)." }
$Zip = (Resolve-Path $Zip).Path
$Samples = (Resolve-Path $Samples).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Out "done.txt")
$zipDir = Split-Path $Zip
$zipName = Split-Path $Zip -Leaf

# the script that runs inside the sandbox at logon
$guest = @'
$ErrorActionPreference = "Continue"
$out = "C:\out"
function Log($t) { Add-Content -Path "$out\run.txt" -Value ("{0:HH:mm:ss} {1}" -f (Get-Date), $t) }
function Shot($name) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save("$out\$name", [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Log "screenshot $name"
}
try {
    $dst = "$env:USERPROFILE\Desktop\ArmaView"
    Expand-Archive -Path "C:\zip\__ZIP__" -DestinationPath $dst -Force
    $viewer = Get-ChildItem -Path $dst -Recurse -Filter ArmaView.exe | Select-Object -First 1
    Log "unpacked: $($viewer.FullName)"
    $p = Start-Process -FilePath $viewer.FullName -PassThru
    Start-Sleep -Seconds 20
    Log "first start: running=$(-not $p.HasExited)"
    Shot "1_first_start.png"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    $model = "C:\samples\__MODEL__"
    Log "model exists: $(Test-Path $model)"
    $p = Start-Process -FilePath $viewer.FullName -ArgumentList "`"$model`"" -PassThru
    Start-Sleep -Seconds 25
    Log "model start: running=$(-not $p.HasExited)"
    Shot "2_samples_model.png"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    $data = Join-Path $env:LOCALAPPDATA "ArmaView"
    if (Test-Path $data) { Copy-Item -Recurse -Force "$data\*" "$out\appdata" -ErrorAction SilentlyContinue; Log "copied $data" }
    Get-ChildItem -Path $dst -Recurse -Filter *.log | ForEach-Object { Copy-Item $_.FullName "$out\" }
} catch { Log "ERROR: $_" }
Log "done"
Set-Content -Path "$out\done.txt" -Value "done"
shutdown /s /t 5
'@
$guest = $guest.Replace("__ZIP__", $zipName).Replace("__MODEL__", $Model)
Set-Content -Path (Join-Path $Out "guest.ps1") -Value $guest -Encoding UTF8

$wsb = @"
<Configuration>
  <Networking>Disable</Networking>
  <MappedFolders>
    <MappedFolder><HostFolder>$zipDir</HostFolder><SandboxFolder>C:\zip</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$Samples</HostFolder><SandboxFolder>C:\samples</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$Out</HostFolder><SandboxFolder>C:\out</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>powershell -ExecutionPolicy Bypass -File C:\out\guest.ps1</Command></LogonCommand>
</Configuration>
"@
$wsbFile = Join-Path $Out "test.wsb"
Set-Content -Path $wsbFile -Value $wsb -Encoding UTF8
Write-Host "Starting Windows Sandbox: $wsbFile"
Start-Process -FilePath $exe -ArgumentList "`"$wsbFile`""
$t0 = Get-Date
while (-not (Test-Path (Join-Path $Out "done.txt"))) {
    if (((Get-Date) - $t0).TotalSeconds -gt $TimeoutSec) { throw "timeout: no done.txt in $Out after $TimeoutSec s" }
    Start-Sleep -Seconds 5
}
Get-Content (Join-Path $Out "run.txt")
Write-Host "Results in $Out"
