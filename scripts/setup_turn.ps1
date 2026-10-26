<#
.SYNOPSIS
  Gives the Murmur service its relay (TURN) secrets, then checks that the live service really hands out a relay.
.DESCRIPTION
  1. In the Cloudflare dashboard: Realtime -> TURN Server -> Create. Note the "TURN Token ID" and the "API Token".
  2. Run this script. It asks for both (typed hidden, never written to disk or shown) and passes them to
     `wrangler secret put`. Secrets apply at once; no new deployment is needed.
  Wrangler's own login cannot create TURN keys (it has no Realtime permission), which is why this step is yours.
.EXAMPLE
  .\scripts\setup_turn.ps1 -Service https://murmur.exemple.workers.dev
#>
param([Parameter(Mandatory)][string]$Service)
$ErrorActionPreference = 'Stop'
$server = Join-Path $PSScriptRoot '..\server'

function Read-Hidden([string]$Prompt) {
    $secure = Read-Host $Prompt -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}
function Put-Secret([string]$Name, [string]$Value) {
    if (-not $Value) { throw "$Name is empty." }
    Push-Location $server
    try { $Value | npx wrangler secret put $Name; if ($LASTEXITCODE -ne 0) { throw "wrangler could not set $Name." } } finally { Pop-Location }
}

Put-Secret 'TURN_KEY_ID' (Read-Hidden 'TURN Token ID')
Put-Secret 'TURN_KEY_API_TOKEN' (Read-Hidden 'TURN API Token')
Write-Host "`nChecking the live service..."
Push-Location $server
try { node test/turn_live.mjs $Service; exit $LASTEXITCODE } finally { Pop-Location }
