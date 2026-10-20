# Builds out\totalcmd-git.wdx (32-bit) and out\totalcmd-git.wdx64 (64-bit) with MSVC (VS / Build Tools).
# With -Package, also creates out\totalcmd-git-<version>.zip, installable by opening it in Total Commander.
param([switch]$Package)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC toolchain not found. Install VS Build Tools with "Desktop development with C++".' }

foreach ($target in @(@{ Arch = 'x86'; Ext = 'wdx' }, @{ Arch = 'x64'; Ext = 'wdx64' })) {
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars$(if ($target.Arch -eq 'x86') { '32' } else { '64' }).bat"
    $obj = "$root\out\obj\$($target.Arch)"
    $out = "$root\out\totalcmd-git.$($target.Ext)"
    New-Item -ItemType Directory -Force $obj | Out-Null
    $rc = "rc /nologo /fo `"$obj\totalcmd-git.res`" `"$root\src\totalcmd-git.rc`""
    $cl = "cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W4 /MT /LD /Fo`"$obj\\`" `"$root\src\totalcmd-git.cpp`" `"$obj\totalcmd-git.res`" " +
          "/link /DEF:`"$root\src\totalcmd-git.def`" /OUT:`"$out`" /IMPLIB:`"$obj\totalcmd-git.lib`" user32.lib gdi32.lib"
    cmd /c "`"$vcvars`" >nul && $rc && $cl"
    if ($LASTEXITCODE) { throw "$($target.Arch) build failed ($LASTEXITCODE)" }
    Write-Host "Built $out"
}

if ($Package) {
    $version = (Get-Item "$root\out\totalcmd-git.wdx64").VersionInfo.ProductVersion
    $zip = "$root\out\totalcmd-git-$version.zip"
    $files = @("$root\out\totalcmd-git.wdx", "$root\out\totalcmd-git.wdx64", "$root\dist\pluginst.inf",
               "$root\dist\readme.txt", "$root\dist\readme_rus.txt", "$root\LICENSE")
    Compress-Archive -Path $files -DestinationPath $zip -Force
    Write-Host "Packaged $zip"
}
