param(
    [string]$BuildDirectory = "build/Release",
    [string]$OutputDirectory = "out/release"
)
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
$versionMatch = [regex]::Match([IO.File]::ReadAllText((Join-Path $projectRoot "CMakeLists.txt")), 'project\(Murmur VERSION ([0-9.]+)')
if (!$versionMatch.Success) { throw "Version introuvable" }
$version = $versionMatch.Groups[1].Value
$application = Join-Path $buildRoot "Murmur.exe"
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($application).ProductVersion -ne $version) { throw "Version du binaire différente de CMake" }
$packageRoot = Join-Path $outputRoot "Murmur-$version-Windows-x64"
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
Copy-Item -LiteralPath $application -Destination $packageRoot -Force
Copy-Item -LiteralPath (Join-Path $buildRoot "licenses") -Destination $packageRoot -Recurse -Force
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
$visualStudio = & $vswhere -latest -products '*' -property installationPath
$runtime = Get-ChildItem -LiteralPath (Join-Path $visualStudio "VC/Redist/MSVC") -Directory |
    Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName "x64/Microsoft.VC143.CRT/vcruntime140.dll" } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$runtime) { throw "Runtime redistribuable x64 introuvable" }
Copy-Item -LiteralPath $runtime -Destination $packageRoot -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "CHANGELOG.md") -Destination $packageRoot -Force
$readme = @"
MURMUR $version - MESSAGES ET APPELS - WINDOWS 10/11 64 BITS

Extraire tout le ZIP puis ouvrir Murmur.exe. Garder les fichiers ensemble.
Parametres > Versions affiche l'historique et les mises a jour depuis Jxlio/Direct.
Une mise a jour signee est telechargee automatiquement au lancement, puis
installee a la fermeture. Aucun appel n'est ferme pour appliquer une mise a jour.
Les anciennes versions 0.5.x necessitent cette installation manuelle initiale.
Parametres permet de choisir le micro et la sortie, regler les volumes et tester
les peripheriques. Test solo ouvre deux clients locaux avec un audio simule :
ce mode reste silencieux et ne reproduit pas deux acces Internet.

Pour ajouter un ami : Amis > Creer un code ami. Envoyer le code en prive.
L'autre personne l'ajoute avec son nom. Un code Murmur 0.7 comprenant le service
Internet configure la connexion automatique aux lancements suivants.
Les anciens codes restent utilisables sur le reseau local. Le mode Internet
manuel reste disponible comme outil avance. Ne jamais publier les codes prives.
Certaines box necessitent un relais TURN. Aucun relais public n'est deploye.
Le partage d'ecran reste a faire. Le logiciel n'a pas ete audite ni signe Authenticode.
"@
Set-Content -LiteralPath (Join-Path $packageRoot "LIRE-MOI.txt") -Value $readme -Encoding utf8
Compress-Archive -LiteralPath $packageRoot -DestinationPath (Join-Path $outputRoot "Murmur-$version-Windows-x64.zip") -Force
Write-Output "Package Murmur $version ready."
