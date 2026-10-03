param(
    [Parameter(Mandatory = $true)][string]$Cod4Root,
    [ValidateSet('mp_shipment', 'mp_crash', 'mp_crossfire')]
    [string[]]$Maps = @('mp_shipment'),
    [string]$ExporterPath = '',
    [ValidateRange(30, 1800)][int]$ExportTimeout = 600,
    [switch]$ContentOnly,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$arguments = @((Join-Path $PSScriptRoot 'tools/prepare_owned_content.py'),
    '--cod4-root', $Cod4Root, '--export-timeout', "$ExportTimeout", '--maps') + $Maps
if ($ExporterPath) { $arguments += @('--exporter', $ExporterPath) }
if ($ContentOnly) { $arguments += '--content-only' }
if ($CheckOnly) { $arguments += '--check-only' }
& python @arguments
if ($LASTEXITCODE -ne 0) { throw 'Owned-content preparation failed. See the reported error and local build logs.' }
