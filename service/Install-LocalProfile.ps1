param(
    [Parameter(Mandatory = $true)][string]$Origin,
    [Parameter(Mandatory = $true)][string]$KeyFile
)
$ErrorActionPreference = 'Stop'
if ($Origin -cnotmatch '^https://[a-z0-9.-]+\.workers\.dev$' -or $Origin.Length -gt 240) { throw 'Origine Workers HTTPS invalide.' }
$directAccess = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $KeyFile)).Trim()
if ($directAccess -cnotmatch '^[a-f0-9]{64}$') { throw 'Le fichier doit contenir un secret hexadécimal de 32 octets.' }
Add-Type -AssemblyName System.Security
$directBytes = New-Object byte[] 321
$directOrigin = [Text.Encoding]::ASCII.GetBytes($Origin)
[Array]::Copy($directOrigin, 0, $directBytes, 0, $directOrigin.Length)
[Array]::Copy([Text.Encoding]::ASCII.GetBytes($directAccess), 0, $directBytes, 256, 64)
$directFolder = Join-Path $env:LOCALAPPDATA 'Murmur'
New-Item -ItemType Directory -Path $directFolder -Force | Out-Null
$directDestination = Join-Path $directFolder 'service.dat'
if (Test-Path -LiteralPath $directDestination) {
    $directExisting = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($directDestination), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    if ([Convert]::ToBase64String($directExisting) -ne [Convert]::ToBase64String($directBytes)) { throw 'Un autre profil existe déjà. Il a été conservé.' }
    [Array]::Clear($directExisting, 0, $directExisting.Length)
} else {
    $directEncrypted = [Security.Cryptography.ProtectedData]::Protect($directBytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $directTemporary = Join-Path $directFolder 'service.dat.tmp'
    [IO.File]::WriteAllBytes($directTemporary, $directEncrypted)
    [IO.File]::Move($directTemporary, $directDestination)
}
[Array]::Clear($directBytes, 0, $directBytes.Length)
$directAccess = $null
Write-Output 'Profil de mise en relation configuré pour ce compte Windows.'
