[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$HelperPath,
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'Audiloquy/voice-packs'),
    [string]$CacheDirectory
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$HelperPath = [IO.Path]::GetFullPath($HelperPath)
if (-not (Test-Path -LiteralPath $HelperPath -PathType Leaf)) { throw 'The Audiloquy voice helper is missing.' }
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
$pack = Join-Path $InstallRoot 'kokoro-en'
$manifest = Join-Path $InstallRoot 'kokoro-en.voice-pack.json'
if (Test-Path -LiteralPath $pack) {
    if ((Test-Path -LiteralPath (Join-Path $pack 'install-receipt.json')) -and
        (Test-Path -LiteralPath $manifest) -and
        (Test-Path -LiteralPath (Join-Path $pack 'model/model.int8.onnx')) -and
        (Test-Path -LiteralPath (Join-Path $pack 'runtime/bin/sherpa-onnx-offline-tts.exe'))) {
        Copy-Item -LiteralPath $HelperPath -Destination (Join-Path $pack 'audiloquy-kokoro-helper.exe') -Force
        Write-Output 'Kokoro is installed; the helper has been updated.'
        exit 0
    }
    throw 'An incomplete kokoro-en directory exists. Rename that directory before installing again.'
}
$stage = Join-Path $InstallRoot ('.kokoro-install-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$downloads = Join-Path $stage 'downloads'
New-Item -ItemType Directory -Path $downloads | Out-Null
try {
    $assets = @(
        @{ Name='sherpa-onnx-v1.13.7-win-x64-shared-MT-Release.tar.bz2'; Release='v1.13.7'; Hash='0B8F4A8CDDE53CEE671B0647947C66E10EFEC1B63ED2606C8EEAC650071C9C60' },
        @{ Name='kokoro-int8-multi-lang-v1_0.tar.bz2'; Release='tts-models'; Hash='4C3052ABAA60943A341F193888CF6ABD68787DAE6AB8AE5C925A706CAA247E4E' }
    )
    foreach ($asset in $assets) {
        $target = Join-Path $downloads $asset.Name
        $cached = if ($CacheDirectory) { Join-Path $CacheDirectory $asset.Name } else { '' }
        if ($cached -and (Test-Path -LiteralPath $cached -PathType Leaf)) {
            Copy-Item -LiteralPath $cached -Destination $target
        } else {
            Write-Output ('Downloading ' + $asset.Name)
            $url = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/' + $asset.Release + '/' + $asset.Name
            Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $target
        }
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $asset.Hash) { throw ('Checksum mismatch: ' + $asset.Name) }
        $entries = & tar -tf $target
        if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the voice archive.' }
        foreach ($entry in $entries) {
            if ($entry -match '(^[/\\]|^[A-Za-z]:|(^|[/\\])\.\.([/\\]|$))') { throw 'Archive contains an unsafe path.' }
        }
        Write-Output ('Extracting ' + $asset.Name)
        & tar -xjf $target -C $stage
        if ($LASTEXITCODE -ne 0) { throw 'Cannot extract the voice archive.' }
    }
    $assembled = Join-Path $stage 'kokoro-en'
    foreach ($movingPath in @($assembled,$pack,(Join-Path $stage 'kokoro-int8-multi-lang-v1_0'),(Join-Path $stage 'sherpa-onnx-v1.13.7-win-x64-shared-MT-Release'))) {
        if (-not ([IO.Path]::GetFullPath($movingPath).StartsWith($InstallRoot + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase))) { throw 'Installation path escaped the selected voice directory.' }
    }
    New-Item -ItemType Directory -Path $assembled | Out-Null
    Move-Item -LiteralPath (Join-Path $stage 'kokoro-int8-multi-lang-v1_0') -Destination (Join-Path $assembled 'model')
    Move-Item -LiteralPath (Join-Path $stage 'sherpa-onnx-v1.13.7-win-x64-shared-MT-Release') -Destination (Join-Path $assembled 'runtime')
    Copy-Item -LiteralPath $HelperPath -Destination (Join-Path $assembled 'audiloquy-kokoro-helper.exe')
    $receipt = @{ runtime='sherpa-onnx v1.13.7'; model='Kokoro v1.0 int8'; archives=$assets; source='https://github.com/k2-fsa/sherpa-onnx'; installedAt=[DateTime]::UtcNow.ToString('o') }
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText((Join-Path $assembled 'install-receipt.json'),($receipt | ConvertTo-Json -Depth 6),$utf8)
    $notice = @'
Kokoro is installed from the upstream sherpa-onnx release and used offline.
Kokoro model: Apache-2.0 (model/LICENSE).
sherpa-onnx runtime: Apache-2.0 with additional third-party components.
The current TTS executable includes GPL espeak-ng / piper-phonemize components.
It is a separate program, downloaded directly from upstream, not linked into Audiloquy.
Runtime source: https://github.com/k2-fsa/sherpa-onnx/tree/v1.13.7
Phonemizer source/license: https://github.com/rhasspy/piper-phonemize
eSpeak NG source/license: https://github.com/espeak-ng/espeak-ng
ONNX Runtime source/license: https://github.com/microsoft/onnxruntime
Keep upstream notices when redistributing these independent components.
Curated voice indices: af_heart=3, am_michael=16, bf_emma=21, bm_george=26.
'@
    [IO.File]::WriteAllText((Join-Path $assembled 'THIRD_PARTY_NOTICES.txt'),$notice,$utf8)
    $config = @{
        schemaVersion=1; id='kokoro-en'; name='Kokoro English'; helper='kokoro-en/audiloquy-kokoro-helper.exe';
        runtimeLicense='Apache-2.0; GPL-3.0 components in separate upstream executable'; modelLicense='Apache-2.0'; distributionMode='user-supplied';
        voices=@(
            @{id='af_heart';name='Heart - American female';locale='en-US';gender='female'},
            @{id='am_michael';name='Michael - American male';locale='en-US';gender='male'},
            @{id='bf_emma';name='Emma - British female';locale='en-GB';gender='female'},
            @{id='bm_george';name='George - British male';locale='en-GB';gender='male'}
        )
    }
    $pendingManifest=Join-Path $stage 'kokoro-en.voice-pack.json'
    [IO.File]::WriteAllText($pendingManifest,($config | ConvertTo-Json -Depth 5),$utf8)
    Move-Item -LiteralPath $assembled -Destination $pack
    Move-Item -LiteralPath $pendingManifest -Destination $manifest
    Write-Output 'Installed four offline English voices. You can disconnect from the Internet now.'
} finally {
    $resolvedStage=[IO.Path]::GetFullPath($stage)
    if ([IO.Path]::GetDirectoryName($resolvedStage) -eq $InstallRoot -and [IO.Path]::GetFileName($resolvedStage).StartsWith('.kokoro-install-')) {
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
