# Build Mesen2 solution (Release|x64) using Visual Studio's MSBuild
# - Locates Visual Studio via vswhere
# - Initializes environment via VsDevCmd (if available)
# - Restores NuGet and builds solution

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Ensure we run from repo root
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$repoRoot = Split-Path -Parent $scriptDir
Set-Location -Path $repoRoot

Write-Host "Repo root: $repoRoot"

# Locate vswhere
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswherePath)) {
    throw "vswhere.exe not found at $vswherePath. Please install Visual Studio 2017+ (Build Tools or higher)."
}

# Find latest VS installation that includes MSBuild
$installPath = & $vswherePath -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installPath) {
    throw 'Visual Studio with MSBuild component not found.'
}

# Resolve MSBuild path
$msbuild = Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    $candidate = Get-ChildItem -Path (Join-Path $installPath 'MSBuild') -Filter MSBuild.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($candidate) { $msbuild = $candidate.FullName }
}
if (-not (Test-Path $msbuild)) {
    throw "MSBuild.exe not found under $installPath"
}

Write-Host ("Using MSBuild: {0}" -f $msbuild)

# Try to initialize VS dev env for C++ toolchain
$vsDevCmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
$exitCode = 0
if (Test-Path $vsDevCmd) {
    Write-Host ("Using VsDevCmd: {0}" -f $vsDevCmd)
    $cmd = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && `"$msbuild`" `"Mesen.sln`" /t:Restore /p:RestorePackagesConfig=true /v:minimal && `"$msbuild`" `"Mesen.sln`" /m /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /v:minimal"
    cmd.exe /c $cmd
    $exitCode = $LASTEXITCODE
} else {
    Write-Host 'VsDevCmd not found, invoking MSBuild directly'
    & $msbuild 'Mesen.sln' /t:Restore /p:RestorePackagesConfig=true /v:minimal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $msbuild 'Mesen.sln' /m /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /v:minimal
    $exitCode = $LASTEXITCODE
}

if ($exitCode -ne 0) {
    throw "Build failed with exit code $exitCode"
}

Write-Host 'Build succeeded (Release|x64)'
