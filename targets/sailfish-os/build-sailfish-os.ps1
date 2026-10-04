param(
  [Parameter(Mandatory=$true)][string]$AppDirectory,
  [ValidateSet('i486','armv7hl','aarch64')][string]$Architecture = 'i486',
  [string]$CoreRepository = '',
  [string]$SdkRoot = 'C:\SailfishOS',
  [switch]$PrepareOnly,
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$argsForNode = @((Join-Path $here 'prepare.mjs'), (Resolve-Path $AppDirectory).Path, $Architecture)
if ($CoreRepository) { $argsForNode += (Resolve-Path $CoreRepository).Path }
& node @argsForNode
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($PrepareOnly) { exit 0 }
$appId = (Get-Content (Join-Path $AppDirectory 'package.json') | ConvertFrom-Json).gea.id
$project = Join-Path (Resolve-Path $AppDirectory).Path ".gea-sailfish/build/$appId-$Architecture/project"
$recover = Join-Path $here 'recover-build.mjs'
$recoverArgs = @($recover, $project)
if ($Clean) { $recoverArgs += '--clean' }
& node @recoverArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$sfdk = Join-Path $SdkRoot 'bin/sfdk.exe'
if (!(Test-Path $sfdk)) { throw "Sailfish SDK missing at $sfdk" }
$log = Join-Path ([System.IO.Path]::GetTempPath()) ("sailfish-sfdk-" + [guid]::NewGuid().ToString() + ".log")
Push-Location $project
try {
  & $sfdk --no-session -c "target=SailfishOS-5.1.0.11-$Architecture" build 2>&1 | Tee-Object -FilePath $log
  $code = $LASTEXITCODE
} finally {
  Pop-Location
}
if ($code -ne 0) {
  $last = Select-String -Path $log -Pattern 'error:|undefined reference|fatal error:|Error [0-9]' | Select-Object -Last 1
  if ($last) { Write-Host "sfdk build failed (exit $code): $($last.Line)" }
  else { Write-Host "sfdk build failed (exit $code)" }
}
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
exit $code
