<#
.SYNOPSIS
    Prepara las dependencias para compilar obs-voice-fx en Windows.

.DESCRIPTION
    1. Busca OBS Studio instalado y verifica version y firma digital.
    2. Busca lib.exe de Visual Studio / Build Tools.
    3. Descarga SOLO los headers publicos de libobs (OBS 33.0.1) desde el
       repositorio oficial, pidiendo exactamente el commit de la version
       oficial y verificandolo.
    4. Crea obsconfig.h (OBS lo genera al compilarse a si mismo).
    5. Lee la tabla de funciones exportadas de tu obs.dll y genera obs.lib.

    Todo queda en la carpeta deps/ (no se sube a GitHub).

.PARAMETER ObsDir
    Carpeta de instalacion de OBS. Si no se indica, se busca sola.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\preparar-deps.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\preparar-deps.ps1 -ObsDir "D:\obs-studio"

.NOTES
    obs-voice-fx
    Copyright (C) 2026 Gonzalo Alejandro Zalazar
    Licencia: GPL-2.0 o posterior (ver LICENSE).
#>

[CmdletBinding()]
param(
    [string]$ObsDir = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# Configuracion
# ---------------------------------------------------------------------------

$ObsVersion = '33.0.1'
# Commit oficial al que apunta el tag 33.0.1 de obsproject/obs-studio.
# Se descarga exactamente este commit y se verifica; si no coincide, se cancela.
$ObsCommit  = '65c320b46e1af5c7a555161aea6182cdf7a8c524'
$ObsRepo    = 'https://github.com/obsproject/obs-studio.git'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$DepsDir     = Join-Path $ProjectRoot 'deps'
$TempSrcDir  = Join-Path $DepsDir 'obs-src-temp'
$IncludeDir  = Join-Path $DepsDir 'include'
$LibDir      = Join-Path $DepsDir 'lib'

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

function Invoke-Git {
    # Ejecuta git sin mensajes de ayuda y corta si falla
    & git -c advice.detachedHead=false @args
    if ($LASTEXITCODE -ne 0) { throw "Fallo: git $($args -join ' ')" }
}

function Get-FuncionesExportadas([string]$rutaDll) {
    # Lee directamente la tabla de exportaciones del archivo DLL (formato PE de Windows).
    # No depende del texto que imprime ninguna herramienta.
    $b = [System.IO.File]::ReadAllBytes($rutaDll)

    function U16([long]$o) {
        if ($o -lt 0 -or $o + 2 -gt $b.Length) { throw "Lectura fuera del archivo" }
        return [long][System.BitConverter]::ToUInt16($b, [int]$o)
    }
    function U32([long]$o) {
        if ($o -lt 0 -or $o + 4 -gt $b.Length) { throw "Lectura fuera del archivo" }
        return [long][System.BitConverter]::ToUInt32($b, [int]$o)
    }

    if ((U16 0) -ne 0x5A4D) { throw "No es un ejecutable de Windows valido" }
    $pe = U32 0x3C
    if ((U32 $pe) -ne 0x4550) { throw "Firma PE invalida" }

    $coff        = $pe + 4
    $maquina     = U16 $coff
    $nSecciones  = U16 ($coff + 2)
    $tamOpcional = U16 ($coff + 16)
    $opcional    = $coff + 20

    if ($maquina -ne 0x8664) { throw "El DLL no es de 64 bits (x64)" }
    if ((U16 $opcional) -ne 0x20B) { throw "Formato PE32+ esperado" }

    $rvaExport = U32 ($opcional + 112)
    if ($rvaExport -eq 0) { throw "El DLL no tiene tabla de exportaciones" }

    $tablaSecciones = $opcional + $tamOpcional
    $secciones = @()
    for ($i = 0; $i -lt $nSecciones; $i++) {
        $s = $tablaSecciones + ($i * 40)
        $secciones += [pscustomobject]@{
            Va      = U32 ($s + 12)
            TamVirt = U32 ($s + 8)
            TamRaw  = U32 ($s + 16)
            PtrRaw  = U32 ($s + 20)
        }
    }

    function RvaAOffset([long]$rva) {
        foreach ($sec in $secciones) {
            $tam = [Math]::Max($sec.TamVirt, $sec.TamRaw)
            if ($rva -ge $sec.Va -and $rva -lt ($sec.Va + $tam)) {
                return $rva - $sec.Va + $sec.PtrRaw
            }
        }
        throw "Direccion RVA fuera de las secciones"
    }

    $dirExport = RvaAOffset $rvaExport
    $cantNombres = U32 ($dirExport + 24)
    $tablaNombres = RvaAOffset (U32 ($dirExport + 32))

    $nombres = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $cantNombres; $i++) {
        $p = RvaAOffset (U32 ($tablaNombres + (4 * $i)))
        $fin = [Array]::IndexOf($b, [byte]0, [int]$p)
        if ($fin -lt 0) { throw "Nombre de funcion sin terminar" }
        $nombre = [System.Text.Encoding]::ASCII.GetString($b, [int]$p, [int]($fin - $p))
        # Solo nombres validos de C: descarta cualquier cosa extrana
        if ($nombre -match '^[A-Za-z_][A-Za-z0-9_]*$') {
            $nombres.Add($nombre)
        }
    }
    return ,$nombres
}

# ---------------------------------------------------------------------------
# 1. Buscar OBS instalado
# ---------------------------------------------------------------------------

Write-Paso "Buscando OBS Studio instalado"

if ($ObsDir -eq "") {
    $reg = Get-ItemProperty -Path 'HKLM:\SOFTWARE\OBS Studio' -ErrorAction SilentlyContinue
    if ($null -ne $reg -and $reg.PSObject.Properties.Name -contains '(default)' -and $reg.'(default)') {
        $ObsDir = $reg.'(default)'
    } else {
        $ObsDir = Join-Path $env:ProgramFiles 'obs-studio'
    }
}

$ObsBin = Join-Path $ObsDir 'bin\64bit'
$ObsDll = Join-Path $ObsBin 'obs.dll'
$ObsExe = Join-Path $ObsBin 'obs64.exe'

if (-not (Test-Path $ObsDll)) {
    throw "No se encontro obs.dll en '$ObsBin'. Indica la carpeta de OBS con -ObsDir `"C:\ruta\a\obs-studio`""
}
Write-Ok "OBS encontrado en $ObsDir"

# Version instalada: tiene que ser de la misma serie que los headers (33.0.x)
$versionInstalada = (Get-Item $ObsExe).VersionInfo.ProductVersion
if (-not $versionInstalada) { $versionInstalada = 'desconocida' }

$serie = ($ObsVersion -split '\.')[0..1] -join '.'
if ($versionInstalada.StartsWith("$serie.")) {
    Write-Ok "Version instalada: $versionInstalada (headers: $ObsVersion)"
} else {
    throw "La version de OBS instalada es $versionInstalada, pero este proyecto usa OBS $ObsVersion. Actualiza OBS desde obsproject.com (o indica otra instalacion con -ObsDir)."
}

# Firma digital: confirma que obs.dll es el original de OBS y no fue modificado
$firma = Get-AuthenticodeSignature -FilePath $ObsDll
if ($firma.Status -eq 'Valid') {
    Write-Ok "obs.dll tiene firma digital valida: $($firma.SignerCertificate.Subject)"
} else {
    Write-Aviso "obs.dll no tiene una firma digital valida (estado: $($firma.Status)). Si no lo modificaste vos, reinstala OBS desde obsproject.com."
}

# ---------------------------------------------------------------------------
# 2. Buscar lib.exe (Visual Studio / Build Tools)
# ---------------------------------------------------------------------------

Write-Paso "Buscando herramientas de compilacion"

$mensajeInstalar = "Instala 'Build Tools para Visual Studio' (o Visual Studio Community) con la carga de trabajo 'Desarrollo para el escritorio con C++'."

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "No se encontro ninguna instalacion de Visual Studio ni Build Tools. $mensajeInstalar"
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Hay una instalacion de Visual Studio, pero le falta el compilador de C++. $mensajeInstalar"
}

$verFile = Join-Path $vsPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
$msvcVer = (Get-Content $verFile -Raw).Trim()
$msvcBin = Join-Path $vsPath "VC\Tools\MSVC\$msvcVer\bin\Hostx64\x64"
$libExe  = Join-Path $msvcBin 'lib.exe'

if (-not (Test-Path $libExe)) {
    throw "No se encontro $libExe. $mensajeInstalar"
}
Write-Ok "MSVC $msvcVer en $vsPath"

# ---------------------------------------------------------------------------
# 3. Descargar headers de libobs (solo la carpeta libobs del commit oficial)
# ---------------------------------------------------------------------------

Write-Paso "Descargando headers de libobs $ObsVersion"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "No se encontro git. Instalalo desde git-scm.com."
}

if (Test-Path $TempSrcDir) { Remove-Item -Recurse -Force $TempSrcDir }
if (Test-Path $IncludeDir) { Remove-Item -Recurse -Force $IncludeDir }
New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null

# Confirma que el tag oficial sigue apuntando al commit esperado
$tagRemoto = & git ls-remote $ObsRepo "refs/tags/$ObsVersion^{}"
if ($LASTEXITCODE -ne 0 -or -not $tagRemoto) { throw "No se pudo consultar el tag $ObsVersion en el repositorio oficial." }
$commitDelTag = ($tagRemoto -split '\s+')[0]
if ($commitDelTag -ne $ObsCommit) {
    throw "El tag oficial $ObsVersion apunta a $commitDelTag y no al commit esperado $ObsCommit. Se cancelo por seguridad."
}
Write-Ok "El tag oficial $ObsVersion apunta al commit esperado"

# Descarga exactamente ese commit, solo la carpeta libobs
Invoke-Git init --quiet $TempSrcDir
Invoke-Git -C $TempSrcDir remote add origin $ObsRepo
Invoke-Git -C $TempSrcDir sparse-checkout set --cone libobs
Invoke-Git -C $TempSrcDir fetch --quiet --depth 1 --filter=blob:none origin $ObsCommit
Invoke-Git -C $TempSrcDir checkout --quiet FETCH_HEAD

$commitDescargado = (& git -C $TempSrcDir rev-parse HEAD).Trim()
if ($commitDescargado -ne $ObsCommit) {
    Remove-Item -Recurse -Force $TempSrcDir
    throw "El commit descargado ($commitDescargado) no coincide con el oficial ($ObsCommit). Se cancelo por seguridad."
}
Write-Ok "Commit verificado: $commitDescargado"

# Copiar solo los .h, respetando la estructura de carpetas
$libobsDir = Join-Path $TempSrcDir 'libobs'
$cantidad = 0
Get-ChildItem -Path $libobsDir -Recurse -Filter '*.h' | ForEach-Object {
    $relativo = $_.FullName.Substring($libobsDir.Length + 1)
    $destino  = Join-Path $IncludeDir $relativo
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destino) | Out-Null
    Copy-Item -Path $_.FullName -Destination $destino
    $cantidad++
}

Remove-Item -Recurse -Force $TempSrcDir
Write-Ok "$cantidad headers copiados a deps\include"

# ---------------------------------------------------------------------------
# 4. Crear obsconfig.h
# ---------------------------------------------------------------------------

Write-Paso "Creando obsconfig.h"

$obsconfig = @(
    '#pragma once',
    '',
    '/* Generado por scripts/preparar-deps.ps1.',
    ' * OBS crea este archivo al compilarse a si mismo; un plugin solo',
    ' * necesita estas definiciones para que los headers compilen. */',
    '',
    '#define OBS_RELEASE_CANDIDATE 0',
    '#define OBS_BETA 0'
)
Set-Content -Path (Join-Path $IncludeDir 'obsconfig.h') -Value $obsconfig -Encoding Ascii
Write-Ok "deps\include\obsconfig.h"

# ---------------------------------------------------------------------------
# 5. Generar obs.lib desde obs.dll
# ---------------------------------------------------------------------------

Write-Paso "Generando obs.lib desde tu obs.dll"

New-Item -ItemType Directory -Force -Path $LibDir | Out-Null

$funciones = Get-FuncionesExportadas $ObsDll

if ($funciones.Count -lt 100) {
    throw "Se encontraron muy pocas funciones exportadas ($($funciones.Count)). obs.dll podria estar danado."
}

# Funciones que el plugin va a necesitar: si falta alguna, avisamos ya
$necesarias = @('obs_register_source_s', 'obs_hotkey_register_frontend', 'obs_module_get_config_path', 'blogva')
foreach ($f in $necesarias) {
    if (-not $funciones.Contains($f)) {
        throw "obs.dll no exporta '$f'. Verifica la version de OBS."
    }
}

$defPath = Join-Path $LibDir 'obs.def'
$defContenido = @('LIBRARY "obs.dll"', 'EXPORTS') + ($funciones | ForEach-Object { "    $_" })
Set-Content -Path $defPath -Value $defContenido -Encoding Ascii

$libPath = Join-Path $LibDir 'obs.lib'
& $libExe /NOLOGO /DEF:$defPath /MACHINE:X64 /OUT:$libPath | Out-Null
if ($LASTEXITCODE -ne 0) { throw "lib.exe no pudo generar obs.lib." }

Write-Ok "$($funciones.Count) funciones -> deps\lib\obs.lib"

# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "Listo. Dependencias preparadas en: $DepsDir" -ForegroundColor Green