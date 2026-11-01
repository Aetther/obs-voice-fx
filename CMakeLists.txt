# obs-voice-fx
# Copyright (C) 2026 Gonzalo Alejandro Zalazar
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program. If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.25)

project(obs-voice-fx
    VERSION 0.1.0
    DESCRIPTION "Efectos de voz para OBS Studio con atajos de teclado"
    LANGUAGES C
)

option(VFX_BUILD_TESTS "Compilar las pruebas de audio (no necesitan OBS)" ON)

# -----------------------------------------------------------------------------
# Plataforma: por ahora solo Windows de 64 bits con el compilador de Microsoft
# -----------------------------------------------------------------------------

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "obs-voice-fx por ahora solo se compila para Windows de 64 bits.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Se necesita el compilador de Microsoft (MSVC) de Build Tools / Visual Studio.")
endif()

# -----------------------------------------------------------------------------
# Dependencias de OBS (las prepara scripts/preparar-deps.ps1)
# -----------------------------------------------------------------------------

set(OBS_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Carpeta con los headers de libobs y obs.lib")
set(OBS_INCLUDE_DIR "${OBS_DEPS_DIR}/include")
set(OBS_IMPORT_LIB "${OBS_DEPS_DIR}/lib/obs.lib")

if(NOT EXISTS "${OBS_INCLUDE_DIR}/obs-module.h" OR NOT EXISTS "${OBS_IMPORT_LIB}")
    message(FATAL_ERROR
        "Faltan las dependencias de OBS en '${OBS_DEPS_DIR}'.\n"
        "Ejecuta primero:\n"
        "  powershell -ExecutionPolicy Bypass -File scripts\\preparar-deps.ps1")
endif()

# Los headers tienen que ser de OBS 33
file(STRINGS "${OBS_INCLUDE_DIR}/obs-config.h" _obs_major_line
    REGEX "^#define LIBOBS_API_MAJOR_VER [0-9]+")
string(REGEX REPLACE "^#define LIBOBS_API_MAJOR_VER ([0-9]+).*$" "\\1" OBS_API_MAJOR "${_obs_major_line}")

if(NOT OBS_API_MAJOR EQUAL 33)
    message(FATAL_ERROR "Los headers en deps/ son de OBS ${OBS_API_MAJOR}, se esperaba OBS 33.")
endif()

# libobs como "biblioteca importada": headers + obs.lib.
# Sus headers se tratan como externos: sus advertencias no frenan nuestro build.
add_library(OBS::libobs UNKNOWN IMPORTED)
set_target_properties(OBS::libobs PROPERTIES
    IMPORTED_LOCATION "${OBS_IMPORT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
)

# -----------------------------------------------------------------------------
# Opciones generales de compilacion
# -----------------------------------------------------------------------------

# Runtime de C incluido dentro del DLL: no depende de que el usuario tenga
# instalada una version concreta de "Visual C++ Redistributable".
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Archivo .pdb con simbolos de depuracion (sirve para analizar un cierre de OBS).
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")

# Si no se indica otra carpeta, "cmake --install" deja el plugin en ./release
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/release" CACHE PATH "Carpeta de instalacion" FORCE)
endif()

# Aplica las mismas reglas estrictas y de seguridad a cualquier objetivo
# (el plugin y las pruebas se compilan exactamente igual).
function(vfx_apply_settings target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    set_target_properties(${target} PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        COMPILE_WARNING_AS_ERROR ON
    )

    target_compile_options(${target} PRIVATE
        /W4                 # nivel alto de advertencias
        /utf-8              # archivos y textos en UTF-8 (tildes, enie)
        /Zc:preprocessor    # preprocesador estandar (igual que OBS)
        /sdl                # chequeos de seguridad adicionales
        /guard:cf           # Control Flow Guard
        /GS                 # proteccion de la pila (stack cookies)
        /MP                 # compila varios archivos en paralelo
    )

    target_link_options(${target} PRIVATE
        /DEBUG              # genera el .pdb tambien en Release
        /GUARD:CF           # Control Flow Guard
        /DYNAMICBASE        # ASLR: direcciones de memoria aleatorias
        /HIGHENTROPYVA      # ASLR de 64 bits
        /NXCOMPAT           # DEP: la memoria de datos no se puede ejecutar
        /CETCOMPAT          # compatible con "shadow stack" de CPUs modernas
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>"
    )
endfunction()

# -----------------------------------------------------------------------------
# El plugin
# -----------------------------------------------------------------------------

add_library(${PROJECT_NAME} MODULE
    src/plugin-main.c
    src/effects/reverb/freeverb.c
)

vfx_apply_settings(${PROJECT_NAME})

target_link_libraries(${PROJECT_NAME} PRIVATE
    OBS::libobs
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PLUGIN_NAME="${PROJECT_NAME}"
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${PROJECT_NAME}"
)

# -----------------------------------------------------------------------------
# Pruebas (no usan OBS: prueban el procesamiento de audio por separado)
# -----------------------------------------------------------------------------

if(VFX_BUILD_TESTS)
    enable_testing()

    add_executable(test-dsp
        tests/test-dsp.c
        src/effects/reverb/freeverb.c
    )

    vfx_apply_settings(test-dsp)

    add_test(NAME test-dsp COMMAND test-dsp)
endif()

# -----------------------------------------------------------------------------
# Instalacion: formato nuevo de OBS 33
#
#   <prefijo>/obs-voice-fx/obs-voice-fx.dll
#   <prefijo>/obs-voice-fx/data/...
#
# Las pruebas no se instalan.
# -----------------------------------------------------------------------------

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${PROJECT_NAME}"
    RUNTIME DESTINATION "${PROJECT_NAME}"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
    DESTINATION "${PROJECT_NAME}/data"
)
