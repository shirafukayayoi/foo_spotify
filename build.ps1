param(
    [string]$VSPath,
    [string]$Config = "Release",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

if (-not $VSPath) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VSPath = & $vswhere -latest -products * -property installationPath
    Write-Host "Visual Studio: $VSPath"
}

$MSBuildPath = Join-Path $VSPath "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $MSBuildPath)) {
    throw "MSBuild が見つかりません: $MSBuildPath"
}

Write-Host "=== foo_spotify_linker をビルドします ===" -ForegroundColor Green
& $MSBuildPath "foo_spotify_linker\foo_spotify_linker.vcxproj" "/p:Configuration=$Config" "/p:Platform=$Platform" "/nologo"
if ($LASTEXITCODE -ne 0) {
    throw "ビルドに失敗しました"
}

Write-Host "=== .fb2k-component を作成します ===" -ForegroundColor Green
python "foo_spotify_linker\scripts\pack_component.py" "--platform" $Platform "--configuration" $Config
if ($LASTEXITCODE -ne 0) {
    throw "パッケージ作成に失敗しました"
}

Write-Host "DLL: foo_spotify_linker\_result\${Platform}_${Config}\bin\foo_spotify_linker.dll"
Write-Host "Component: foo_spotify_linker\_result\foo_spotify_linker.fb2k-component"
