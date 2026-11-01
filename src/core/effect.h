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
 * El "contrato" que cumple cada efecto.
 *
 * Un efecto se describe con datos (nombre, deslizadores) y unas pocas
 * funciones de audio. NO usa nada de OBS: la parte de OBS (filtro,
 * deslizadores en pantalla, atajos) la arma core/ automaticamente.
 *
 * Reglas para las funciones de un efecto:
 *   - create:  puede reservar memoria. Devuelve NULL si falla
 *              (entonces el filtro deja pasar la voz sin cambios).
 *   - destroy: libera todo. Tiene que aceptar NULL.
 *   - set_params, clear, process: se llaman desde el hilo de audio.
 *              NO pueden reservar memoria ni bloquearse.
 *   - process: escribe exactamente "frames" muestras en out_left (y en
 *              out_right si no es NULL). in_right/out_right son NULL si el
 *              audio es mono. Las salidas nunca son las mismas que las entradas.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cantidad maxima de deslizadores por efecto. */
#define VFX_MAX_PARAMS 8

/* Como se combina el resultado del efecto con la voz (ver core/safety.h). */
enum vfx_mix_mode {
	VFX_MIX_ADDITIVE = 0, /* voz intacta + lo que agrega el efecto (reverb, eco) */
	VFX_MIX_REPLACE = 1,  /* el efecto reemplaza la voz (cambio de tono, robot) */
};

/* Un deslizador. */
struct vfx_param_info {
	const char *key;      /* nombre interno guardado en la configuracion de OBS (no cambiarlo nunca) */
	const char *text_key; /* clave del texto traducido en data/locale/<idioma>.ini */
	double min;
	double max;
	double default_value;
	double step;
	const char *suffix; /* texto despues del numero, por ejemplo " %" (puede ser NULL) */
};

/* Descripcion completa de un efecto. */
struct vfx_effect_info {
	const char *id;       /* identificador del filtro en OBS (no cambiarlo nunca) */
	const char *name_key; /* clave del texto traducido con el nombre del filtro */
	enum vfx_mix_mode mix_mode;

	const struct vfx_param_info *params;
	size_t num_params;

	void *(*create)(uint32_t sample_rate);
	void (*destroy)(void *state);

	/* values: un valor por deslizador, en el mismo orden que params, ya validados. */
	void (*set_params)(void *state, const float *values);

	void (*clear)(void *state);

	void (*process)(void *state, const float *in_left, const float *in_right, float *out_left, float *out_right,
			size_t frames);
};

/*
 * Verifica que la descripcion de un efecto sea coherente: funciones presentes,
 * rangos validos, valor por defecto dentro del rango, claves no repetidas.
 * Un efecto que no pasa esta verificacion no se registra en OBS.
 */
bool vfx_effect_info_is_valid(const struct vfx_effect_info *info);

#ifdef __cplusplus
}
#endif
