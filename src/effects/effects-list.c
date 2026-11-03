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
 * Lista de efectos disponibles.
 *
 * Para agregar un efecto nuevo:
 *   1. Crear src/effects/<nombre>/<nombre>.c que defina
 *      "const struct vfx_effect_info vfx_effect_<nombre>".
 *   2. Declararlo con "extern" en effects-list.h y sumarlo a la lista de abajo.
 *   3. Agregar el .c en CMakeLists.txt y sus textos en data/locale/<idioma>.ini.
 */

#include "effects/effects-list.h"

static const struct vfx_effect_info *const effects[] = {
	&vfx_effect_reverb,
	&vfx_effect_echo,
};

const struct vfx_effect_info *const *vfx_get_effects(size_t *count)
{
	if (count)
		*count = sizeof(effects) / sizeof(effects[0]);
	return effects;
}
