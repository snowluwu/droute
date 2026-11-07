param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$assembly = Get-Content -LiteralPath (Join-Path $root 'installer/Properties/AssemblyInfo.cs') -Raw
if ($assembly -notmatch 'AssemblyVersion\("(\d+)\.(\d+)\.(\d+)\.(\d+)"\)') {
    throw 'AssemblyVersion must contain four numeric components'
}
$parts = @($Matches[1], $Matches[2], $Matches[3], $Matches[4])
foreach ($part in $parts) {
    if ([uint64]$part -gt 65535) { throw 'Version component exceeds Windows resource limits' }
}
$version = $parts -join '.'
$hash = git -C $root rev-parse --short=12 --verify HEAD
if ($LASTEXITCODE -ne 0) { throw 'Git revision not found' }
$header = "#pragma once`n#define DROUTE_VERSION `"$version+$hash`"`n#define DROUTE_FILE_VERSION $($parts -join ',')`n#define DROUTE_FILE_VERSION_STRING `"$version`"`n"
$path = Join-Path $OutputDirectory 'version.h'
if (!(Test-Path -LiteralPath $path) -or (Get-Content -LiteralPath $path -Raw) -ne $header) {
    [IO.File]::WriteAllText($path, $header, [Text.UTF8Encoding]::new($false))
}
