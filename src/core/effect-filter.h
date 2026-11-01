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
 * Convierte cualquier efecto (core/effect.h) en un filtro de audio de OBS.
 *
 * Es el UNICO lugar del plugin que conecta los efectos con OBS: arma el
 * filtro, sus deslizadores en pantalla y le pasa el audio al motor
 * (core/effect-process.h).
 */

#pragma once

#include "core/effect.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registra en OBS un filtro de audio para el efecto.
 * Devuelve false (y no registra nada) si la descripcion del efecto no es valida.
 * Se llama una vez por efecto, desde obs_module_load().
 */
bool vfx_filter_register(const struct vfx_effect_info *info);

/*
 * Numero que cambia cada vez que algun filtro del plugin se crea, se borra,
 * se prende, se apaga o cambia un deslizador (desde cualquier lugar de OBS).
 * Se puede leer desde cualquier hilo.
 */
long vfx_filter_state_version(void);

/* Avisa al panel que algo cambio (por ejemplo, la tecla de un atajo). */
void vfx_filter_notify_changed(void);

#ifdef __cplusplus
}
#endif
