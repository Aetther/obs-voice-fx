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
 * Motor comun que hace funcionar cualquier efecto sobre el audio.
 *
 * Se encarga de:
 *   - crear y destruir el efecto,
 *   - validar los parametros antes de pasarselos,
 *   - procesar en bloques de tamano fijo (sin reservar memoria en el hilo de audio),
 *   - mezclar con la voz usando las reglas de core/safety.h,
 *   - reiniciar el efecto si alguna vez produce valores invalidos.
 *
 * No usa nada de OBS: se prueba solo en tests/test-dsp.c.
 */

#pragma once

#include "core/effect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tamano maximo de cada bloque que se le pasa al efecto. */
#define VFX_BLOCK_FRAMES 1024

struct vfx_processor {
	const struct vfx_effect_info *info;
	void *state; /* NULL si el efecto no se pudo crear: la voz pasa sin cambios */

	/* Memoria de trabajo reservada de antemano (nunca en el hilo de audio). */
	float scratch_left[VFX_BLOCK_FRAMES];
	float scratch_right[VFX_BLOCK_FRAMES];
};

/*
 * Crea el efecto para una frecuencia de muestreo y le aplica los valores por
 * defecto. Devuelve false si no se pudo crear (el procesador queda en modo
 * "pasar la voz sin cambios", que es seguro de usar igual).
 */
bool vfx_processor_init(struct vfx_processor *p, const struct vfx_effect_info *info, uint32_t sample_rate);

/* Destruye el efecto. Se puede llamar aunque init haya fallado. */
void vfx_processor_free(struct vfx_processor *p);

/*
 * Aplica nuevos valores de los deslizadores. "values" tiene info->num_params
 * elementos; cada uno se valida otra vez contra su rango antes de usarlo.
 */
void vfx_processor_set_params(struct vfx_processor *p, const float *values);

/* Borra la memoria interna del efecto (por ejemplo, la cola de la reverb). */
void vfx_processor_clear(struct vfx_processor *p);

/*
 * Procesa audio en el lugar.
 *   left:  canal izquierdo (o el unico, si es mono).
 *   right: canal derecho, o NULL si es mono.
 * Devuelve true si el efecto produjo valores invalidos en este llamado;
 * en ese caso esas muestras salieron como la voz original y el efecto
 * ya se reinicio.
 */
bool vfx_processor_run(struct vfx_processor *p, float *left, float *right, size_t frames);

#ifdef __cplusplus
}
#endif
