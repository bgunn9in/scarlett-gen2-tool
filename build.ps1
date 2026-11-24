$ErrorActionPreference = 'Stop'
$compiler = (Get-Command cl.exe -CommandType Application).Source
$source = Join-Path $PSScriptRoot 'src'
$output = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$images = @{
    '8202-1051.bin' = 'f8084934586ad894d5c740d4ac6d7232b00dd48da47f12f1c8206477261f021f'
    '8202-1116.bin' = '49782a53d27664c5d58494e69d347410e7996f2af1687bb3ea8e2820ab720c98'
}
foreach ($name in $images.Keys) {
    $image = Join-Path $PSScriptRoot ('firmware\' + $name)
    if ((Get-FileHash -LiteralPath $image).Hash -ne $images[$name]) {
        throw "Firmware SHA-256 mismatch: $image"
    }
    Copy-Item -LiteralPath $image -Destination $output
}
Push-Location -LiteralPath $output
try {
    $compile = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/guard:cf', '/utf-8')
    $link = @('/link', '/GUARD:CF', '/MACHINE:X64', '/Brepro', 'setupapi.lib', 'bcrypt.lib', 'kernel32.lib')
    & $compiler @compile '/Fescarlett-gen2.exe' (Join-Path $source 'gui.cpp') @link '/SUBSYSTEM:WINDOWS' '/MANIFEST:EMBED' user32.lib gdi32.lib shell32.lib advapi32.lib comctl32.lib
    if ($LASTEXITCODE -ne 0) { throw "GUI compilation failed (exit $LASTEXITCODE)." }
    & $compiler @compile '/Fescarlett1116.exe' (Join-Path $source 'updater.cpp') @link
    if ($LASTEXITCODE -ne 0) { throw "CLI compilation failed (exit $LASTEXITCODE)." }

    Get-FileHash -LiteralPath 'scarlett-gen2.exe'
} finally {
    Pop-Location
}
