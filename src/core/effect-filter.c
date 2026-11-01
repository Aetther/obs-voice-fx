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
 * Hilos involucrados:
 *   - Hilo de la interfaz: create, destroy, update, propiedades.
 *   - Hilo de audio: filter_audio (cada ~10-20 ms).
 *
 * Reglas de seguridad entre hilos:
 *   - El procesador (efecto + memoria de trabajo) SOLO lo usa el hilo de audio.
 *   - Los valores nuevos de los deslizadores se dejan en "pending" protegidos
 *     por un candado (SRWLOCK). El hilo de audio los toma SIN esperar: si el
 *     candado esta ocupado en ese instante, los toma en el bloque siguiente.
 *     Asi el audio nunca se traba esperando a la interfaz.
 *   - El pedido de "vaciar la cola" al reactivar el filtro es un entero atomico.
 */

#include "core/effect-filter.h"
#include "core/effect-process.h"
#include "core/safety.h"
#include "plugin-info.h"

#include <obs-module.h>
#include <media-io/audio-io.h>

#include <windows.h>

#include <string.h>

struct vfx_filter {
	obs_source_t *source;
	const struct vfx_effect_info *info;
	struct vfx_processor *processor; /* solo lo usa el hilo de audio (salvo create/destroy) */
	size_t channels;                 /* canales de audio de OBS (2 = estereo) */

	/* Valores nuevos de los deslizadores, protegidos por "lock". */
	SRWLOCK lock;
	float pending[VFX_MAX_PARAMS];
	bool pending_dirty;

	volatile LONG clear_requested; /* 1 = vaciar la memoria del efecto en el proximo bloque */
	volatile LONG fault_reported;  /* 1 = ya se aviso en el log de un fallo del efecto */
};

/*
 * Contador de cambios: sube cada vez que un filtro se crea, se borra, se
 * prende, se apaga o cambia un deslizador. El panel lo compara para saber,
 * al instante y sin recorrer OBS, si tiene que actualizarse.
 */
static volatile LONG g_state_version = 0;

static void state_changed(void)
{
	InterlockedIncrement(&g_state_version);
}

long vfx_filter_state_version(void)
{
	return InterlockedCompareExchange(&g_state_version, 0, 0);
}

void vfx_filter_notify_changed(void)
{
	state_changed();
}

/* ------------------------------------------------------------------------- */
/* Datos del tipo de filtro                                                   */

static const char *filter_get_name(void *type_data)
{
	const struct vfx_effect_info *info = type_data;
	return obs_module_text(info->name_key);
}

static void filter_get_defaults(void *type_data, obs_data_t *settings)
{
	const struct vfx_effect_info *info = type_data;

	for (size_t i = 0; i < info->num_params; i++) {
		const struct vfx_param_info *param = &info->params[i];
		obs_data_set_default_double(settings, param->key, param->default_value);
	}
}

static obs_properties_t *filter_get_properties(void *data, void *type_data)
{
	UNUSED_PARAMETER(data);
	const struct vfx_effect_info *info = type_data;
	obs_properties_t *props = obs_properties_create();

	for (size_t i = 0; i < info->num_params; i++) {
		const struct vfx_param_info *param = &info->params[i];
		obs_property_t *p = obs_properties_add_float_slider(props, param->key, obs_module_text(param->text_key),
								     param->min, param->max, param->step);
		if (p && param->suffix)
			obs_property_float_set_suffix(p, param->suffix);
	}

	return props;
}

/* ------------------------------------------------------------------------- */
/* Ciclo de vida de cada filtro                                               */

/* OBS avisa cuando el filtro se activa o desactiva (el "ojito" en la lista de filtros). */
static void filter_enable_changed(void *data, calldata_t *cd)
{
	struct vfx_filter *f = data;

	/* Al reactivarlo se vacia la cola, para no escuchar restos de antes. */
	if (calldata_bool(cd, "enabled"))
		InterlockedExchange(&f->clear_requested, 1);

	state_changed();
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct vfx_filter *f = data;
	const struct vfx_effect_info *info = f->info;
	float values[VFX_MAX_PARAMS] = {0};

	/* Cada valor se valida: el archivo de configuracion podria estar danado. */
	for (size_t i = 0; i < info->num_params; i++) {
		const struct vfx_param_info *param = &info->params[i];
		double v = obs_data_get_double(settings, param->key);
		values[i] = vfx_param_clamp(v, param->min, param->max, param->default_value);
	}

	AcquireSRWLockExclusive(&f->lock);
	memcpy(f->pending, values, sizeof(values));
	f->pending_dirty = true;
	ReleaseSRWLockExclusive(&f->lock);

	state_changed();
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	const struct vfx_effect_info *info = obs_source_get_type_data(source);
	if (!info)
		return NULL;

	struct vfx_filter *f = bzalloc(sizeof(*f));
	f->source = source;
	f->info = info;
	InitializeSRWLock(&f->lock);

	uint32_t sample_rate = 0;
	audio_t *audio = obs_get_audio();
	if (audio) {
		sample_rate = audio_output_get_sample_rate(audio);
		f->channels = audio_output_get_channels(audio);
	}

	/* El procesador se reserva aca (hilo de la interfaz), nunca en el hilo de audio. */
	f->processor = bzalloc(sizeof(*f->processor));
	if (!vfx_processor_init(f->processor, info, sample_rate)) {
		obs_log(LOG_WARNING, "no se pudo crear el efecto '%s' (%u Hz): el filtro dejara pasar la voz sin cambios",
			info->id, sample_rate);
	}

	signal_handler_connect(obs_source_get_signal_handler(source), "enable", filter_enable_changed, f);

	filter_update(f, settings);
	state_changed();
	return f;
}

static void filter_destroy(void *data)
{
	struct vfx_filter *f = data;

	signal_handler_disconnect(obs_source_get_signal_handler(f->source), "enable", filter_enable_changed, f);

	vfx_processor_free(f->processor);
	bfree(f->processor);
	bfree(f);

	state_changed();
}

/* ------------------------------------------------------------------------- */
/* Audio                                                                      */

static struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio)
{
	struct vfx_filter *f = data;

	if (!audio || audio->frames == 0 || !audio->data[0])
		return audio;

	/* Valores nuevos de los deslizadores: se toman sin esperar. */
	float values[VFX_MAX_PARAMS] = {0};
	bool apply = false;

	if (TryAcquireSRWLockExclusive(&f->lock)) {
		if (f->pending_dirty) {
			memcpy(values, f->pending, sizeof(values));
			f->pending_dirty = false;
			apply = true;
		}
		ReleaseSRWLockExclusive(&f->lock);
	}

	if (apply)
		vfx_processor_set_params(f->processor, values);

	if (InterlockedExchange(&f->clear_requested, 0))
		vfx_processor_clear(f->processor);

	/* Se procesan los canales delanteros (izquierdo y derecho); el resto pasa igual. */
	float *left = (float *)audio->data[0];
	float *right = (f->channels >= 2 && audio->data[1]) ? (float *)audio->data[1] : NULL;

	if (vfx_processor_run(f->processor, left, right, audio->frames)) {
		/* Se avisa una sola vez, para no llenar el log desde el hilo de audio. */
		if (InterlockedExchange(&f->fault_reported, 1) == 0) {
			obs_log(LOG_WARNING, "el efecto '%s' en '%s' produjo valores invalidos: se reinicio y la voz paso sin cambios",
				f->info->id, obs_source_get_name(f->source));
		}
	}

	return audio;
}

/* ------------------------------------------------------------------------- */
/* Registro                                                                   */

bool vfx_filter_register(const struct vfx_effect_info *info)
{
	if (!vfx_effect_info_is_valid(info)) {
		obs_log(LOG_ERROR, "efecto con descripcion invalida: no se registra");
		return false;
	}

	struct obs_source_info si = {0};
	si.id = info->id;
	si.type = OBS_SOURCE_TYPE_FILTER;
	si.output_flags = OBS_SOURCE_AUDIO;
	si.type_data = (void *)info; /* OBS lo devuelve tal cual; nunca se modifica */
	si.get_name = filter_get_name;
	si.create = filter_create;
	si.destroy = filter_destroy;
	si.update = filter_update;
	si.filter_audio = filter_audio;
	si.get_defaults2 = filter_get_defaults;
	si.get_properties2 = filter_get_properties;

	/* OBS copia esta estructura: puede vivir en la pila. */
	obs_register_source(&si);

	obs_log(LOG_INFO, "filtro registrado: %s", info->id);
	return true;
}
