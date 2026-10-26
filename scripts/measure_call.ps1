<#
.SYNOPSIS
  CPU and memory of running Murmur windows (with their WebView2 processes) over a time window, for calls.
.EXAMPLE
  .\scripts\measure_call.ps1 -ProcessIds 1234,5678 -Seconds 10
#>
param(
    [Parameter(Mandatory)][string]$ProcessIds,                       # comma-separated process ids
    [int]$Seconds = 10
)
$ErrorActionPreference = 'Stop'
$roots = @($ProcessIds -split ',' | Where-Object { $_ } | ForEach-Object { [int]$_ })
function Get-Tree([int[]]$Roots) {
    $all = Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId
    $ids = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($root in $Roots) { [void]$ids.Add($root) }
    do {
        $added = $false
        foreach ($row in $all) { if ($ids.Contains([int]$row.ParentProcessId) -and $ids.Add([int]$row.ProcessId)) { $added = $true } }
    } while ($added)
    foreach ($id in $ids) { Get-Process -Id $id -ErrorAction SilentlyContinue }
}
function Snapshot { $p = @(Get-Tree $roots); [pscustomobject]@{ Count = $p.Count; Cpu = ($p | ForEach-Object { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum; Private = ($p | Measure-Object PrivateMemorySize64 -Sum).Sum } }
$before = Snapshot
$clock = [Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds $Seconds
$after = Snapshot
$clock.Stop()
$cores = [Environment]::ProcessorCount
[ordered]@{
    windows       = $roots.Count
    processes     = $after.Count
    cpuOneCorePct = [math]::Round(($after.Cpu - $before.Cpu) / $clock.Elapsed.TotalSeconds * 100, 1)
    cpuMachinePct = [math]::Round(($after.Cpu - $before.Cpu) / $clock.Elapsed.TotalSeconds / $cores * 100, 2)
    privateMB     = [math]::Round($after.Private / 1MB, 1)
    seconds       = [math]::Round($clock.Elapsed.TotalSeconds, 1)
} | ConvertTo-Json
