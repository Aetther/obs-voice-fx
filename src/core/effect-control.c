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
 * Seguridad:
 *   - Nunca se guardan punteros a fuentes o filtros: cada vez que se necesitan
 *     se buscan de nuevo y se toma una referencia de OBS mientras se usan.
 *     Asi, si una fuente se borra, no queda ningun puntero "colgado".
 *   - El archivo de teclas se lee con el lector de JSON de OBS; si esta danado,
 *     OBS usa la copia de respaldo o simplemente no se cargan teclas.
 *   - Se escribe con el guardado seguro de OBS (archivo temporal + respaldo).
 */

#include "core/effect-control.h"
#include "core/effect-filter.h"
#include "core/safety.h"
#include "plugin-info.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <string.h>

#define VFX_MAX_EFFECTS 32
#define HOTKEYS_FILE "hotkeys.json"

struct effect_entry {
	const struct vfx_effect_info *info;
	obs_hotkey_id hotkey;
};

static struct effect_entry g_entries[VFX_MAX_EFFECTS];
static size_t g_count = 0;
static bool g_loading = false; /* evita guardar mientras se cargan las teclas */

/* ------------------------------------------------------------------------- */
/* Buscar los filtros de un efecto                                            */

/* Un filtro del efecto y la fuente que lo tiene (las dos con referencia tomada). */
struct found_filter {
	obs_source_t *parent;
	obs_source_t *filter;
};

struct filter_list {
	const char *id;
	struct found_filter *items; /* referencias tomadas: hay que liberarlas */
	size_t num;
	size_t capacity;
};

static void list_push(struct filter_list *list, obs_source_t *parent, obs_source_t *filter)
{
	if (list->num == list->capacity) {
		size_t new_capacity = list->capacity ? list->capacity * 2 : 8;
		list->items = brealloc(list->items, new_capacity * sizeof(*list->items));
		list->capacity = new_capacity;
	}
	list->items[list->num].parent = parent;
	list->items[list->num].filter = filter;
	list->num++;
}

static void collect_filter(obs_source_t *parent, obs_source_t *filter, void *param)
{
	struct filter_list *list = param;

	const char *id = obs_source_get_unversioned_id(filter);
	if (!id || strcmp(id, list->id) != 0)
		return;

	/* Referencias "seguras": devuelven NULL si la fuente o el filtro se estan borrando. */
	obs_source_t *parent_ref = obs_source_get_ref(parent);
	obs_source_t *filter_ref = obs_source_get_ref(filter);

	if (parent_ref && filter_ref) {
		list_push(list, parent_ref, filter_ref);
	} else {
		obs_source_release(parent_ref);
		obs_source_release(filter_ref);
	}
}

static bool collect_source(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, collect_filter, param);
	return true;
}

static void filters_find(const struct vfx_effect_info *info, struct filter_list *list)
{
	memset(list, 0, sizeof(*list));
	list->id = info->id;
	obs_enum_sources(collect_source, list);
}

static void filters_release(struct filter_list *list)
{
	for (size_t i = 0; i < list->num; i++) {
		obs_source_release(list->items[i].filter);
		obs_source_release(list->items[i].parent);
	}
	bfree(list->items);
	memset(list, 0, sizeof(*list));
}

/* ------------------------------------------------------------------------- */
/* Prender / apagar                                                           */

size_t vfx_control_count_filters(const struct vfx_effect_info *info)
{
	struct filter_list list;
	filters_find(info, &list);
	size_t count = list.num;
	filters_release(&list);
	return count;
}

bool vfx_control_is_enabled(const struct vfx_effect_info *info)
{
	struct filter_list list;
	filters_find(info, &list);

	bool enabled = false;
	for (size_t i = 0; i < list.num && !enabled; i++)
		enabled = obs_source_enabled(list.items[i].filter);

	filters_release(&list);
	return enabled;
}

void vfx_control_set_enabled(const struct vfx_effect_info *info, bool enabled)
{
	struct filter_list list;
	filters_find(info, &list);

	for (size_t i = 0; i < list.num; i++)
		obs_source_set_enabled(list.items[i].filter, enabled);

	if (list.num == 0)
		obs_log(LOG_INFO, "'%s': no esta agregado a ninguna fuente de audio", info->id);
	else
		obs_log(LOG_INFO, "'%s' %s en %zu fuente(s)", info->id, enabled ? "activado" : "desactivado",
			list.num);

	filters_release(&list);
}

void vfx_control_toggle(const struct vfx_effect_info *info)
{
	vfx_control_set_enabled(info, !vfx_control_is_enabled(info));
}

/* ------------------------------------------------------------------------- */
/* Fuente de audio del efecto (para el panel)                                 */

bool vfx_control_is_audio_source(obs_source_t *source)
{
	if (!source)
		return false;
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT)
		return false;
	return (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
}

char *vfx_control_get_source_name(const struct vfx_effect_info *info, size_t *count)
{
	struct filter_list list;
	filters_find(info, &list);

	char *name = NULL;
	if (list.num > 0) {
		const char *n = obs_source_get_name(list.items[0].parent);
		name = bstrdup(n ? n : "");
	}

	if (count)
		*count = list.num;

	filters_release(&list);
	return name;
}

/* Nombre para el filtro nuevo que no choque con otro filtro de la misma fuente. */
static char *unique_filter_name(obs_source_t *target, const char *base)
{
	struct dstr name = {0};
	dstr_copy(&name, base);

	for (int n = 2; n < 100; n++) {
		obs_source_t *existing = obs_source_get_filter_by_name(target, name.array);
		if (!existing)
			break;
		obs_source_release(existing);
		dstr_printf(&name, "%s %d", base, n);
	}

	return name.array; /* el que llama lo libera con bfree */
}

bool vfx_control_attach(const struct vfx_effect_info *info, const char *source_name)
{
	obs_source_t *target = NULL;

	if (source_name && source_name[0]) {
		target = obs_get_source_by_name(source_name);
		if (!target) {
			obs_log(LOG_WARNING, "no existe la fuente elegida para '%s'", info->id);
			return false;
		}
		if (!vfx_control_is_audio_source(target)) {
			obs_log(LOG_WARNING, "la fuente elegida para '%s' no es una fuente de audio", info->id);
			obs_source_release(target);
			return false;
		}
	}

	struct filter_list list;
	filters_find(info, &list);

	/* Se conservan la configuracion y el estado del filtro que ya existia. */
	obs_data_t *settings = NULL;
	bool enabled = true;
	bool target_has_it = false;

	for (size_t i = 0; i < list.num; i++) {
		if (!settings) {
			settings = obs_source_get_settings(list.items[i].filter);
			enabled = obs_source_enabled(list.items[i].filter);
		}
		if (list.items[i].parent == target)
			target_has_it = true;
	}

	/* Se quita de todas las demas fuentes (el efecto queda en una sola). */
	for (size_t i = 0; i < list.num; i++) {
		if (list.items[i].parent != target)
			obs_source_filter_remove(list.items[i].parent, list.items[i].filter);
	}

	bool ok = true;

	if (target && !target_has_it) {
		char *name = unique_filter_name(target, obs_module_text(info->name_key));
		obs_source_t *filter = obs_source_create(info->id, name, settings, NULL);
		bfree(name);

		if (filter) {
			obs_source_set_enabled(filter, enabled);
			obs_source_filter_add(target, filter);
			obs_source_release(filter);
			obs_log(LOG_INFO, "'%s' agregado a una fuente de audio", info->id);
		} else {
			obs_log(LOG_WARNING, "no se pudo crear el filtro '%s'", info->id);
			ok = false;
		}
	} else if (!target) {
		obs_log(LOG_INFO, "'%s' quitado de todas las fuentes", info->id);
	}

	obs_data_release(settings);
	filters_release(&list);
	obs_source_release(target);
	return ok;
}

/* ------------------------------------------------------------------------- */
/* Deslizadores (para el panel)                                               */

bool vfx_control_get_params(const struct vfx_effect_info *info, float *values)
{
	struct filter_list list;
	filters_find(info, &list);

	obs_data_t *settings = list.num > 0 ? obs_source_get_settings(list.items[0].filter) : NULL;

	for (size_t i = 0; i < info->num_params; i++) {
		const struct vfx_param_info *param = &info->params[i];
		double v = settings ? obs_data_get_double(settings, param->key) : param->default_value;
		values[i] = vfx_param_clamp(v, param->min, param->max, param->default_value);
	}

	bool found = settings != NULL;
	obs_data_release(settings);
	filters_release(&list);
	return found;
}

void vfx_control_set_param(const struct vfx_effect_info *info, size_t index, double value)
{
	if (index >= info->num_params)
		return;

	const struct vfx_param_info *param = &info->params[index];
	double safe = vfx_param_clamp(value, param->min, param->max, param->default_value);

	struct filter_list list;
	filters_find(info, &list);

	obs_data_t *changes = obs_data_create();
	obs_data_set_double(changes, param->key, safe);

	/* Igual que mover el deslizador en la ventana de filtros de OBS. */
	for (size_t i = 0; i < list.num; i++)
		obs_source_update(list.items[i].filter, changes);

	obs_data_release(changes);
	filters_release(&list);
}

/* ------------------------------------------------------------------------- */
/* Atajos                                                                     */

static struct effect_entry *entry_for_info(const struct vfx_effect_info *info)
{
	for (size_t i = 0; i < g_count; i++) {
		if (g_entries[i].info == info)
			return &g_entries[i];
	}
	return NULL;
}

static struct effect_entry *entry_for_hotkey(obs_hotkey_id id)
{
	for (size_t i = 0; i < g_count; i++) {
		if (g_entries[i].hotkey == id)
			return &g_entries[i];
	}
	return NULL;
}

obs_hotkey_id vfx_control_get_hotkey(const struct vfx_effect_info *info)
{
	struct effect_entry *e = entry_for_info(info);
	return e ? e->hotkey : OBS_INVALID_HOTKEY_ID;
}

/* Combinaciones asignadas a un atajo (se copian; el texto se arma despues). */
#define MAX_SHOWN_BINDINGS 4

struct binding_copy {
	obs_hotkey_id id;
	obs_key_combination_t combos[MAX_SHOWN_BINDINGS];
	size_t num;
};

static bool copy_binding(void *data, size_t idx, obs_hotkey_binding_t *binding)
{
	UNUSED_PARAMETER(idx);
	struct binding_copy *copy = data;

	if (obs_hotkey_binding_get_hotkey_id(binding) == copy->id && copy->num < MAX_SHOWN_BINDINGS)
		copy->combos[copy->num++] = obs_hotkey_binding_get_key_combination(binding);

	return true;
}

char *vfx_control_get_hotkey_text(const struct vfx_effect_info *info)
{
	struct effect_entry *e = entry_for_info(info);
	if (!e)
		return NULL;

	struct binding_copy copy = {0};
	copy.id = e->hotkey;
	obs_enum_hotkey_bindings(copy_binding, &copy);

	/* El texto se arma fuera de la enumeracion de OBS (sin candados tomados). */
	struct dstr text = {0};
	for (size_t i = 0; i < copy.num; i++) {
		struct dstr one = {0};
		obs_key_combination_to_str(copy.combos[i], &one);
		if (one.len) {
			if (text.len)
				dstr_cat(&text, " / ");
			dstr_cat(&text, one.array);
		}
		dstr_free(&one);
	}

	if (!text.len) {
		dstr_free(&text);
		return NULL;
	}
	return text.array;
}

bool vfx_control_set_hotkey(const struct vfx_effect_info *info, obs_key_combination_t combo)
{
	struct effect_entry *e = entry_for_info(info);
	if (!e || combo.key == OBS_KEY_NONE)
		return false;

	/* Reemplaza las teclas de este atajo (OBS avisa y se guardan solas). */
	obs_hotkey_load_bindings(e->hotkey, &combo, 1);
	return true;
}

void vfx_control_clear_hotkey(const struct vfx_effect_info *info)
{
	struct effect_entry *e = entry_for_info(info);
	if (e)
		obs_hotkey_load_bindings(e->hotkey, NULL, 0);
}

static void hotkey_pressed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	/* Solo al apretar la tecla, no al soltarla. */
	if (!pressed)
		return;

	struct effect_entry *e = data;
	vfx_control_toggle(e->info);
}

/* ------------------------------------------------------------------------- */
/* Guardar y cargar las teclas                                                */

static char *hotkeys_file_path(void)
{
	/* La carpeta de configuracion del plugin se crea si no existe. */
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path(HOTKEYS_FILE);
}

static void hotkeys_save(void)
{
	char *path = hotkeys_file_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create();

	for (size_t i = 0; i < g_count; i++) {
		obs_data_array_t *bindings = obs_hotkey_save(g_entries[i].hotkey);
		if (bindings) {
			obs_data_set_array(data, g_entries[i].info->id, bindings);
			obs_data_array_release(bindings);
		}
	}

	if (!obs_data_save_json_safe(data, path, "tmp", "bak"))
		obs_log(LOG_WARNING, "no se pudieron guardar los atajos en %s", path);

	obs_data_release(data);
	bfree(path);
}

static void hotkeys_load(void)
{
	char *path = hotkeys_file_path();
	if (!path)
		return;

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);

	if (!data)
		return; /* todavia no hay teclas guardadas */

	g_loading = true;
	for (size_t i = 0; i < g_count; i++) {
		obs_data_array_t *bindings = obs_data_get_array(data, g_entries[i].info->id);
		if (bindings) {
			obs_hotkey_load(g_entries[i].hotkey, bindings);
			obs_data_array_release(bindings);
		}
	}
	g_loading = false;

	obs_data_release(data);
}

/* OBS avisa cada vez que cambian las teclas de cualquier atajo. */
static void on_bindings_changed(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);

	if (g_loading)
		return;

	obs_hotkey_t *hotkey = calldata_ptr(cd, "key");
	if (hotkey && entry_for_hotkey(obs_hotkey_get_id(hotkey))) {
		hotkeys_save();
		vfx_filter_notify_changed(); /* el panel muestra la tecla nueva al instante */
	}
}

/* ------------------------------------------------------------------------- */
/* Inicio y cierre                                                            */

void vfx_control_init(const struct vfx_effect_info *const *effects, size_t count)
{
	g_count = 0;

	for (size_t i = 0; i < count && g_count < VFX_MAX_EFFECTS; i++) {
		const struct vfx_effect_info *info = effects[i];
		if (!vfx_effect_info_is_valid(info))
			continue;

		struct effect_entry *e = &g_entries[g_count];
		e->info = info;

		/* Nombre interno (no cambiarlo nunca) y descripcion visible en Ajustes -> Atajos. */
		struct dstr name = {0};
		struct dstr description = {0};
		dstr_printf(&name, "obs_voice_fx.toggle.%s", info->id);
		dstr_copy(&description, obs_module_text(info->name_key));
		dstr_cat(&description, obs_module_text("Hotkey.ToggleSuffix"));

		/* OBS copia los textos: se pueden liberar despues. */
		e->hotkey = obs_hotkey_register_frontend(name.array, description.array, hotkey_pressed, e);

		dstr_free(&name);
		dstr_free(&description);

		if (e->hotkey != OBS_INVALID_HOTKEY_ID)
			g_count++;
	}

	hotkeys_load();
	signal_handler_connect(obs_get_signal_handler(), "hotkey_bindings_changed", on_bindings_changed, NULL);

	obs_log(LOG_INFO, "%zu atajo(s) registrado(s)", g_count);
}

void vfx_control_shutdown(void)
{
	signal_handler_disconnect(obs_get_signal_handler(), "hotkey_bindings_changed", on_bindings_changed, NULL);

	for (size_t i = 0; i < g_count; i++) {
		obs_hotkey_unregister(g_entries[i].hotkey);
		g_entries[i].hotkey = OBS_INVALID_HOTKEY_ID;
		g_entries[i].info = NULL;
	}
	g_count = 0;
}
