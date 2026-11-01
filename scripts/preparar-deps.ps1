<#
.SYNOPSIS
    Prepara las dependencias para compilar obs-voice-fx en Windows.

.DESCRIPTION
    1. Busca OBS Studio instalado y verifica version y firma digital.
    2. Busca lib.exe de Visual Studio / Build Tools.
    3. Descarga SOLO los headers publicos de libobs y de obs-frontend-api
       (OBS 33.0.1) desde el repositorio oficial, pidiendo exactamente el
       commit de la version oficial y verificandolo.
    4. Crea obsconfig.h (OBS lo genera al compilarse a si mismo).
    5. Genera obs.lib y obs-frontend-api.lib a partir de los DLL de tu OBS.
    6. Descarga Qt 6 (el paquete oficial que usa OBS 33.0.1, de obs-deps),
       verifica su huella SHA-256 y lo descomprime.

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
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# Configuracion
# ---------------------------------------------------------------------------

$ObsVersion = '33.0.1'
# Commit oficial al que apunta el tag 33.0.1 de obsproject/obs-studio.
# Se descarga exactamente este commit y se verifica; si no coincide, se cancela.
$ObsCommit  = '65c320b46e1af5c7a555161aea6182cdf7a8c524'
$ObsRepo    = 'https://github.com/obsproject/obs-studio.git'

# Qt 6 que usa OBS 33.0.1 (datos de CMakePresets.json de esa version de OBS).
$QtSerie   = '6.11'
$QtDepsVer = '2026-10-23'
$QtZipName = "windows-deps-qt6-$QtDepsVer-x64.zip"
$QtUrl     = "https://github.com/obsproject/obs-deps/releases/download/$QtDepsVer/$QtZipName"
$QtSha256  = '78cd7a138464bb6edcdd3b85fcd63a5a7c69e369629963f3bc395bab74fb3f47'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$DepsDir     = Join-Path $ProjectRoot 'deps'
$TempSrcDir  = Join-Path $DepsDir 'obs-src-temp'
$IncludeDir  = Join-Path $DepsDir 'include'
$LibDir      = Join-Path $DepsDir 'lib'
$DownloadDir = Join-Path $DepsDir 'downloads'
$QtDir       = Join-Path $DepsDir 'qt6'

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

function New-ImportLib([string]$rutaDll, [string[]]$necesarias, [int]$minimo) {
    # Genera <nombre>.lib a partir de las funciones exportadas por el DLL.
    $nombreDll = [System.IO.Path]::GetFileName($rutaDll)
    $base      = [System.IO.Path]::GetFileNameWithoutExtension($rutaDll)

    $funciones = Get-FuncionesExportadas $rutaDll
    if ($funciones.Count -lt $minimo) {
        throw "Se encontraron muy pocas funciones exportadas en $nombreDll ($($funciones.Count)). El archivo podria estar danado."
    }

    foreach ($f in $necesarias) {
        if (-not $funciones.Contains($f)) {
            throw "$nombreDll no exporta '$f'. Verifica la version de OBS."
        }
    }

    $defPath = Join-Path $LibDir "$base.def"
    $defContenido = @("LIBRARY `"$nombreDll`"", 'EXPORTS') + ($funciones | ForEach-Object { "    $_" })
    Set-Content -Path $defPath -Value $defContenido -Encoding Ascii

    $libPath = Join-Path $LibDir "$base.lib"
    & $libExe /NOLOGO /DEF:$defPath /MACHINE:X64 /OUT:$libPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib.exe no pudo generar $base.lib." }

    Write-Ok "$($funciones.Count) funciones -> deps\lib\$base.lib"
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

$ObsBin         = Join-Path $ObsDir 'bin\64bit'
$ObsDll         = Join-Path $ObsBin 'obs.dll'
$ObsFrontendDll = Join-Path $ObsBin 'obs-frontend-api.dll'
$ObsExe         = Join-Path $ObsBin 'obs64.exe'
$ObsQtCoreDll   = Join-Path $ObsBin 'Qt6Core.dll'

foreach ($archivo in @($ObsDll, $ObsFrontendDll, $ObsExe, $ObsQtCoreDll)) {
    if (-not (Test-Path $archivo)) {
        throw "No se encontro '$archivo'. Indica la carpeta de OBS con -ObsDir `"C:\ruta\a\obs-studio`""
    }
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

# Qt de OBS: el panel se compila contra la misma serie de Qt que trae OBS
$qtInstalado = (Get-Item $ObsQtCoreDll).VersionInfo.FileVersion
if ($qtInstalado -and $qtInstalado.StartsWith("$QtSerie.")) {
    Write-Ok "Qt de OBS: $qtInstalado"
} else {
    throw "OBS trae Qt '$qtInstalado', pero este proyecto se compila con Qt $QtSerie. Verifica la version de OBS."
}

# Firma digital: confirma que los DLL son los originales de OBS y no fueron modificados
foreach ($dll in @($ObsDll, $ObsFrontendDll)) {
    $firma = Get-AuthenticodeSignature -FilePath $dll
    $nombre = [System.IO.Path]::GetFileName($dll)
    if ($firma.Status -eq 'Valid') {
        Write-Ok "$nombre tiene firma digital valida: $($firma.SignerCertificate.Subject)"
    } else {
        Write-Aviso "$nombre no tiene una firma digital valida (estado: $($firma.Status)). Si no lo modificaste vos, reinstala OBS desde obsproject.com."
    }
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
# 3. Descargar headers de libobs y obs-frontend-api (del commit oficial)
# ---------------------------------------------------------------------------

Write-Paso "Descargando headers de OBS $ObsVersion"

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

# Descarga exactamente ese commit, solo las carpetas necesarias
Invoke-Git init --quiet $TempSrcDir
Invoke-Git -C $TempSrcDir remote add origin $ObsRepo
Invoke-Git -C $TempSrcDir sparse-checkout set --cone libobs frontend/api
Invoke-Git -C $TempSrcDir fetch --quiet --depth 1 --filter=blob:none origin $ObsCommit
Invoke-Git -C $TempSrcDir checkout --quiet FETCH_HEAD

$commitDescargado = (& git -C $TempSrcDir rev-parse HEAD).Trim()
if ($commitDescargado -ne $ObsCommit) {
    Remove-Item -Recurse -Force $TempSrcDir
    throw "El commit descargado ($commitDescargado) no coincide con el oficial ($ObsCommit). Se cancelo por seguridad."
}
Write-Ok "Commit verificado: $commitDescargado"

# Copiar solo los .h de libobs, respetando la estructura de carpetas
$libobsDir = Join-Path $TempSrcDir 'libobs'
$cantidad = 0
Get-ChildItem -Path $libobsDir -Recurse -Filter '*.h' | ForEach-Object {
    $relativo = $_.FullName.Substring($libobsDir.Length + 1)
    $destino  = Join-Path $IncludeDir $relativo
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destino) | Out-Null
    Copy-Item -Path $_.FullName -Destination $destino
    $cantidad++
}

# Header de obs-frontend-api (para el panel)
$frontendHeader = Join-Path $TempSrcDir 'frontend\api\obs-frontend-api.h'
if (-not (Test-Path $frontendHeader)) { throw "No se encontro obs-frontend-api.h en el codigo de OBS." }
Copy-Item -Path $frontendHeader -Destination (Join-Path $IncludeDir 'obs-frontend-api.h')
$cantidad++

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
# 5. Generar obs.lib y obs-frontend-api.lib desde los DLL de OBS
# ---------------------------------------------------------------------------

Write-Paso "Generando bibliotecas de importacion desde tu OBS"

New-Item -ItemType Directory -Force -Path $LibDir | Out-Null

New-ImportLib -rutaDll $ObsDll -minimo 100 -necesarias @(
    'obs_register_source_s', 'obs_hotkey_register_frontend', 'obs_hotkey_load_bindings',
    'obs_module_get_config_path', 'obs_source_get_type_data', 'blog'
)

New-ImportLib -rutaDll $ObsFrontendDll -minimo 20 -necesarias @(
    'obs_frontend_add_dock_by_id', 'obs_frontend_remove_dock',
    'obs_frontend_add_event_callback', 'obs_frontend_remove_event_callback'
)

# ---------------------------------------------------------------------------
# 6. Qt 6 (paquete oficial de obs-deps, verificado por SHA-256)
# ---------------------------------------------------------------------------

Write-Paso "Preparando Qt $QtSerie (paquete oficial de OBS $QtDepsVer)"

$qtMarca = Join-Path $QtDir '.verificado-sha256'
$qtListo = (Test-Path $qtMarca) -and ((Get-Content $qtMarca -Raw).Trim() -eq $QtSha256) -and
           (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6\Qt6Config.cmake'))

if ($qtListo) {
    Write-Ok "Qt ya estaba preparado y verificado en deps\qt6"
} else {
    New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
    $qtZip = Join-Path $DownloadDir $QtZipName

    # Si ya se descargo antes y la huella coincide, no se baja de nuevo
    $descargar = $true
    if (Test-Path $qtZip) {
        $hashExistente = (Get-FileHash -Algorithm SHA256 -Path $qtZip).Hash.ToLowerInvariant()
        if ($hashExistente -eq $QtSha256) {
            $descargar = $false
            Write-Ok "Usando descarga anterior (huella correcta)"
        } else {
            Remove-Item -Force $qtZip
        }
    }

    if ($descargar) {
        Write-Host "    Descargando $QtZipName (~260 MB, puede tardar unos minutos)..."
        & curl.exe --fail --location --silent --show-error --proto '=https' --tlsv1.2 --output $qtZip $QtUrl
        if ($LASTEXITCODE -ne 0) { throw "Fallo la descarga de Qt desde $QtUrl" }
    }

    $hashDescargado = (Get-FileHash -Algorithm SHA256 -Path $qtZip).Hash.ToLowerInvariant()
    if ($hashDescargado -ne $QtSha256) {
        Remove-Item -Force $qtZip
        throw "La huella SHA-256 del paquete de Qt no coincide con la oficial. Se borro la descarga y se cancelo por seguridad."
    }
    Write-Ok "Huella SHA-256 verificada: $hashDescargado"

    if (Test-Path $QtDir) { Remove-Item -Recurse -Force $QtDir }
    New-Item -ItemType Directory -Force -Path $QtDir | Out-Null

    Write-Host "    Descomprimiendo (puede tardar un par de minutos)..."
    & tar.exe -x -f $qtZip -C $QtDir
    if ($LASTEXITCODE -ne 0) { throw "Fallo la descompresion de Qt." }

    if (-not (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6\Qt6Config.cmake'))) {
        throw "El paquete de Qt no tiene la estructura esperada."
    }

    Set-Content -Path $qtMarca -Value $QtSha256 -Encoding Ascii
    Write-Ok "Qt listo en deps\qt6"
}

# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "Listo. Dependencias preparadas en: $DepsDir" -ForegroundColor Green
