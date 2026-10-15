[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install', 'Start', 'Stop', 'Remove', 'Status')][string]$Action,
    [string]$DriverPath = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\OffsetReader.sys')
)
$ErrorActionPreference = 'Stop'
$serviceName = 'OffsetReader'
if ($Action -ne 'Status') {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this command in an elevated PowerShell on your driver test VM.'
    }
}
function Invoke-ServiceCommand {
    param([string[]]$Arguments)
    & sc.exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "sc.exe failed with exit code $LASTEXITCODE." }
}
switch ($Action) {
    'Install' {
        $resolvedPath = (Resolve-Path -LiteralPath $DriverPath).Path
        $signature = Get-AuthenticodeSignature -LiteralPath $resolvedPath
        if ($signature.Status -ne 'Valid') {
            throw "Driver signature status is $($signature.Status). Sign and trust the test driver in your VM first. See README.md."
        }
        if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
            throw 'OffsetReader is already installed. Stop and remove that service before installing a replacement.'
        }
        Invoke-ServiceCommand -Arguments @('create', $serviceName, 'type=', 'kernel', 'start=', 'demand', 'binPath=', ('"' + $resolvedPath + '"'))
        Write-Host 'Installed for manual start. Run -Action Start to load it.'
    }
    'Start' { Invoke-ServiceCommand -Arguments @('start', $serviceName) }
    'Stop' { Invoke-ServiceCommand -Arguments @('stop', $serviceName) }
    'Remove' {
        $service = Get-Service -Name $serviceName -ErrorAction Stop
        if ($service.Status -ne 'Stopped') { throw 'Stop OffsetReader before removing it.' }
        Invoke-ServiceCommand -Arguments @('delete', $serviceName)
    }
    'Status' { Invoke-ServiceCommand -Arguments @('query', $serviceName) }
}

