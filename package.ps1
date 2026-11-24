param(
    [string]$InnoCompiler = 'ISCC.exe',
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version = '1.1.0'
)
$ErrorActionPreference = 'Stop'
$iscc = (Get-Command $InnoCompiler -CommandType Application).Source
& (Join-Path $PSScriptRoot 'build.ps1')

$build = Join-Path $PSScriptRoot 'build'
$release = Join-Path $build "release-v$Version"
$staging = Join-Path $release 'stage'
$portable = Join-Path $staging 'Portable'
New-Item -ItemType Directory -Path $portable -Force | Out-Null

$files = foreach ($name in @('scarlett-gen2.exe', '8202-1051.bin', '8202-1116.bin')) {
    Join-Path $build $name
}
Copy-Item -LiteralPath $files -Destination $portable
& $iscc "/DReleaseVersion=$Version" "/O$staging" (Join-Path $PSScriptRoot 'installer.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed (exit $LASTEXITCODE)." }

$installer = Join-Path $staging 'Scarlett-Gen2-Tool-Setup.exe'
$archive = Join-Path $release "scarlett-gen2-tool-v$Version-windows-x64.zip"
Compress-Archive -LiteralPath $portable, $installer -DestinationPath $archive -CompressionLevel Optimal -Force
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Encoding ascii -Value "$hash  $([IO.Path]::GetFileName($archive))"
Get-Item -LiteralPath $archive
