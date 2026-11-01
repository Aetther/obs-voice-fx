<#
.SYNOPSIS
    Arma el .zip de una version de obs-voice-fx para publicarlo en GitHub.

.DESCRIPTION
    1. Verifica que no haya cambios sin commitear: el .zip tiene que salir
       exactamente de un commit publicado (asi cualquiera puede ver el codigo
       de lo que instala, como pide la GPL).
    2. Compila en Release y corre las pruebas.
    3. Instala el plugin en release/ (DLL, LICENSE, data/manifest.json, idiomas).
    4. Revisa que esten todos los archivos y que la version coincida.
    5. Crea out/obs-voice-fx-<version>-windows-x64.zip y su huella SHA-256.
    6. Crea aparte un .zip con el .pdb (sirve para analizar un cierre de OBS;
       no lo necesita el usuario).

    Antes hay que haber configurado el proyecto una vez:
        cmake -S . -B build

.PARAMETER PermitirCambios
    Empaqueta aunque haya cambios sin commitear (solo para pruebas locales;
    ese .zip NO se debe publicar).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\empaquetar.ps1

.NOTES
    obs-voice-fx
    Copyright (C) 2026 Gonzalo Alejandro Zalazar
    Licencia: GPL-2.0 o posterior (ver LICENSE).
#>

[CmdletBinding()]
param(
    [switch]$PermitirCambios
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$Raiz = Split-Path -Parent $PSScriptRoot
$Nombre = 'obs-voice-fx'
$BuildDir = Join-Path $Raiz 'build'
$ReleaseDir = Join-Path $Raiz 'release'
$OutDir = Join-Path $Raiz 'out'
$PluginDir = Join-Path $ReleaseDir $Nombre

function Write-Paso([string]$texto) {
    Write-Host ""
    Write-Host "==> $texto" -ForegroundColor Cyan
}

function Write-Ok([string]$texto) {
    Write-Host "    OK: $texto" -ForegroundColor Green
}

function Write-Aviso([string]$texto) {
    Write-Host "    AVISO: $texto" -ForegroundColor Yellow
}

# Ejecuta un programa externo y corta si falla.
function Invoke-Externo([string]$programa, [string[]]$argumentos) {
    & $programa @argumentos
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo: $programa $($argumentos -join ' ') (codigo $LASTEXITCODE)"
    }
}

# Borra una carpeta solo si esta dentro del proyecto (nunca otra cosa).
function Remove-CarpetaDelProyecto([string]$ruta) {
    if (-not (Test-Path -LiteralPath $ruta)) {
        return
    }
    $completa = (Resolve-Path -LiteralPath $ruta).Path
    $raizCompleta = (Resolve-Path -LiteralPath $Raiz).Path.TrimEnd('\') + '\'
    if (-not $completa.StartsWith($raizCompleta, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Se intento borrar una carpeta fuera del proyecto: $completa"
    }
    Remove-Item -LiteralPath $completa -Recurse -Force
}

# -----------------------------------------------------------------------------
Write-Paso "Version del proyecto"

$cmakeLists = Get-Content -LiteralPath (Join-Path $Raiz 'CMakeLists.txt') -Raw
$coincidencia = [regex]::Match($cmakeLists, 'project\(\s*obs-voice-fx\s+VERSION\s+(\d+\.\d+\.\d+)')
if (-not $coincidencia.Success) {
    throw "No se encontro 'project(obs-voice-fx VERSION x.y.z' en CMakeLists.txt"
}
$Version = $coincidencia.Groups[1].Value
$Tag = "v$Version"
Write-Ok "version $Version"

# -----------------------------------------------------------------------------
Write-Paso "Estado de git"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "No se encontro git."
}

$cambios = & git -C $Raiz status --porcelain
if ($LASTEXITCODE -ne 0) {
    throw "git status fallo (la carpeta es un repositorio?)"
}
if ($cambios) {
    if ($PermitirCambios) {
        Write-Aviso "hay cambios sin commitear: este .zip es SOLO para pruebas, no lo publiques"
    } else {
        Write-Host ($cambios -join "`n")
        throw "Hay cambios sin commitear. Hace commit primero (o usa -PermitirCambios para una prueba local)."
    }
}

$Commit = (& git -C $Raiz rev-parse --short=12 HEAD).Trim()
Write-Ok "commit $Commit"

$tagsEnHead = @(& git -C $Raiz tag --points-at HEAD)
if ($tagsEnHead -contains $Tag) {
    Write-Ok "el commit tiene la etiqueta $Tag"
} else {
    Write-Aviso "el commit todavia no tiene la etiqueta $Tag (se crea despues de probar el .zip)"
}

# -----------------------------------------------------------------------------
Write-Paso "Compilacion y pruebas"

if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
    throw "El proyecto no esta configurado. Ejecuta primero: cmake -S . -B build"
}

Invoke-Externo 'cmake' @('--build', $BuildDir, '--config', 'Release')
Invoke-Externo 'ctest' @('--test-dir', $BuildDir, '-C', 'Release', '--output-on-failure')
Write-Ok "compilado y pruebas pasadas"

# -----------------------------------------------------------------------------
Write-Paso "Instalacion limpia en release\"

Remove-CarpetaDelProyecto $ReleaseDir
Invoke-Externo 'cmake' @('--install', $BuildDir, '--config', 'Release', '--prefix', $ReleaseDir)

$necesarios = @(
    "$Nombre.dll",
    'LICENSE',
    'data\manifest.json',
    'data\locale\en-US.ini',
    'data\locale\es-ES.ini'
)
foreach ($archivo in $necesarios) {
    if (-not (Test-Path -LiteralPath (Join-Path $PluginDir $archivo) -PathType Leaf)) {
        throw "Falta $archivo en $PluginDir"
    }
}

# Solo debe existir la carpeta del plugin dentro de release\
$extras = @(Get-ChildItem -LiteralPath $ReleaseDir | Where-Object { $_.Name -ne $Nombre })
if ($extras.Count -gt 0) {
    throw "Hay archivos inesperados en release\: $($extras.Name -join ', ')"
}

$manifest = Get-Content -LiteralPath (Join-Path $PluginDir 'data\manifest.json') -Raw | ConvertFrom-Json
if ($manifest.version -ne $Version) {
    throw "manifest.json dice version '$($manifest.version)' y CMakeLists.txt '$Version'"
}
Write-Ok "archivos completos, manifest.json con version $Version"

# -----------------------------------------------------------------------------
Write-Paso "Archivo .zip"

if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

$ZipNombre = "$Nombre-$Version-windows-x64.zip"
$Zip = Join-Path $OutDir $ZipNombre
if (Test-Path -LiteralPath $Zip) {
    Remove-Item -LiteralPath $Zip -Force
}

# tar.exe (incluido en Windows 10/11) arma un .zip estandar con rutas "/".
Invoke-Externo 'tar.exe' @('-a', '-c', '-f', $Zip, '-C', $ReleaseDir, $Nombre)

# Todo lo que hay dentro tiene que estar bajo obs-voice-fx/
$entradas = @(& tar.exe -t -f $Zip)
if ($LASTEXITCODE -ne 0 -or $entradas.Count -eq 0) {
    throw "No se pudo leer el .zip recien creado"
}
foreach ($entrada in $entradas) {
    if (-not $entrada.StartsWith("$Nombre/") -or $entrada.Contains('..')) {
        throw "Entrada inesperada en el .zip: $entrada"
    }
}
if ($entradas -notcontains "$Nombre/$Nombre.dll") {
    throw "El .zip no contiene $Nombre/$Nombre.dll"
}
Write-Ok "$ZipNombre ($($entradas.Count) entradas)"

# Huella SHA-256 en el formato de sha256sum: "<huella>  <archivo>"
$Hash = (Get-FileHash -LiteralPath $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
$HashArchivo = "$Zip.sha256"
[System.IO.File]::WriteAllText($HashArchivo, "$Hash  $ZipNombre`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Ok "SHA-256 guardado en $ZipNombre.sha256"

# -----------------------------------------------------------------------------
Write-Paso "Simbolos de depuracion (.pdb)"

$Pdb = Join-Path $BuildDir "Release\$Nombre.pdb"
if (Test-Path -LiteralPath $Pdb) {
    $PdbZip = Join-Path $OutDir "$Nombre-$Version-windows-x64-pdb.zip"
    if (Test-Path -LiteralPath $PdbZip) {
        Remove-Item -LiteralPath $PdbZip -Force
    }
    Invoke-Externo 'tar.exe' @('-a', '-c', '-f', $PdbZip, '-C', (Split-Path -Parent $Pdb), "$Nombre.pdb")
    Write-Ok (Split-Path -Leaf $PdbZip)
} else {
    Write-Aviso "no se encontro $Pdb (no es obligatorio)"
}

# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "Listo. Archivos para el release $Tag en: $OutDir" -ForegroundColor Green
Write-Host ""
Write-Host "  Archivo : $ZipNombre"
Write-Host "  Commit  : $Commit"
Write-Host "  SHA-256 : $Hash"
Write-Host ""
