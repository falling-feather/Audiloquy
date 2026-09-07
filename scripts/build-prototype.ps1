[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipPackage,
    [string]$ToolchainBin = $env:AUDILOQUY_UCRT64_BIN,
    [string]$PackageDirectory
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ToolchainBin)) {
    $ToolchainBin = @('D:\msys64\ucrt64\bin', 'C:\msys64\ucrt64\bin') |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'cmake.exe') } |
        Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($ToolchainBin)) {
    throw 'UCRT64 toolchain not found. Pass -ToolchainBin or set AUDILOQUY_UCRT64_BIN.'
}
$toolchainBin = [System.IO.Path]::GetFullPath($ToolchainBin)
$toolchainPrefix = Split-Path -Parent $toolchainBin
$cmake = Join-Path $toolchainBin 'cmake.exe'
$ninja = Join-Path $toolchainBin 'ninja.exe'
$deploy = Join-Path $toolchainBin 'windeployqt.exe'
$objdump = Join-Path $toolchainBin 'objdump.exe'

foreach ($required in @($cmake, $ninja)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required prototype tool was not found: $required"
    }
}

$env:Path = "$toolchainBin;$env:Path"
$buildDirectory = Join-Path $projectRoot 'build\prototype'

& $cmake -S $projectRoot -B $buildDirectory -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_PREFIX_PATH=$toolchainPrefix" `
    '-DBUILD_TESTING=ON'
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& $cmake --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Prototype build failed.' }

& (Join-Path $toolchainBin 'ctest.exe') --test-dir $buildDirectory --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Prototype tests failed.' }

$executable = Join-Path $buildDirectory 'Audiloquy.exe'
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Prototype executable was not produced: $executable"
}

if (-not $SkipPackage) {
    $distribution = if ([string]::IsNullOrWhiteSpace($PackageDirectory)) {
        Join-Path $projectRoot 'dist\Audiloquy'
    } else {
        [System.IO.Path]::GetFullPath($PackageDirectory)
    }
    New-Item -ItemType Directory -Force -Path $distribution | Out-Null
    Copy-Item -Force -LiteralPath $executable -Destination $distribution
    & $deploy --release --compiler-runtime --no-translations `
        --no-system-d3d-compiler --no-system-dxc-compiler `
        (Join-Path $distribution 'Audiloquy.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }

    # The MSYS2 build of windeployqt does not always copy its non-Qt UCRT64
    # dependencies when the destination path contains non-ASCII characters.
    # Resolve PE imports recursively so the packaged prototype runs without a
    # preconfigured MSYS2 PATH.
    if (-not (Test-Path -LiteralPath $objdump)) {
        throw "Dependency scanner was not found: $objdump"
    }
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    Get-ChildItem -Recurse -File -LiteralPath $distribution |
        Where-Object { $_.Extension -in '.exe', '.dll' } |
        ForEach-Object { $queue.Enqueue($_.FullName) }
    Push-Location $projectRoot
    try {
        while ($queue.Count -gt 0) {
            $binary = $queue.Dequeue()
            if (-not $seen.Add($binary)) { continue }
            # MSYS2 objdump cannot accept a non-ASCII absolute Windows path,
            # while an ASCII path relative to the same current directory works.
            $binaryForTool = [System.IO.Path]::GetRelativePath($projectRoot, $binary)
            $imports = & $objdump -p $binaryForTool 2>$null |
                Select-String -Pattern '^\s*DLL Name:\s*(.+)$' |
                ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
            if ($LASTEXITCODE -ne 0) {
                throw "Dependency scan failed for '$binary'."
            }
            foreach ($library in $imports) {
                $packagedLibrary = Join-Path $distribution $library
                if (Test-Path -LiteralPath $packagedLibrary) { continue }
                $systemLibrary = Join-Path ([Environment]::SystemDirectory) $library
                if (Test-Path -LiteralPath $systemLibrary) { continue }
                $toolchainLibrary = Join-Path $toolchainBin $library
                if (Test-Path -LiteralPath $toolchainLibrary) {
                    Copy-Item -Force -LiteralPath $toolchainLibrary -Destination $packagedLibrary
                    $queue.Enqueue($packagedLibrary)
                    continue
                }
                if ($library -notmatch '^(api-ms-win-|ext-ms-win-)') {
                    throw "Unresolved packaged dependency '$library' required by '$binary'."
                }
            }
        }
    } finally {
        Pop-Location
    }

    $exampleDirectory = Join-Path $distribution 'examples'
    New-Item -ItemType Directory -Force -Path $exampleDirectory | Out-Null
    Copy-Item -Force -LiteralPath (Join-Path $projectRoot 'examples\demo-project.json') `
        -Destination $exampleDirectory
    Copy-Item -Force -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.txt') `
        -Destination $distribution
    Copy-Item -Force -LiteralPath (Join-Path $projectRoot 'README.txt') `
        -Destination $distribution
    $voicePackDirectory = Join-Path $distribution 'voice-packs'
    New-Item -ItemType Directory -Force -Path $voicePackDirectory | Out-Null
    Copy-Item -Force -LiteralPath (Join-Path $projectRoot 'voice-packs\README.txt') `
        -Destination $voicePackDirectory
}

Write-Host "Audiloquy / 语澜 ready: $executable"
