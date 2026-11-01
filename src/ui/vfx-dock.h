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

/*
 * Panel "Efectos de voz" dentro de la ventana de OBS (como "Salida multiple").
 *
 * Esta escrito en C++ porque usa Qt, la biblioteca de interfaz de OBS.
 * Desde C solo se ven estas dos funciones.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Crea el panel y se lo entrega a OBS. Se llama desde obs_module_load(). */
bool vfx_dock_create(void);

/* Se desconecta de OBS. Se llama desde obs_module_unload(). El panel lo borra OBS. */
void vfx_dock_shutdown(void);

#ifdef __cplusplus
}
#endif
