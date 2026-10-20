# Builds out\totalcmd-git.wdx (32-bit) and out\totalcmd-git.wdx64 (64-bit) with MSVC (VS / Build Tools).
# With -Package, also creates out\totalcmd-git-<version>.zip, installable by opening it in Total Commander.
# -Version X.Y.Z sets the version; by default it is taken from the latest vX.Y.Z git tag.
param([switch]$Package, [string]$Version)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not $Version) {
    $tag = git -C $root describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' 2>$null
    $Version = if ($LASTEXITCODE -eq 0 -and $tag) { $tag.TrimStart('v') } else { '0.0.0' }
}
if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "Version must be X.Y.Z, got '$Version'" }
$numeric = "$($Matches[1]),$($Matches[2]),$($Matches[3]),0"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC toolchain not found. Install VS Build Tools with "Desktop development with C++".' }

New-Item -ItemType Directory -Force "$root\out\obj" | Out-Null
Set-Content "$root\out\obj\version.h" "#define GP_VERSION $numeric`n#define GP_VERSION_STR `"$Version`"" -Encoding ascii

foreach ($target in @(@{ Arch = 'x86'; Ext = 'wdx' }, @{ Arch = 'x64'; Ext = 'wdx64' })) {
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars$(if ($target.Arch -eq 'x86') { '32' } else { '64' }).bat"
    $obj = "$root\out\obj\$($target.Arch)"
    $out = "$root\out\totalcmd-git.$($target.Ext)"
    New-Item -ItemType Directory -Force $obj | Out-Null
    $rc = "rc /nologo /i `"$root\out\obj`" /fo `"$obj\totalcmd-git.res`" `"$root\src\totalcmd-git.rc`""
    $cl = "cl /nologo /O2 /EHsc /std:c++17 /utf-8 /W4 /MT /LD /Fo`"$obj\\`" `"$root\src\totalcmd-git.cpp`" `"$obj\totalcmd-git.res`" " +
          "/link /DEF:`"$root\src\totalcmd-git.def`" /OUT:`"$out`" /IMPLIB:`"$obj\totalcmd-git.lib`" user32.lib gdi32.lib"
    cmd /c "`"$vcvars`" >nul && $rc && $cl"
    if ($LASTEXITCODE) { throw "$($target.Arch) build failed ($LASTEXITCODE)" }
    Write-Host "Built $out ($Version)"
}

if ($Package) {
    $pkg = "$root\out\pkg"
    Remove-Item -LiteralPath $pkg -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $pkg | Out-Null
    Copy-Item "$root\out\totalcmd-git.wdx", "$root\out\totalcmd-git.wdx64", "$root\dist\readme.txt",
              "$root\dist\readme_rus.txt", "$root\LICENSE" $pkg
    (Get-Content "$root\dist\pluginst.inf" -Raw).Replace('@VERSION@', $Version) |
        Set-Content "$pkg\pluginst.inf" -Encoding ascii -NoNewline
    $zip = "$root\out\totalcmd-git-$Version.zip"
    Compress-Archive -Path "$pkg\*" -DestinationPath $zip -Force
    Write-Host "Packaged $zip"
}
