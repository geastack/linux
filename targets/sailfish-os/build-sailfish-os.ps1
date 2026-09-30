param(
  [Parameter(Mandatory=$true)][string]$AppDirectory,
  [ValidateSet('i486','armv7hl','aarch64')][string]$Architecture = 'i486',
  [string]$CoreRepository = '',
  [string]$SdkRoot = 'C:\SailfishOS',
  [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$argsForNode = @((Join-Path $here 'prepare.mjs'), (Resolve-Path $AppDirectory).Path, $Architecture)
if ($CoreRepository) { $argsForNode += (Resolve-Path $CoreRepository).Path }
& node @argsForNode
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($PrepareOnly) { exit 0 }
$project = Join-Path $here "build/$((Get-Content (Join-Path $AppDirectory 'package.json') | ConvertFrom-Json).gea.id)-$Architecture/project"
$sfdk = Join-Path $SdkRoot 'bin/sfdk.exe'
if (!(Test-Path $sfdk)) { throw "Sailfish SDK missing at $sfdk" }
Push-Location $project
try {
  & $sfdk --no-session -c "target=SailfishOS-5.1.0.11-$Architecture" build
} finally {
  Pop-Location
}
exit $LASTEXITCODE
