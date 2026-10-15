[CmdletBinding()]
param(
    [switch]$RestoreWdk,
    [switch]$Check,
    [switch]$Analyze,
    [switch]$ClientOnly,
    [switch]$DriverOnly,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workspacePath = $PSScriptRoot
if ($ClientOnly -and $DriverOnly) { throw 'Choose ClientOnly or DriverOnly, not both.' }
$packageVersion = '10.0.26100.6584'
$packageHash = 'C393D03DFB640B5C92F546B32F6770EF68CD3AAF691956E7D66D8E2C28A1B55E'
$toolsPath = Join-Path $workspacePath '.tools'
$packagePath = Join-Path $toolsPath "wdk.$packageVersion.nupkg"
$wdkPath = Join-Path $toolsPath 'wdk\c'
$sdkVersion = '10.0.26100.0'
$kmdfVersion = '1.15'
$kitPath = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$vsRoots = @()
$vsWherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vsWherePath) {
    $vsRoots += @(& $vsWherePath -all -prerelease -products '*' -property installationPath)
}
$vsRoots += @(
    'C:\Program Files\Microsoft Visual Studio\18\Insiders',
    'C:\Program Files\Microsoft Visual Studio\18\Community',
    'C:\Program Files\Microsoft Visual Studio\2026\Community',
    'C:\Program Files\Microsoft Visual Studio\2026\BuildTools',
    'C:\Program Files\Microsoft Visual Studio\2022\Community',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools'
)
$compilerRoot = $null
foreach ($vsRoot in ($vsRoots | Where-Object { $_ } | Select-Object -Unique)) {
    $msvcPath = Join-Path $vsRoot 'VC\Tools\MSVC'
    if (Test-Path -LiteralPath $msvcPath) {
        $compilerRoot = Get-ChildItem -LiteralPath $msvcPath -Directory |
            Sort-Object Name -Descending |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe') } |
            Select-Object -First 1 -ExpandProperty FullName
        if ($compilerRoot) { break }
    }
}

if ($Check) {
    [pscustomobject]@{
        Compiler = $compilerRoot
        SDK = (Test-Path -LiteralPath (Join-Path $kitPath "Include\$sdkVersion\shared\ntdef.h"))
        WDK = (Test-Path -LiteralPath (Join-Path $wdkPath "Include\$sdkVersion\km\ntifs.h"))
        KMDF = $kmdfVersion
        NativeJson = (Test-Path -LiteralPath (Join-Path $workspacePath 'client\third_party\nlohmann\json.hpp'))
    } | Format-List
    return
}
if (!$compilerRoot) { throw 'Install Visual Studio C++ build tools (Desktop development with C++).' }
if (!(Test-Path -LiteralPath (Join-Path $kitPath "Include\$sdkVersion\shared\ntdef.h"))) {
    throw "Install the Windows SDK with build number 26100. See README.md."
}
if (!$ClientOnly -and !(Test-Path -LiteralPath (Join-Path $wdkPath "Include\$sdkVersion\km\ntifs.h"))) {
    if (!$RestoreWdk) { throw 'Run .\build.ps1 -RestoreWdk once to fetch the pinned Microsoft WDK into .tools.' }
    New-Item -ItemType Directory -Force -Path $toolsPath | Out-Null
    if (!(Test-Path -LiteralPath $packagePath)) {
        $packageUri = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$packageVersion/microsoft.windows.wdk.x64.$packageVersion.nupkg"
        Write-Host "Downloading Microsoft WDK $packageVersion (about 106 MiB)..."
        Invoke-WebRequest -Uri $packageUri -OutFile $packagePath -TimeoutSec 180
    }
    if ((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -ne $packageHash) {
        throw 'WDK package hash mismatch. Remove the downloaded archive and retry.'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $extractPath = Join-Path $toolsPath 'wdk'
    if (Test-Path -LiteralPath $extractPath) {
        throw "Incomplete WDK extraction at $extractPath. Rename that folder and retry."
    }
    [IO.Compression.ZipFile]::ExtractToDirectory($packagePath, $extractPath)
}

$outputPath = Join-Path $workspacePath 'dist'
$objectPath = Join-Path $workspacePath 'build'
New-Item -ItemType Directory -Force -Path $outputPath, $objectPath | Out-Null
$binPath = Join-Path $compilerRoot 'bin\Hostx64\x64'
if (!$DriverOnly) {
    $jsonHeader = Join-Path $workspacePath 'client\third_party\nlohmann\json.hpp'
    if ((Get-FileHash -LiteralPath $jsonHeader -Algorithm SHA256).Hash -ne 'AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63') {
        throw 'Vendored JSON header hash mismatch.'
    }
    $clientIncludes = @(
        (Join-Path $compilerRoot 'include'),
        (Join-Path $kitPath "Include\$sdkVersion\ucrt"),
        (Join-Path $kitPath "Include\$sdkVersion\shared"),
        (Join-Path $kitPath "Include\$sdkVersion\um"),
        (Join-Path $workspacePath 'client\third_party')
    )
    $clientFlags = @('/nologo', '/c', '/std:c++17', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/Zi',
        '/guard:cf', '/permissive-', '/utf-8', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX',
        '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0A00', '/external:W0',
        "/I$(Join-Path $workspacePath 'client\include')", "/Fd$(Join-Path $objectPath 'Client.compiler.pdb')")
    foreach ($includePath in $clientIncludes) { $clientFlags += "/external:I$includePath" }
    $sources = @('client\src\memory.cpp', 'client\src\profile.cpp', 'client\src\main.cpp', 'samples\demo_target.cpp', 'client\examples\read_value.cpp', 'tests\native_tests.cpp')
    foreach ($source in $sources) {
        $baseName = [IO.Path]::GetFileNameWithoutExtension($source)
        & (Join-Path $binPath 'cl.exe') @clientFlags "/Fo$(Join-Path $objectPath "$baseName.obj")" (Join-Path $workspacePath $source)
        if ($LASTEXITCODE -ne 0) { throw "Native client compilation failed: $source ($LASTEXITCODE)." }
    }
    $clientLibrary = Join-Path $outputPath 'OffsetReaderClient.lib'
    & (Join-Path $binPath 'lib.exe') '/nologo' "/out:$clientLibrary" (Join-Path $objectPath 'memory.obj') (Join-Path $objectPath 'profile.obj')
    if ($LASTEXITCODE -ne 0) { throw 'Native client library build failed.' }
    Copy-Item -LiteralPath (Join-Path $objectPath 'Client.compiler.pdb') -Destination (Join-Path $outputPath 'Client.compiler.pdb') -Force
    $clientLinkFlags = @('/nologo', '/machine:x64', '/subsystem:console', '/dynamicbase', '/nxcompat',
        '/guard:cf', '/debug:full', '/opt:ref', '/opt:icf',
        "/libpath:$(Join-Path $compilerRoot 'lib\x64')", "/libpath:$(Join-Path $kitPath "Lib\$sdkVersion\um\x64")",
        "/libpath:$(Join-Path $kitPath "Lib\$sdkVersion\ucrt\x64")", $clientLibrary, 'kernel32.lib')
    $executables = @{ 'main' = 'OffsetReader'; 'demo_target' = 'OffsetReaderDemo'; 'native_tests' = 'OffsetReaderTests' }
    foreach ($baseName in $executables.Keys) {
        $targetName = $executables[$baseName]
        & (Join-Path $binPath 'link.exe') @clientLinkFlags (Join-Path $objectPath "$baseName.obj") "/out:$(Join-Path $outputPath "$targetName.exe")" "/pdb:$(Join-Path $outputPath "$targetName.pdb")" "/pdbaltpath:$targetName.pdb"
        if ($LASTEXITCODE -ne 0) { throw "Native client linking failed: $targetName ($LASTEXITCODE)." }
    }
    Write-Host "Built native C++ client: $(Join-Path $outputPath 'OffsetReader.exe')"
}
if (!$ClientOnly) {
    $includePaths = @(
        (Join-Path $wdkPath "Include\$sdkVersion\km"),
        (Join-Path $wdkPath "Include\$sdkVersion\km\crt"),
        (Join-Path $wdkPath "Include\wdf\kmdf\$kmdfVersion"),
        (Join-Path $kitPath "Include\$sdkVersion\shared"),
        (Join-Path $kitPath "Include\$sdkVersion\ucrt"),
        (Join-Path $compilerRoot 'include')
    )
    $kernelFlags = @(
        '/nologo', '/c', '/TP', '/std:c++17', '/kernel', '/GR-', '/permissive-', '/utf-8',
        '/W4', '/WX', '/Zl', '/Zp8', '/GS', '/guard:cf',
        '/Gy', '/Oi', '/O2', '/Zi', '/D_AMD64_', '/DAMD64', '/external:W0',
        '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A000000',
        '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=15',
        "/Fd$(Join-Path $objectPath 'OffsetReader.compiler.pdb')"
    )
    foreach ($includePath in $includePaths) { $kernelFlags += "/external:I$includePath" }
    $kernelSources = @(
        @{ Source = 'driver\OffsetReader.cpp'; Name = 'OffsetReader' },
        @{ Source = 'driver\memory_reader.cpp'; Name = 'memory_reader' },
        @{ Source = 'driver\examples\read_value.cpp'; Name = 'kernel_read_value' }
    )
    foreach ($source in $kernelSources) {
        $compilerArgs = $kernelFlags + "/Fo$(Join-Path $objectPath ($source.Name + '.obj'))"
        if ($Analyze) {
            $compilerArgs += @('/analyze', '/analyze:external-', '/analyze:log', (Join-Path $objectPath ($source.Name + '.analysis.xml')))
        }
        & (Join-Path $binPath 'cl.exe') @compilerArgs (Join-Path $workspacePath $source.Source)
        if ($LASTEXITCODE -ne 0) { throw "Driver compilation failed: $($source.Source) ($LASTEXITCODE)." }
    }
    $libraryPath = Join-Path $wdkPath "Lib\$sdkVersion\km\x64"
    $wdfLibraryPath = Join-Path $wdkPath "Lib\wdf\kmdf\x64\$kmdfVersion"
    $linkArgs = @(
        '/nologo', '/driver', '/kernel', '/subsystem:native,10.00', '/osversion:10.0',
        '/entry:FxDriverEntry', '/nodefaultlib', '/machine:x64', '/dynamicbase', '/nxcompat',
        '/guard:cf', '/integritycheck', '/debug:full', '/release', '/opt:ref', '/opt:icf',
        "/out:$(Join-Path $outputPath 'OffsetReader.sys')",
        "/pdb:$(Join-Path $outputPath 'OffsetReader.pdb')",
        '/pdbaltpath:OffsetReader.pdb',
        (Join-Path $objectPath 'OffsetReader.obj'),
        (Join-Path $objectPath 'memory_reader.obj'),
        (Join-Path $wdfLibraryPath 'wdfdriverentry.lib'),
        (Join-Path $wdfLibraryPath 'wdfldr.lib'),
        (Join-Path $libraryPath 'ntoskrnl.lib'),
        (Join-Path $libraryPath 'hal.lib'),
        (Join-Path $libraryPath 'wmilib.lib'),
        (Join-Path $libraryPath 'wdmsec.lib'),
        (Join-Path $libraryPath 'BufferOverflowFastFailK.lib')
    )
    & (Join-Path $binPath 'link.exe') @linkArgs
    if ($LASTEXITCODE -ne 0) { throw "Driver linking failed ($LASTEXITCODE)." }
    Write-Host "Built unsigned driver: $(Join-Path $outputPath 'OffsetReader.sys')"
    Get-FileHash -LiteralPath (Join-Path $outputPath 'OffsetReader.sys') -Algorithm SHA256
}
if ($Test) {
    $testPath = Join-Path $outputPath 'OffsetReaderTests.exe'
    if (!(Test-Path -LiteralPath $testPath)) { throw 'Build the native client tests first.' }
    Push-Location $workspacePath
    try {
        & $testPath
        if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
    } finally { Pop-Location }
}

