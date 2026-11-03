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
 * Linea de retardo: guarda las ultimas muestras para poder leer "lo que
 * entro hace N muestras". Es la base del eco, del pre-delay de la reverb,
 * del chorus y del cambio de tono.
 *
 * Uso en cada muestra:
 *     vfx_delay_line_push(&d, entrada);
 *     salida = vfx_delay_line_read(&d, retardo);   // retardo 0 = lo recien guardado
 *
 * Seguridad:
 *   - La memoria se reserva una sola vez en vfx_delay_line_init (fuera del
 *     hilo de audio). push/read/clear no reservan nada.
 *   - Cualquier retardo pedido se limita a [0, max_delay]: nunca se lee
 *     fuera de la memoria, aunque el valor venga danado (NaN, negativo, enorme).
 *   - Si init fallo, push no hace nada y read devuelve 0 (silencio).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Retardo maximo permitido: unos 87 segundos a 192 kHz. Evita reservas absurdas. */
#define VFX_DELAY_LINE_MAX_SAMPLES ((size_t)1 << 24)

struct vfx_delay_line {
	float *buffer;
	size_t mask;      /* tamano - 1 (el tamano es potencia de 2) */
	size_t write;     /* posicion de la ultima muestra guardada */
	size_t max_delay; /* retardo maximo que se puede leer */
};

/*
 * Reserva memoria para leer retardos de hasta max_delay muestras.
 * Devuelve false si max_delay es 0, supera el limite o falta memoria.
 */
bool vfx_delay_line_init(struct vfx_delay_line *d, size_t max_delay);

/* Libera la memoria. Acepta una linea sin iniciar o ya liberada. */
void vfx_delay_line_free(struct vfx_delay_line *d);

/* Llena de silencio (por ejemplo, al reactivar el efecto). */
void vfx_delay_line_clear(struct vfx_delay_line *d);

/* Guarda una muestra nueva. */
static inline void vfx_delay_line_push(struct vfx_delay_line *d, float x)
{
	if (!d->buffer)
		return;
	d->write = (d->write + 1) & d->mask;
	d->buffer[d->write] = x;
}

/* Lee la muestra guardada hace "delay" muestras (entero). */
static inline float vfx_delay_line_read(const struct vfx_delay_line *d, size_t delay)
{
	if (!d->buffer)
		return 0.0f;
	if (delay > d->max_delay)
		delay = d->max_delay;
	return d->buffer[(d->write - delay) & d->mask];
}

/*
 * Lee con retardo fraccionario (por ejemplo 120.37 muestras), interpolando
 * entre las dos muestras vecinas. Necesario cuando el retardo cambia de a
 * poco (chorus, cambio de tono) para que no se escuchen saltos.
 */
static inline float vfx_delay_line_read_frac(const struct vfx_delay_line *d, float delay)
{
	if (!d->buffer)
		return 0.0f;

	/* "!(delay >= 0)" tambien atrapa NaN. */
	if (!(delay >= 0.0f))
		delay = 0.0f;
	if (delay > (float)d->max_delay)
		delay = (float)d->max_delay;

	size_t whole = (size_t)delay;
	float frac = delay - (float)whole;
	if (whole >= d->max_delay) {
		whole = d->max_delay;
		frac = 0.0f;
	}

	const float a = d->buffer[(d->write - whole) & d->mask];
	const float b = d->buffer[(d->write - whole - 1) & d->mask];
	return a + (b - a) * frac;
}

#ifdef __cplusplus
}
#endif
