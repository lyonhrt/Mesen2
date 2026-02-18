# Build Mesen2 for Wine/Linux compatibility
# Creates a self-contained single-file executable WITHOUT AOT compilation
# AOT executables have Wine compatibility issues due to Windows-specific native code

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$repoRoot = Split-Path -Parent $scriptDir
Set-Location -Path $repoRoot

Write-Host "Building Wine-compatible Mesen2..."
Write-Host "Repo root: $repoRoot"

# First build the native Core library
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswherePath)) {
    throw "vswhere.exe not found. Please install Visual Studio 2017+."
}

$installPath = & $vswherePath -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $installPath) {
    throw 'Visual Studio with MSBuild component not found.'
}

$msbuild = Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    throw "MSBuild.exe not found under $installPath"
}

Write-Host "Using MSBuild: $msbuild"

# Build full solution (includes Core library)
$vsDevCmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
if (Test-Path $vsDevCmd) {
    Write-Host "Building full solution..."
    $cmd = "`"$vsDevCmd`" -arch=x64 -host_arch=x64 && `"$msbuild`" `"Mesen.sln`" /t:Restore /p:RestorePackagesConfig=true /v:minimal && `"$msbuild`" `"Mesen.sln`" /m /p:Configuration=Release /p:Platform=x64 /v:minimal"
    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "Solution build failed" }
}

# Create output directory
$outputDir = Join-Path $repoRoot 'bin\wine-compatible'
if (Test-Path $outputDir) {
    Remove-Item -Path $outputDir -Recurse -Force
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

# Publish UI project as self-contained single-file (NO AOT)
# This creates a single .exe that includes the .NET runtime
# Wine can run this better than AOT-compiled executables
Write-Host "Publishing Wine-compatible executable..."
$publishArgs = @(
    'publish',
    'UI/UI.csproj',
    '-c', 'Release',
    '-r', 'win-x64',
    '--self-contained', 'true',
    '-p:PublishSingleFile=true',
    '-p:PublishAot=false',
    '-p:IncludeNativeLibrariesForSelfExtract=true',
    '-p:EnableCompressionInSingleFile=true',
    '-p:OptimizeUi=true',
    '-o', $outputDir
)

& dotnet $publishArgs
if ($LASTEXITCODE -ne 0) { throw "Publish failed" }

# Copy the native Core DLL
$coreDll = Join-Path $repoRoot 'bin\win-x64\Release\MesenCore.dll'
if (Test-Path $coreDll) {
    Copy-Item -Path $coreDll -Destination $outputDir -Force
    Write-Host "Copied MesenCore.dll"
}

# Also copy any other required native DLLs
$nativeDlls = @('libSkiaSharp.dll', 'libHarfBuzzSharp.dll')
foreach ($dll in $nativeDlls) {
    $dllPath = Join-Path $repoRoot "bin\win-x64\Release\$dll"
    if (Test-Path $dllPath) {
        Copy-Item -Path $dllPath -Destination $outputDir -Force
        Write-Host "Copied $dll"
    }
}

Write-Host ""
Write-Host "=========================================="
Write-Host "Wine-compatible build complete!"
Write-Host "Output: $outputDir"
Write-Host ""
Write-Host "To run under Wine/Linux:"
Write-Host "  wine $outputDir\Mesen.exe"
Write-Host ""
Write-Host "Note: You may need to install Wine dependencies:"
Write-Host "  - vcrun2019 (via winetricks)"
Write-Host "  - corefonts (via winetricks)"
Write-Host "=========================================="
