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
#include <stdlib.h>
#include <string.h>

#include "core/safety.h"
#include "effects/reverb/freeverb.h"

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
/* freeverb                                                                   */

#define TEST_RATE 48000u
#define TEST_BLOCK 4800 /* 100 ms a 48 kHz */

static bool all_finite(const float *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (!isfinite(buf[i]))
			return false;
	}
	return true;
}

static float peak_abs(const float *buf, size_t n)
{
	float peak = 0.0f;
	for (size_t i = 0; i < n; i++) {
		if (fabsf(buf[i]) > peak)
			peak = fabsf(buf[i]);
	}
	return peak;
}

static double energy(const float *buf, size_t n)
{
	double e = 0.0;
	for (size_t i = 0; i < n; i++)
		e += (double)buf[i] * (double)buf[i];
	return e;
}

static void test_freeverb_init(void)
{
	printf("freeverb_init (frecuencias validas e invalidas)\n");
	struct freeverb fv;

	CHECK(freeverb_init(&fv, 0) == false);
	CHECK(fv.ready == false);
	freeverb_free(&fv);

	CHECK(freeverb_init(&fv, FREEVERB_MIN_SAMPLE_RATE - 1) == false);
	freeverb_free(&fv);

	CHECK(freeverb_init(&fv, FREEVERB_MAX_SAMPLE_RATE + 1) == false);
	freeverb_free(&fv);

	const uint32_t rates[] = {8000u, 44100u, 48000u, 96000u, 384000u};
	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		CHECK(freeverb_init(&fv, rates[i]) == true);
		CHECK(fv.ready == true);
		freeverb_free(&fv);
		CHECK(fv.ready == false);
	}

	/* Liberar dos veces no debe romper. */
	freeverb_free(&fv);
}

static void test_freeverb_not_ready(void)
{
	printf("freeverb sin inicializar produce silencio\n");
	struct freeverb fv;
	freeverb_init(&fv, 0); /* falla a proposito */

	float in[64], wl[64], wr[64];
	for (int i = 0; i < 64; i++) {
		in[i] = 0.5f;
		wl[i] = 9.0f;
		wr[i] = 9.0f;
	}

	freeverb_process_wet(&fv, in, in, wl, wr, 64);
	CHECK(peak_abs(wl, 64) == 0.0f);
	CHECK(peak_abs(wr, 64) == 0.0f);
	freeverb_free(&fv);
}

static void test_freeverb_params(void)
{
	printf("freeverb_set_params (limites y valores invalidos)\n");
	struct freeverb fv;
	CHECK(freeverb_init(&fv, TEST_RATE));

	freeverb_set_params(&fv, 5.0f, -2.0f, 3.0f, 7.0f);
	CHECK(fv.room_size == 1.0f);
	CHECK(fv.damping == 0.0f);
	CHECK(fv.width == 1.0f);
	CHECK(fv.amount == 1.0f);
	CHECK(fv.feedback < 1.0f); /* la cola siempre se apaga */

	freeverb_set_params(&fv, make_nan(), make_nan(), make_inf(), make_nan());
	CHECK(fv.room_size == 0.5f);
	CHECK(fv.damping == 0.5f);
	CHECK(fv.width == 1.0f);
	CHECK(fv.amount == 0.2f);

	freeverb_free(&fv);
}

static void test_freeverb_impulse(void)
{
	printf("freeverb: respuesta a un impulso (la cola se apaga sola)\n");
	struct freeverb fv;
	CHECK(freeverb_init(&fv, TEST_RATE));
	freeverb_set_params(&fv, 0.5f, 0.5f, 1.0f, 0.2f);

	const size_t n = TEST_RATE * 3; /* 3 segundos */
	float *in = calloc(n, sizeof(float));
	float *wl = calloc(n, sizeof(float));
	float *wr = calloc(n, sizeof(float));
	CHECK(in && wl && wr);
	if (!in || !wl || !wr)
		goto done;

	in[0] = 1.0f;
	freeverb_process_wet(&fv, in, in, wl, wr, n);

	CHECK(all_finite(wl, n));
	CHECK(all_finite(wr, n));

	double e0 = energy(wl, TEST_RATE);
	double e1 = energy(wl + TEST_RATE, TEST_RATE);
	double e2 = energy(wl + 2 * TEST_RATE, TEST_RATE);
	CHECK(e0 > 0.0);       /* hay reverb */
	CHECK(e1 < e0 * 1e-2); /* cae mucho en el segundo 2 */
	CHECK(e2 < e1);        /* y sigue cayendo */

	/* El izquierdo y el derecho son distintos (sensacion estereo). */
	CHECK(memcmp(wl, wr, n * sizeof(float)) != 0);

done:
	free(in);
	free(wl);
	free(wr);
	freeverb_free(&fv);
}

static void test_freeverb_tail_reaches_zero(void)
{
	printf("freeverb: tras un silencio largo (120 s), la cola llega a cero exacto\n");
	struct freeverb fv;
	CHECK(freeverb_init(&fv, TEST_RATE));
	freeverb_set_params(&fv, 1.0f, 0.0f, 1.0f, 1.0f); /* la cola mas larga posible */

	float block[1024];
	float wl[1024], wr[1024];

	for (int i = 0; i < 1024; i++)
		block[i] = rand_range(-1.0f, 1.0f);
	freeverb_process_wet(&fv, block, block, wl, wr, 1024);

	/* Con la sala al maximo la cola dura mucho (medido: cero exacto a los ~80 s). */
	memset(block, 0, sizeof(block));
	for (int i = 0; i < (int)(TEST_RATE * 120 / 1024); i++) /* 120 segundos de silencio */
		freeverb_process_wet(&fv, block, block, wl, wr, 1024);

	CHECK(peak_abs(wl, 1024) == 0.0f);
	CHECK(peak_abs(wr, 1024) == 0.0f);
	freeverb_free(&fv);
}

static void test_freeverb_worst_case(void)
{
	printf("freeverb: peor caso (sala maxima, sin amortiguacion, ruido a todo volumen, 30 s)\n");
	struct freeverb fv;
	CHECK(freeverb_init(&fv, TEST_RATE));
	freeverb_set_params(&fv, 1.0f, 0.0f, 1.0f, 1.0f);

	float in_l[1024], in_r[1024], wl[1024], wr[1024];
	float peak = 0.0f;
	bool finite = true;
	bool mixed_ok = true;

	for (int b = 0; b < (int)(TEST_RATE * 30 / 1024); b++) {
		for (int i = 0; i < 1024; i++) {
			in_l[i] = rand_range(-1.0f, 1.0f);
			in_r[i] = rand_range(-1.0f, 1.0f);
		}
		freeverb_process_wet(&fv, in_l, in_r, wl, wr, 1024);

		finite = finite && all_finite(wl, 1024) && all_finite(wr, 1024);
		float p = peak_abs(wl, 1024);
		if (p > peak)
			peak = p;

		/* Mezclado con la voz, nunca pasa el techo. */
		for (int i = 0; i < 1024; i++) {
			float out = vfx_mix_additive(in_l[i], wl[i], NULL);
			if (fabsf(out) > VFX_CEILING)
				mixed_ok = false;
		}
	}

	printf("  pico de la reverb sola: %.3f\n", (double)peak);
	CHECK(finite);
	CHECK(peak < 20.0f); /* acotada: no crece sin control */
	CHECK(mixed_ok);
	freeverb_free(&fv);
}

static void test_freeverb_invalid_input(void)
{
	printf("freeverb: entrada con NaN/infinito no la contamina\n");
	struct freeverb fv;
	CHECK(freeverb_init(&fv, TEST_RATE));

	/* Bloques de 100 ms: la reverb tarda unos 25 ms en empezar a sonar. */
	static float in[TEST_BLOCK], wl[TEST_BLOCK], wr[TEST_BLOCK];
	for (int i = 0; i < TEST_BLOCK; i++)
		in[i] = (i % 3 == 0) ? make_nan() : ((i % 5 == 0) ? make_inf() : 0.3f);

	freeverb_process_wet(&fv, in, in, wl, wr, TEST_BLOCK);
	CHECK(all_finite(wl, TEST_BLOCK));
	CHECK(all_finite(wr, TEST_BLOCK));

	/* Despues, con entrada normal, sigue funcionando bien. */
	for (int i = 0; i < TEST_BLOCK; i++)
		in[i] = rand_range(-0.5f, 0.5f);
	freeverb_process_wet(&fv, in, in, wl, wr, TEST_BLOCK);
	CHECK(all_finite(wl, TEST_BLOCK));
	CHECK(peak_abs(wl, TEST_BLOCK) > 0.0f);

	freeverb_free(&fv);
}

static void test_freeverb_amount_zero_and_clear(void)
{
	printf("freeverb: cantidad 0 = silencio; clear borra la cola\n");
	struct freeverb fv;
	CHECK(freeverb_init(&fv, TEST_RATE));

	float in[512], wl[512], wr[512];
	for (int i = 0; i < 512; i++)
		in[i] = rand_range(-0.8f, 0.8f);

	freeverb_set_params(&fv, 0.5f, 0.5f, 1.0f, 0.0f);
	freeverb_process_wet(&fv, in, in, wl, wr, 512);
	CHECK(peak_abs(wl, 512) == 0.0f);
	CHECK(peak_abs(wr, 512) == 0.0f);

	/* Con cantidad normal hay cola; despues de clear, el silencio da silencio exacto. */
	freeverb_set_params(&fv, 0.8f, 0.5f, 1.0f, 0.5f);
	freeverb_process_wet(&fv, in, in, wl, wr, 512);
	freeverb_clear(&fv);

	memset(in, 0, sizeof(in));
	freeverb_process_wet(&fv, in, in, wl, wr, 512);
	CHECK(peak_abs(wl, 512) == 0.0f);
	CHECK(peak_abs(wr, 512) == 0.0f);

	freeverb_free(&fv);
}

static void test_freeverb_mono(void)
{
	printf("freeverb: audio mono\n");
	struct freeverb fv;
	CHECK(freeverb_init(&fv, TEST_RATE));

	static float in[TEST_BLOCK], wl[TEST_BLOCK];
	for (int i = 0; i < TEST_BLOCK; i++)
		in[i] = rand_range(-0.5f, 0.5f);

	freeverb_process_wet(&fv, in, NULL, wl, NULL, TEST_BLOCK);
	CHECK(all_finite(wl, TEST_BLOCK));
	CHECK(peak_abs(wl, TEST_BLOCK) > 0.0f);

	freeverb_free(&fv);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
	test_param_clamp();
	test_sanitize_and_denormal();
	test_mix_additive();
	test_mix_replace();
	test_mix_additive_random();

	test_freeverb_init();
	test_freeverb_not_ready();
	test_freeverb_params();
	test_freeverb_impulse();
	test_freeverb_tail_reaches_zero();
	test_freeverb_worst_case();
	test_freeverb_invalid_input();
	test_freeverb_amount_zero_and_clear();
	test_freeverb_mono();

	printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
