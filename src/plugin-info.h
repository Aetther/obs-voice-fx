/*
obs-voice-fx
Copyright (C) 2026 Gonzalo Alejandro Zalazar

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <util/base.h>

/* PLUGIN_NAME y PLUGIN_VERSION los define CMakeLists.txt a partir de project(). */
#if !defined(PLUGIN_NAME) || !defined(PLUGIN_VERSION)
#error "PLUGIN_NAME y PLUGIN_VERSION tienen que venir de CMakeLists.txt"
#endif

/*
 * Escribe en el log de OBS con el prefijo [obs-voice-fx].
 *
 * El primer argumento despues del nivel tiene que ser un texto literal entre
 * comillas: si no, no compila. Asi nunca se usa como formato un texto que
 * venga de afuera (proteccion contra errores de "format string").
 *
 * Ejemplo: obs_log(LOG_INFO, "version %s", PLUGIN_VERSION);
 */
#define obs_log(level, ...) blog(level, "[" PLUGIN_NAME "] " __VA_ARGS__)