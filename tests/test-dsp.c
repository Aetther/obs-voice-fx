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
 * Pruebas del procesamiento de audio. No necesitan OBS.
 *
 * Ejecutar:  ctest --test-dir build -C Release --output-on-failure
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/safety.h"

/* ------------------------------------------------------------------------- */
/* Mini sistema de pruebas                                                    */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                  \
	do {                                                                         \
		g_checks++;                                                          \
		if (!(cond)) {                                                       \
			g_failures++;                                                \
			printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
		}                                                                    \
	} while (0)

/* NaN e infinito generados en tiempo de ejecucion (evita avisos del compilador). */
static volatile float g_big = 1.0e30f;

static float make_inf(void)
{
	return g_big * g_big;
}

static float make_nan(void)
{
	float inf = make_inf();
	return inf - inf;
}

/* Generador pseudoaleatorio simple y reproducible (xorshift32). */
static uint32_t g_rng = 0x12345678u;

static float rand_range(float min, float max)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	float t = (float)(g_rng >> 8) / (float)(1u << 24);
	return min + (max - min) * t;
}

/* ------------------------------------------------------------------------- */
/* safety.h                                                                   */

static void test_param_clamp(void)
{
	printf("vfx_param_clamp\n");
	CHECK(vfx_param_clamp(0.5, 0.0, 1.0, 0.25) == 0.5f);
	CHECK(vfx_param_clamp(-3.0, 0.0, 1.0, 0.25) == 0.0f);
	CHECK(vfx_param_clamp(7.0, 0.0, 1.0, 0.25) == 1.0f);
	CHECK(vfx_param_clamp((double)make_nan(), 0.0, 1.0, 0.25) == 0.25f);
	CHECK(vfx_param_clamp((double)make_inf(), 0.0, 1.0, 0.25) == 0.25f);
	CHECK(vfx_param_clamp(-(double)make_inf(), 0.0, 1.0, 0.25) == 0.25f);
}

static void test_sanitize_and_denormal(void)
{
	printf("vfx_sanitize_input / vfx_flush_denormal\n");
	CHECK(vfx_sanitize_input(0.3f) == 0.3f);
	CHECK(vfx_sanitize_input(make_nan()) == 0.0f);
	CHECK(vfx_sanitize_input(make_inf()) == 0.0f);
	CHECK(vfx_flush_denormal(1.0e-30f) == 0.0f);
	CHECK(vfx_flush_denormal(-1.0e-30f) == 0.0f);
	CHECK(vfx_flush_denormal(0.001f) == 0.001f);
}

static void test_mix_additive(void)
{
	printf("vfx_mix_additive\n");
	bool fault = false;

	/* Sin efecto, la voz sale identica (incluso si supera 0 dBFS). */
	CHECK(vfx_mix_additive(0.5f, 0.0f, &fault) == 0.5f);
	CHECK(vfx_mix_additive(1.7f, 0.0f, &fault) == 1.7f);
	CHECK(vfx_mix_additive(-1.7f, 0.0f, &fault) == -1.7f);

	/* Suma normal. */
	CHECK(vfx_mix_additive(0.5f, 0.25f, &fault) == 0.75f);

	/* El efecto no puede pasar el techo. */
	CHECK(vfx_mix_additive(0.9f, 0.5f, &fault) == 1.0f);
	CHECK(vfx_mix_additive(-0.9f, -0.5f, &fault) == -1.0f);

	/* Si la voz ya supera el techo, queda igual y el efecto no suma. */
	CHECK(vfx_mix_additive(1.5f, 0.2f, &fault) == 1.5f);
	CHECK(vfx_mix_additive(-1.5f, -0.2f, &fault) == -1.5f);

	CHECK(fault == false);

	/* Efecto invalido: pasa la voz y se avisa. */
	fault = false;
	CHECK(vfx_mix_additive(0.4f, make_nan(), &fault) == 0.4f);
	CHECK(fault == true);

	fault = false;
	CHECK(vfx_mix_additive(0.4f, make_inf(), &fault) == 0.4f);
	CHECK(fault == true);

	/* Puntero de aviso nulo: no debe romper. */
	CHECK(vfx_mix_additive(0.4f, make_nan(), NULL) == 0.4f);
}

static void test_mix_replace(void)
{
	printf("vfx_mix_replace\n");
	bool fault = false;

	CHECK(vfx_mix_replace(0.5f, 0.3f, &fault) == 0.3f);
	CHECK(vfx_mix_replace(0.5f, 2.0f, &fault) == 1.0f);
	CHECK(vfx_mix_replace(0.5f, -2.0f, &fault) == -1.0f);
	CHECK(vfx_mix_replace(1.5f, 2.0f, &fault) == 1.5f);
	CHECK(fault == false);

	CHECK(vfx_mix_replace(0.5f, make_nan(), &fault) == 0.5f);
	CHECK(fault == true);
}

/* Un millon de combinaciones al azar, incluidos valores invalidos. */
static void test_mix_additive_random(void)
{
	printf("vfx_mix_additive (1.000.000 casos al azar)\n");
	int bad_finite = 0, bad_limit = 0, bad_exact = 0;

	for (int i = 0; i < 1000000; i++) {
		float dry = rand_range(-2.0f, 2.0f);
		float wet = rand_range(-10.0f, 10.0f);
		if (i % 97 == 0)
			wet = make_nan();
		if (i % 89 == 0)
			wet = make_inf();

		float out = vfx_mix_additive(dry, wet, NULL);
		float limit = fabsf(dry) > VFX_CEILING ? fabsf(dry) : VFX_CEILING;

		if (!isfinite(out))
			bad_finite++;
		if (fabsf(out) > limit)
			bad_limit++;
		if (isfinite(wet) && fabsf(dry + wet) <= VFX_CEILING && out != dry + wet)
			bad_exact++;
	}

	CHECK(bad_finite == 0);
	CHECK(bad_limit == 0);
	CHECK(bad_exact == 0);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
	test_param_clamp();
	test_sanitize_and_denormal();
	test_mix_additive();
	test_mix_replace();
	test_mix_additive_random();

	printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}