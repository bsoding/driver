[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$workspacePath = Split-Path $PSScriptRoot -Parent
$outputPath = Join-Path $workspacePath 'dist'
$archivePath = Join-Path $outputPath 'OffsetReader-starter.zip'
$artifactNames = @('OffsetReader.exe', 'OffsetReaderDemo.exe', 'OffsetReaderTests.exe', 'OffsetReaderClient.lib', 'OffsetReader.sys')
$artifacts = @()
foreach ($name in $artifactNames) {
    $path = Join-Path $outputPath $name
    if (!(Test-Path -LiteralPath $path)) { throw "Missing $name. Run build.ps1 first." }
    $artifacts += Get-Item -LiteralPath $path
}
$sumsPath = Join-Path $outputPath 'SHA256SUMS.txt'
$sums = foreach ($artifact in $artifacts) {
    $hash = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($artifact.Name)"
}
[IO.File]::WriteAllText($sumsPath, (($sums -join "`n") + "`n"), [Text.Encoding]::ASCII)
$files = @('README.md', 'VERIFICATION.md', 'build.ps1') | ForEach-Object { Get-Item -LiteralPath (Join-Path $workspacePath $_) }
foreach ($folder in @('client', 'driver', 'profiles', 'samples', 'scripts', 'tests', 'docs')) {
    $files += Get-ChildItem -LiteralPath (Join-Path $workspacePath $folder) -Recurse -File |
        Where-Object { $_.FullName -notmatch '[\\/]__pycache__[\\/]' -and $_.Extension -ne '.pyc' -and $_.Name -notlike '*.generated.*' }
}
$files += $artifacts
$files += Get-ChildItem -LiteralPath $outputPath -Filter '*.pdb' -File
$files += Get-Item -LiteralPath $sumsPath
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.File]::Open($archivePath, [IO.FileMode]::Create)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($file in ($files | Sort-Object FullName -Unique)) {
            $relative = $file.FullName.Substring($workspacePath.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $relative, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }
$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    foreach ($artifact in $artifacts) {
        $entry = $archive.GetEntry('dist/' + $artifact.Name)
        if (!$entry) { throw "Missing packaged artifact $($artifact.Name)." }
        $entryStream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($entryStream)).Replace('-', '') }
        finally { $entryStream.Dispose(); $sha.Dispose() }
        if ($hash -ne (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash) { throw "Packaged hash mismatch: $($artifact.Name)" }
    }
    if ($archive.Entries | Where-Object { $_.FullName -like '*.py' }) { throw 'Python source unexpectedly included in the native package.' }
} finally { $archive.Dispose() }
Write-Host "Packaged native client, driver, SDK, and source: $archivePath"
Get-FileHash -LiteralPath $archivePath -Algorithm SHA256
