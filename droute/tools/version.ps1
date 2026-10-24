param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$assembly = Get-Content (Join-Path $root 'installer/Properties/AssemblyInfo.cs') -Raw
if ($assembly -notmatch 'AssemblyVersion\("([0-9.]+)"\)') { throw 'Installer version not found' }
$version = $Matches[1]
$hash = git -C $root rev-parse --short=12 --verify HEAD
if ($LASTEXITCODE -ne 0) { throw 'Git revision not found' }
$header = "#pragma once`n#define DROUTE_VERSION `"$version+$hash`"`n"
$path = Join-Path $OutputDirectory 'version.h'
if (!(Test-Path -LiteralPath $path) -or (Get-Content -LiteralPath $path -Raw) -ne $header) {
    [IO.File]::WriteAllText($path, $header, [Text.UTF8Encoding]::new($false))
}
