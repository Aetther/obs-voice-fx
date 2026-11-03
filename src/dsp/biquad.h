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
 * Filtro "biquad": el filtro basico de casi todo el procesamiento de audio.
 *
 * Con distintos coeficientes hace de:
 *   - paso bajo   (deja pasar los graves, corta los agudos)
 *   - paso alto   (deja pasar los agudos, corta los graves)
 *   - pasa banda  (deja pasar solo una zona, como un telefono)
 *   - pico        (sube o baja una zona: ecualizador)
 *   - estantes    (suben o bajan todo lo que esta por debajo / por encima)
 *
 * Formulas del "Audio EQ Cookbook" de Robert Bristow-Johnson (de uso libre),
 * escritas desde cero.
 *
 * Seguridad:
 *   - vfx_biquad_set valida todo. Si algun valor no sirve (frecuencia fuera
 *     de rango, NaN, etc.), el filtro queda en "pasar sin cambios".
 *   - vfx_biquad_process nunca produce NaN ni valores denormales a partir de
 *     una entrada valida, y no reserva memoria: se puede usar en el hilo de audio.
 */

#pragma once

#include "core/safety.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vfx_biquad_type {
	VFX_BIQUAD_LOWPASS = 0,
	VFX_BIQUAD_HIGHPASS,
	VFX_BIQUAD_BANDPASS, /* ganancia 0 dB en la frecuencia central */
	VFX_BIQUAD_PEAK,
	VFX_BIQUAD_LOWSHELF,
	VFX_BIQUAD_HIGHSHELF,
};

/* Limites aceptados por vfx_biquad_set. */
#define VFX_BIQUAD_MIN_FREQ 10.0       /* Hz */
#define VFX_BIQUAD_MAX_FREQ_RATIO 0.45 /* como maximo, 45 % de la frecuencia de muestreo */
#define VFX_BIQUAD_MIN_Q 0.1
#define VFX_BIQUAD_MAX_Q 20.0
#define VFX_BIQUAD_MAX_GAIN_DB 24.0

struct vfx_biquad {
	/* Coeficientes (ya divididos por a0). */
	float b0, b1, b2, a1, a2;
	/* Memoria del filtro. */
	float z1, z2;
};

/* Deja el filtro en "pasar sin cambios" y borra su memoria. */
void vfx_biquad_init(struct vfx_biquad *f);

/* Borra la memoria del filtro (por ejemplo, al reactivar el efecto). */
void vfx_biquad_clear(struct vfx_biquad *f);

/*
 * Calcula los coeficientes. No borra la memoria, asi que se puede llamar
 * mientras suena (al mover un deslizador) sin producir clics fuertes.
 *
 *   sample_rate: frecuencia de muestreo en Hz.
 *   freq:        frecuencia de corte o central en Hz.
 *   q:           ancho (0.707 = respuesta plana tipica para paso bajo/alto).
 *   gain_db:     solo para pico y estantes; se ignora en los demas.
 *
 * Devuelve false si algun valor no era valido: el filtro queda en "pasar sin cambios".
 */
bool vfx_biquad_set(struct vfx_biquad *f, enum vfx_biquad_type type, double sample_rate, double freq, double q,
		    double gain_db);

/* Procesa una muestra (forma directa II transpuesta). */
static inline float vfx_biquad_process(struct vfx_biquad *f, float x)
{
	float y = f->b0 * x + f->z1;
	f->z1 = vfx_flush_denormal(f->b1 * x - f->a1 * y + f->z2);
	f->z2 = vfx_flush_denormal(f->b2 * x - f->a2 * y);
	return y;
}

#ifdef __cplusplus
}
#endif
