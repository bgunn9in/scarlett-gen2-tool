param(
    [string]$Innoextract = 'innoextract.exe',
    [string]$SevenZip = '7z.exe',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
$Innoextract = (Get-Command $Innoextract -CommandType Application).Source
$SevenZip = (Get-Command $SevenZip -CommandType Application).Source
$output = (New-Item -ItemType Directory -Path $OutputDirectory -Force).FullName
$work = Join-Path $PSScriptRoot ('build\firmware-sources\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Test-Hash($Path, $Expected) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {
        throw "SHA-256 mismatch: $Path"
    }
}

function Expand-SevenZip($Archive, $Destination, $Member) {
    $arguments = @('x', '-y', '-bso0', '-bsp0', ('-o' + $Destination), $Archive)
    if ($Member) { $arguments += @('-r', $Member) }
    & $SevenZip @arguments
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed for $Archive (exit $LASTEXITCODE)." }
}

function Extract-Image($Source, $Offset, $Length, $Filename, $Sha) {
    $bytes = [byte[]]::new($Length)
    [Array]::Copy([IO.File]::ReadAllBytes($Source), $Offset, $bytes, 0, $Length)
    $candidate = Join-Path $work $Filename
    [IO.File]::WriteAllBytes($candidate, $bytes)
    Test-Hash $candidate $Sha
    Copy-Item -LiteralPath $candidate -Destination $output
}

$cdn = 'https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/focusrite/downloads'
$installer = Join-Path $work '1051.exe'
$dmg = Join-Path $work '1116.dmg'
Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 -Uri "$cdn/31626/scarlett-2nd-gen-firmware-updater-1.0.1.165.exe" -OutFile $installer
Test-Hash $installer 'e7da7667dcbb41ae6ef1a3ab399edf41bff78c08b664d356049e6a8490644223'
Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 -Uri "$cdn/32269/scarlettupdater-1.0.1.178.dmg" -OutFile $dmg
Test-Hash $dmg 'ffba1496c5b80144c4d98502024e60eddc3661bb43b48a67cd009bfd6c5e4578'

$win = Join-Path $work 'win'
$member = 'Scarlett (2nd Gen) Firmware Updater.exe'
& $Innoextract -d $win $installer
if ($LASTEXITCODE -ne 0) { throw "Innoextract failed (exit $LASTEXITCODE)." }
$pe = Join-Path $win "app\$member"
Test-Hash $pe '9e89a593e4b27ba828e6615a8a70b4e6697d0930491d8a7f712a7ab3710dc983'
Extract-Image $pe 0x183410 64000 '8202-1051.bin' 'f8084934586ad894d5c740d4ac6d7232b00dd48da47f12f1c8206477261f021f'

$disk = Join-Path $work 'dmg'
$pkg = Join-Path $work 'pkg'
$cpio = Join-Path $work 'cpio'
$app = Join-Path $work 'app'
Expand-SevenZip $dmg $disk 'ScarlettUpdater.pkg'
Expand-SevenZip (Join-Path $disk 'Install ScarlettUpdater\ScarlettUpdater.pkg') $pkg 'Payload'
Expand-SevenZip (Join-Path $pkg 'ScarlettUpdater.pkg\Payload') $cpio
Expand-SevenZip (Join-Path $cpio 'Payload~') $app 'ScarlettUpdater'
$macho = Join-Path $app 'Applications\ScarlettUpdater.app\Contents\MacOS\ScarlettUpdater'
Extract-Image $macho 0x116420 64256 '8202-1116.bin' '49782a53d27664c5d58494e69d347410e7996f2af1687bb3ea8e2820ab720c98'

Get-FileHash -LiteralPath (Join-Path $output '8202-1051.bin'), (Join-Path $output '8202-1116.bin')
