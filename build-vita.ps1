param(
    [ValidateSet('Release', 'Profile', 'ScalarOracle')]
    [string]$Configuration = 'Release',
    [switch]$EmbedLegacyAssets
)

$ErrorActionPreference = 'Stop'

if ($EmbedLegacyAssets -and $Configuration -ne 'ScalarOracle') {
    throw 'Native builds require external content. Legacy embedding is diagnostic-only.'
}

$workspace = (Resolve-Path $PSScriptRoot).Path
$sdk = Join-Path $workspace '.tools\vitasdk'
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$ninja = (Get-Command ninja -ErrorAction Stop).Source
$python = (Get-Command python -ErrorAction Stop).Source
$toolchain = Join-Path $sdk 'share\vita.toolchain.cmake'
$vitaGlLeaf = if ($Configuration -eq 'Profile') { 'vitaGL-profile' } else { 'vitaGL-nosplash' }
$noSplashVitaGl = Join-Path $workspace "build\vita\$vitaGlLeaf\libvitaGL.a"
$vitaShark = Join-Path $workspace 'build\vita\vitaShaRK\libvitashark.a'
$buildLeaf = switch ($Configuration) {
    'Release' { 'release' }
    'Profile' { 'profile' }
    'ScalarOracle' { 'scalar-oracle' }
}
$build = Join-Path $workspace "build\vita\$buildLeaf"
$fidelityPackage = Join-Path $workspace 'assets\packages\mp-map-fidelity-retailclosure.kvap'
$currentFidelityPackage = Join-Path $workspace 'assets\packages\mp-map-fidelity-retailclosure-color-tail-model320-venv3.kvap'
$modelSemanticPackage = Join-Path $workspace 'assets\packages\mp-map-model-semantics.kvap'
$typedPackage = Join-Path $workspace 'assets\packages\shipment-db-reference.kvap'
$assetPackage = Join-Path $workspace 'assets\packages\shipment-reference.kvap'
$fallbackPackage = Join-Path $workspace 'assets\packages\shipment-load-smoke.kvap'

$requiredFiles = @($cmake, $ninja, $python, $toolchain)
foreach ($required in $requiredFiles) {
    if (!(Test-Path -LiteralPath $required)) {
        throw "Required Vita build dependency was not found: $required"
    }
}

$env:VITASDK = $sdk
$env:Path = (Join-Path $sdk 'bin') + ';' + $env:Path

$dependencyArgs = @((Join-Path $workspace 'kisakcod\tools\vita_asset_prep\build_vitagl.py'), '--workspace', $workspace)
if ($Configuration -eq 'Profile') { $dependencyArgs += '--profile' }
& $python @dependencyArgs
if ($LASTEXITCODE -ne 0) { throw 'Pinned Vita graphics dependency build failed.' }
& $python (Join-Path $workspace 'kisakcod\tools\vita_asset_prep\build_openal.py') --workspace $workspace
if ($LASTEXITCODE -ne 0) { throw 'Pinned Vita audio dependency build failed.' }

$configureArgs = @(
    '-S', (Join-Path $workspace 'kisakcod'),
    '-B', $build,
    '-G', 'Ninja',
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DKISAK_PLATFORM=vita',
    '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG',
    '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG',
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    '-DKISAK_VITA_EXTERNAL_CONTENT=ON'
)
$diagnosticOffOptions = @(
    'KISAK_VITA_EXPERIMENTAL_NATIVE_ENTITY_RUNTIME',
    'KISAK_VITA_SMOKE_AUTOSTART_SHIPMENT',
    'KISAK_VITA_SMOKE_GAMEPLAY_ONLY',
    'KISAK_VITA_SMOKE_AUTHORED_FX_EVENT',
    'KISAK_VITA_MENU_ONLY',
    'KISAK_VITA_DEBUG_NO_FRAME_EFFECTS',
    'KISAK_VITA_CAPTURE_OVERLAY_ONLY',
    'KISAK_VITA_SMOKE_LITE_ASSETS',
    'KISAK_VITA_NATIVE_FRAME_PROBE',
    'KISAK_VITA_NATIVE_DIRECT_DISPLAY_PROBE',
    'KISAK_VITA_NATIVE_SKIP_CLEAR_PROBE',
    'KISAK_VITA_NATIVE_SKIP_VIEWMODEL_PROBE',
    'KISAK_VITA_NATIVE_SKIP_MODEL_COLOR_PROBE',
    'KISAK_VITA_NATIVE_CPU_TEXTURE_UPLOAD'
    'KISAK_VITA_CLEAN_VIEWMODEL'
)
foreach ($option in $diagnosticOffOptions) {
    $configureArgs += "-D${option}=OFF"
}
$configureArgs += '-DKISAK_VITA_MENU_SCREEN='
$configureArgs += '-DKISAK_VITA_SMOKE_MAP_INDEX='
$configureArgs += '-DKISAK_VITA_NATIVE_TRIANGLE_BUDGET='
$buildType = if ($Configuration -eq 'Profile') { 'RelWithDebInfo' } else { 'Release' }
$configureArgs += "-DCMAKE_BUILD_TYPE=$buildType"
$configureArgs += if ($Configuration -eq 'ScalarOracle') {
    '-DKISAK_VITA_ALLOW_SOFTWARE_FALLBACK=ON'
} else {
    '-DKISAK_VITA_ALLOW_SOFTWARE_FALLBACK=OFF'
}
$configureArgs += if ($Configuration -eq 'Release') {
    '-DKISAK_VITA_PROFILE=OFF'
} else {
    '-DKISAK_VITA_PROFILE=ON'
}
$configureArgs += if ($Configuration -eq 'ScalarOracle') {
    '-DKISAK_VITA_SOURCE_RUNTIME=OFF'
} else {
    '-DKISAK_VITA_SOURCE_RUNTIME=ON'
}
$configureArgs += if ($Configuration -eq 'ScalarOracle') {
    '-DKISAK_VITA_CAPTURE_SOFTWARE_FRAME=ON'
} else {
    '-DKISAK_VITA_CAPTURE_SOFTWARE_FRAME=OFF'
}
$configureArgs += if ($Configuration -eq 'ScalarOracle') {
    '-DKISAK_VITA_VITA3K_NO_AUDIO=ON'
} else {
    '-DKISAK_VITA_VITA3K_NO_AUDIO=OFF'
}
if (Test-Path -LiteralPath $noSplashVitaGl) {
    $configureArgs += "-DKISAK_VITA_VITAGL_LIBRARY=$noSplashVitaGl"
}
$configureArgs += "-DKISAK_VITA_VITASHARK_LIBRARY=$vitaShark"
# Prefer the current source-derived multiplayer closure. The older descriptive
# names remain compatibility fallbacks for a fresh checkout or an older build
# tree that has not generated the current fidelity package yet.
if ($EmbedLegacyAssets -and (Test-Path -LiteralPath $currentFidelityPackage)) {
    $assetPackage = $currentFidelityPackage
}
elseif ($EmbedLegacyAssets -and (Test-Path -LiteralPath $fidelityPackage)) {
    $assetPackage = $fidelityPackage
}
elseif ($EmbedLegacyAssets -and (Test-Path -LiteralPath $typedPackage)) {
    $assetPackage = $typedPackage
}
if ($EmbedLegacyAssets -and !(Test-Path -LiteralPath $assetPackage) -and (Test-Path -LiteralPath $fallbackPackage)) {
    $assetPackage = $fallbackPackage
}
if ($EmbedLegacyAssets -and (Test-Path -LiteralPath $assetPackage)) {
    $configureArgs = $configureArgs | Where-Object { $_ -ne '-DKISAK_VITA_EXTERNAL_CONTENT=ON' }
    $configureArgs += '-DKISAK_VITA_EXTERNAL_CONTENT=OFF'
    $configureArgs += "-DKISAK_VITA_ASSET_PACKAGE=$assetPackage"
    Write-Host "Embedding local Vita asset package: $assetPackage"
}
if ($EmbedLegacyAssets -and (Test-Path -LiteralPath $modelSemanticPackage)) {
    $configureArgs += "-DKISAK_VITA_MODEL_SEMANTIC_PACKAGE=$modelSemanticPackage"
    Write-Host "Embedding optional Vita model semantic package: $modelSemanticPackage"
}
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'Vita CMake configure failed.' }

$buildTargets = if ($Configuration -eq 'ScalarOracle') {
    @('KisakCOD-vita-bootstrap.self-self')
} else {
    @('KisakCOD-vita-bootstrap.vpk-vpk',
      'KisakCOD-vita-source-ownership-report')
}
& $ninja -C $build -j 4 @buildTargets
if ($LASTEXITCODE -ne 0) { throw 'Vita build failed.' }

if ($Configuration -ne 'ScalarOracle') {
    & $python (Join-Path $workspace 'kisakcod\tools\vita_asset_prep\record_vita_build.py') `
        --workspace $workspace --build $build --configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'Vita artifact provenance verification failed.' }
}

$artifactName = if ($Configuration -eq 'ScalarOracle') {
    'KisakCOD-vita-bootstrap.self'
} else {
    'KisakCOD-vita-bootstrap.vpk'
}
$artifact = Join-Path $build "scripts\platform\vita\$artifactName"
if (!(Test-Path -LiteralPath $artifact)) {
    throw "VitaSDK did not produce the expected artifact: $artifact"
}
Write-Host "Built $artifact"
