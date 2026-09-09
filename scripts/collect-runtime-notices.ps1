#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageDirectory,
      [Parameter(Mandatory=$true)][string]$ToolchainBin)
$ErrorActionPreference='Stop'
$packageRoot=[IO.Path]::GetFullPath($PackageDirectory)
$prefix=Split-Path -Parent ([IO.Path]::GetFullPath($ToolchainBin))
$msysRoot=Split-Path -Parent $prefix
$pacman=Join-Path $msysRoot 'usr/bin/pacman.exe'
$licenseRoot=Join-Path $prefix 'share/licenses'
$output=Join-Path $packageRoot 'licenses'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$packages=@{}
$ownerPaths=@()
foreach($file in Get-ChildItem -LiteralPath $packageRoot -Recurse -File -Filter '*.dll') {
    $relative=[IO.Path]::GetRelativePath($packageRoot,$file.FullName)
    $original=Join-Path $ToolchainBin $file.Name
    if(-not(Test-Path -LiteralPath $original)){ $original=Join-Path $prefix ('share/qt6/plugins/'+$relative) }
    if(-not(Test-Path -LiteralPath $original)){throw "Cannot identify runtime dependency: $relative"}
    $unix='/'+[IO.Path]::GetRelativePath($msysRoot,$original).Replace([IO.Path]::DirectorySeparatorChar,'/')
    $ownerPaths+=$unix
}
$owners=& $pacman -Q -o @ownerPaths
if($LASTEXITCODE -ne 0){throw 'Some runtime dependencies have no package owner.'}
foreach($owner in $owners){if($owner -match 'is owned by (\S+) (\S+)'){$packages[$Matches[1]]=$Matches[2]}}
$allPackageFiles=& $pacman -Ql @($packages.Keys)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate package license files.'}
$records=@()
foreach($name in ($packages.Keys | Sort-Object)) {
    $version=$packages[$name]
    $desc=Join-Path $msysRoot ('var/lib/pacman/local/'+$name+'-'+$version+'/desc')
    $metadata=Get-Content -LiteralPath $desc -Raw -Encoding UTF8
    $base=if($metadata -match '(?m)^%BASE%\r?\n([^\r\n]+)'){$Matches[1]}else{$name -replace '^mingw-w64-ucrt-x86_64-','mingw-w64-'}
    $license=if($metadata -match '(?s)%LICENSE%\r?\n(.*?)\r?\n\r?\n'){$Matches[1].Trim()}else{'See bundled license files'}
    $upstream=if($metadata -match '(?m)^%URL%\r?\n([^\r\n]+)'){$Matches[1]}else{''}
    $files=@($allPackageFiles | Where-Object { $_.StartsWith($name+' ',[StringComparison]::Ordinal) })
    $copied=0
    foreach($line in $files) {
        if($line -match ' /ucrt64/share/licenses/(.+)$') {
            $relative=$Matches[1]
            $source=Join-Path $licenseRoot $relative
        } elseif($line -match ' (/ucrt64/.*/(?:LICENSE|COPYING|COPYRIGHT|NOTICE)(?:\.[^/]+)?)$') {
            $source=Join-Path $msysRoot $Matches[1].TrimStart('/')
            $relative=Join-Path ($name -replace '^mingw-w64-ucrt-x86_64-','') ([IO.Path]::GetFileName($source))
        } else {continue}
        if(-not(Test-Path -LiteralPath $source -PathType Leaf)){continue}
        $destination=Join-Path $output $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
        ++$copied
    }
    if(-not $copied){throw "No license text collected for $name"}
    $records+=@{name=$name;version=$version;license=$license;upstream=$upstream;sourcePackage="https://repo.msys2.org/mingw/sources/$base-$version.src.tar.zst";packagePage="https://packages.msys2.org/packages/$name";licenseFiles=$copied}
}
$manifest=@{generatedAt=[DateTime]::UtcNow.ToString('o');platform='Windows x64 / MSYS2 UCRT64';linkage='shared DLLs; replaceable alongside the executable';dependencies=$records}
[IO.File]::WriteAllText((Join-Path $output 'runtime-dependencies.json'),($manifest|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
Write-Output ('Collected runtime license files for '+$records.Count+' packages.')
