<#
.SYNOPSIS
  Mesure RAM et CPU au repos de Direct/Murmur (processus + enfants WebView2).
.EXAMPLE
  .\scripts\measure_performance.ps1 -Output build\perf.json
#>
param(
    [string]$Exe = "$PSScriptRoot\..\out\build-latest\Murmur.exe",
    [int]$WarmupSeconds = 8,
    [int]$SampleSeconds = 20,
    [string]$Output = '',
    [string]$BrowserArgs = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "Executable introuvable : $Exe" }

function Get-Tree([int]$RootId) {
    $all = Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId
    $ids = [System.Collections.Generic.HashSet[int]]::new()
    [void]$ids.Add($RootId)
    do {
        $added = $false
        foreach ($row in $all) {
            if ($ids.Contains([int]$row.ParentProcessId) -and $ids.Add([int]$row.ProcessId)) { $added = $true }
        }
    } while ($added)
    foreach ($id in $ids) { Get-Process -Id $id -ErrorAction SilentlyContinue }
}
function Get-Snapshot([int]$RootId) {
    $procs = @(Get-Tree $RootId)
    $cpu = 0.0
    foreach ($item in $procs) { $cpu += $item.TotalProcessorTime.TotalSeconds }
    [pscustomobject]@{
        Processes  = $procs.Count
        PrivateMB  = [math]::Round(($procs | Measure-Object PrivateMemorySize64 -Sum).Sum / 1MB, 1)
        WorkingMB  = [math]::Round(($procs | Measure-Object WorkingSet64 -Sum).Sum / 1MB, 1)
        CpuSeconds = $cpu
    }
}

$startArgs = @{ FilePath = (Resolve-Path $Exe).Path; PassThru = $true }
# Lets experiments try WebView2 flags without rebuilding.
if ($BrowserArgs) { $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = $BrowserArgs }
$process = Start-Process @startArgs
Remove-Item Env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS -ErrorAction SilentlyContinue
try {
    Start-Sleep -Seconds $WarmupSeconds
    $before = Get-Snapshot $process.Id
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $SampleSeconds
    $after = Get-Snapshot $process.Id
    $clock.Stop()
    $cores = [Environment]::ProcessorCount
    $cpuPercent = [math]::Round((($after.CpuSeconds - $before.CpuSeconds) / $clock.Elapsed.TotalSeconds / $cores) * 100, 3)
    $result = [ordered]@{
        date       = (Get-Date -Format 's')
        browserArgs = $BrowserArgs
        processes  = $after.Processes
        privateMB  = $after.PrivateMB
        workingMB  = $after.WorkingMB
        idleCpuPct = $cpuPercent
        sampleSec  = [math]::Round($clock.Elapsed.TotalSeconds, 1)
    }
    $json = $result | ConvertTo-Json
    if ($Output) { $json | Set-Content -Encoding utf8 $Output }
    $json
} finally {
    Get-Tree $process.Id | Sort-Object Id -Descending |
        ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
}
