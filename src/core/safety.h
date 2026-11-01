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
 * Reglas de seguridad comunes a todos los efectos.
 *
 * Este archivo no depende de OBS: se puede probar solo (tests/test-dsp.c).
 *
 * Garantias:
 *  1. La voz original nunca se modifica: el efecto solo SUMA por debajo.
 *  2. El efecto nunca lleva la senal por encima del maximo digital (0 dBFS).
 *     Si la voz ya venia mas fuerte que eso, se deja tal cual y el efecto
 *     no suma nada en esa muestra.
 *  3. Si el efecto produce un valor invalido (NaN o infinito), se descarta
 *     y pasa la voz sin cambios. Quien llama se entera por "fault" y puede
 *     reiniciar el efecto.
 *  4. Los parametros (deslizadores) siempre quedan dentro de su rango,
 *     aunque el archivo de configuracion tenga valores corruptos.
 */

#pragma once

#include <math.h>
#include <stdbool.h>

/* Maximo digital: 1.0 en audio de punto flotante equivale a 0 dBFS. */
#define VFX_CEILING 1.0f

/* Valores por debajo de esto se consideran silencio (unos -400 dB). */
#define VFX_DENORMAL_THRESHOLD 1.0e-20f

/*
 * Devuelve un parametro valido: si no es un numero finito usa "fallback",
 * y despues lo limita al rango [min, max].
 */
static inline float vfx_param_clamp(double value, double min, double max, double fallback)
{
	if (!isfinite(value))
		value = fallback;
	if (value < min)
		value = min;
	if (value > max)
		value = max;
	return (float)value;
}

/*
 * Muestra que entra a un algoritmo: nunca NaN ni infinito.
 * Un solo valor invalido dentro de una reverb la "contamina" para siempre,
 * por eso se reemplaza por silencio antes de entrar.
 */
static inline float vfx_sanitize_input(float x)
{
	return isfinite(x) ? x : 0.0f;
}

/*
 * Convierte valores minusculos en cero exacto. Los numeros "denormales" son
 * inaudibles pero en algunas CPU hacen el calculo cientos de veces mas lento.
 */
static inline float vfx_flush_denormal(float x)
{
	return fabsf(x) < VFX_DENORMAL_THRESHOLD ? 0.0f : x;
}

/*
 * Mezcla para efectos que SUMAN algo a la voz (reverb, eco, ...).
 *
 *  dry:   la voz original (no se modifica nunca).
 *  wet:   lo que agrega el efecto.
 *  fault: si no es NULL, se pone en true cuando "wet" es invalido.
 */
static inline float vfx_mix_additive(float dry, float wet, bool *fault)
{
	/* La voz no la generamos nosotros: si ya viene invalida, pasa igual. */
	if (!isfinite(dry))
		return dry;

	if (!isfinite(wet)) {
		if (fault)
			*fault = true;
		return dry;
	}

	float out = dry + wet;

	if (out > VFX_CEILING)
		out = (dry > VFX_CEILING) ? dry : VFX_CEILING;
	else if (out < -VFX_CEILING)
		out = (dry < -VFX_CEILING) ? dry : -VFX_CEILING;

	return out;
}

/*
 * Mezcla para efectos que REEMPLAZAN la voz (por ejemplo, cambio de tono).
 * Si el resultado es invalido, pasa la voz original. El resultado nunca supera
 * el techo, salvo que la voz original ya lo superara (en ese caso, ese nivel).
 */
static inline float vfx_mix_replace(float dry, float processed, bool *fault)
{
	if (!isfinite(processed)) {
		if (fault)
			*fault = true;
		return dry;
	}

	float limit = VFX_CEILING;
	if (isfinite(dry) && fabsf(dry) > limit)
		limit = fabsf(dry);

	if (processed > limit)
		return limit;
	if (processed < -limit)
		return -limit;
	return processed;
}