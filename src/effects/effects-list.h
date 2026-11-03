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

#include "core/effect.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cada efecto disponible (definidos en src/effects/<nombre>/<nombre>.c). */
extern const struct vfx_effect_info vfx_effect_reverb;
extern const struct vfx_effect_info vfx_effect_echo;

/*
 * Devuelve la lista de todos los efectos del plugin.
 * "count" recibe la cantidad de efectos.
 */
const struct vfx_effect_info *const *vfx_get_effects(size_t *count);

#ifdef __cplusplus
}
#endif
