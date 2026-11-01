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

#include <obs-module.h>

#include "core/effect-control.h"
#include "core/effect-filter.h"
#include "effects/effects-list.h"
#include "plugin-info.h"
#include "ui/vfx-dock.h"

/* Funciones obligatorias que OBS busca en todo plugin (puntero al modulo, version de la API). */
OBS_DECLARE_MODULE()

/* Textos traducidos: se leen de data/locale/<idioma>.ini, con inglés como respaldo. */
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

OBS_MODULE_AUTHOR("Gonzalo Alejandro Zalazar")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS Voice FX";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}

/* OBS llama a esta función una vez, al arrancar. Si devuelve false, el plugin no se carga. */
bool obs_module_load(void)
{
	size_t count = 0;
	const struct vfx_effect_info *const *effects = vfx_get_effects(&count);
	size_t registered = 0;

	/* Un filtro de OBS por cada efecto de la lista. */
	for (size_t i = 0; i < count; i++) {
		if (vfx_filter_register(effects[i]))
			registered++;
	}

	/* Un atajo de teclado por efecto (Ajustes -> Atajos). */
	vfx_control_init(effects, count);

	/* Panel "Efectos de voz". Si no se pudiera crear, el resto sigue funcionando. */
	vfx_dock_create();

	obs_log(LOG_INFO, "plugin cargado (version %s): %zu de %zu efectos disponibles", PLUGIN_VERSION, registered,
		count);
	return true;
}

/* OBS llama a esta función una vez, al cerrarse. */
void obs_module_unload(void)
{
	vfx_dock_shutdown();
	vfx_control_shutdown();
	obs_log(LOG_INFO, "plugin descargado");
}
