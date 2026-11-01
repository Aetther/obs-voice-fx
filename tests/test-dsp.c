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

#include "core/effect-process.h"
#include "core/safety.h"
#include "effects/effects-list.h"
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
/* Contrato de efectos y motor de procesamiento                               */

/* Efectos de prueba: uno que no se puede crear y uno que produce NaN. */

static void *fake_create_fails(uint32_t sample_rate)
{
	(void)sample_rate;
	return NULL;
}

static int g_fake_state = 0;
static int g_fake_clears = 0;

static void *fake_create_ok(uint32_t sample_rate)
{
	(void)sample_rate;
	return &g_fake_state;
}

static void fake_destroy(void *state)
{
	(void)state;
}

static void fake_set_params(void *state, const float *values)
{
	(void)state;
	(void)values;
}

static void fake_clear(void *state)
{
	(void)state;
	g_fake_clears++;
}

static void fake_process_nan(void *state, const float *in_left, const float *in_right, float *out_left,
			     float *out_right, size_t frames)
{
	(void)state;
	(void)in_left;
	(void)in_right;
	for (size_t i = 0; i < frames; i++) {
		out_left[i] = make_nan();
		if (out_right)
			out_right[i] = make_nan();
	}
}

static const struct vfx_param_info fake_params[1] = {
	{"level", "Fake.Level", 0.0, 1.0, 0.5, 0.1, NULL},
};

static struct vfx_effect_info make_fake_info(void)
{
	struct vfx_effect_info info = {
		.id = "fake",
		.name_key = "Fake.Name",
		.mix_mode = VFX_MIX_ADDITIVE,
		.params = fake_params,
		.num_params = 1,
		.create = fake_create_ok,
		.destroy = fake_destroy,
		.set_params = fake_set_params,
		.clear = fake_clear,
		.process = fake_process_nan,
	};
	return info;
}

static void test_effect_info_validation(void)
{
	printf("vfx_effect_info_is_valid (descripciones correctas e incorrectas)\n");

	struct vfx_effect_info ok = make_fake_info();
	CHECK(vfx_effect_info_is_valid(&ok));
	CHECK(!vfx_effect_info_is_valid(NULL));

	struct vfx_effect_info bad = ok;
	bad.id = NULL;
	CHECK(!vfx_effect_info_is_valid(&bad));

	bad = ok;
	bad.process = NULL;
	CHECK(!vfx_effect_info_is_valid(&bad));

	bad = ok;
	bad.num_params = VFX_MAX_PARAMS + 1;
	CHECK(!vfx_effect_info_is_valid(&bad));

	bad = ok;
	bad.mix_mode = (enum vfx_mix_mode)7;
	CHECK(!vfx_effect_info_is_valid(&bad));

	/* Rangos invalidos y claves repetidas. */
	struct vfx_param_info params[2];
	params[0] = fake_params[0];
	params[1] = fake_params[0];
	bad = ok;
	bad.params = params;
	bad.num_params = 2;
	CHECK(!vfx_effect_info_is_valid(&bad)); /* clave "level" repetida */

	params[1].key = "other";
	CHECK(vfx_effect_info_is_valid(&bad));

	params[1].default_value = 5.0; /* fuera de [0, 1] */
	CHECK(!vfx_effect_info_is_valid(&bad));

	params[1].default_value = 0.5;
	params[1].min = 2.0; /* min > max */
	CHECK(!vfx_effect_info_is_valid(&bad));

	params[1].min = 0.0;
	params[1].step = 0.0;
	CHECK(!vfx_effect_info_is_valid(&bad));

	params[1].step = 0.1;
	params[1].max = (double)make_nan();
	CHECK(!vfx_effect_info_is_valid(&bad));
}

static void test_all_effects_follow_contract(void)
{
	size_t count = 0;
	const struct vfx_effect_info *const *effects = vfx_get_effects(&count);

	printf("todos los efectos de la lista cumplen el contrato (%zu efecto/s)\n", count);
	CHECK(count > 0);

	for (size_t e = 0; e < count; e++) {
		const struct vfx_effect_info *info = effects[e];
		CHECK(vfx_effect_info_is_valid(info));

		/* Los id de filtro no se pueden repetir entre efectos. */
		for (size_t k = 0; k < e; k++)
			CHECK(strcmp(info->id, effects[k]->id) != 0);

		const uint32_t rates[] = {44100u, 48000u};
		for (size_t r = 0; r < 2; r++) {
			struct vfx_processor *p = malloc(sizeof(*p));
			CHECK(p != NULL);
			if (!p)
				continue;

			CHECK(vfx_processor_init(p, info, rates[r]));

			/* 2 segundos de ruido con algunos valores invalidos en la entrada. */
			static float l[TEST_BLOCK], rr[TEST_BLOCK];
			bool finite = true;
			for (int b = 0; b < 20; b++) {
				for (int i = 0; i < TEST_BLOCK; i++) {
					l[i] = rand_range(-0.9f, 0.9f);
					rr[i] = rand_range(-0.9f, 0.9f);
				}
				if (b == 3)
					l[10] = make_nan();
				vfx_processor_run(p, l, rr, TEST_BLOCK);
				for (int i = 0; i < TEST_BLOCK; i++) {
					if ((b != 3 || i != 10) && (!isfinite(l[i]) || !isfinite(rr[i])))
						finite = false;
				}
			}
			CHECK(finite);

			/* Parametros basura: se corrigen solos, sin romper nada. */
			float junk[VFX_MAX_PARAMS];
			for (int i = 0; i < VFX_MAX_PARAMS; i++)
				junk[i] = (i % 2) ? make_nan() : 1.0e9f;
			vfx_processor_set_params(p, junk);
			vfx_processor_run(p, l, rr, TEST_BLOCK);
			CHECK(all_finite(l, TEST_BLOCK));

			vfx_processor_free(p);
			vfx_processor_free(p); /* dos veces: no debe romper */
			free(p);
		}
	}
}

/* Copia el audio, lo procesa y devuelve si quedo bit a bit igual. */
static bool run_and_compare(struct vfx_processor *p, size_t frames, bool stereo)
{
	float *l = malloc(frames * sizeof(float));
	float *r = malloc(frames * sizeof(float));
	float *l0 = malloc(frames * sizeof(float));
	float *r0 = malloc(frames * sizeof(float));
	bool same = false;

	if (l && r && l0 && r0) {
		for (size_t i = 0; i < frames; i++) {
			l[i] = l0[i] = rand_range(-1.5f, 1.5f);
			r[i] = r0[i] = rand_range(-1.5f, 1.5f);
		}
		vfx_processor_run(p, l, stereo ? r : NULL, frames);
		same = memcmp(l, l0, frames * sizeof(float)) == 0 && memcmp(r, r0, frames * sizeof(float)) == 0;
	}

	free(l);
	free(r);
	free(l0);
	free(r0);
	return same;
}

static void test_processor_voice_untouched(void)
{
	printf("motor + reverb: con cantidad 0 %%, la voz sale bit a bit identica\n");
	struct vfx_processor *p = malloc(sizeof(*p));
	CHECK(p != NULL);
	if (!p)
		return;

	CHECK(vfx_processor_init(p, &vfx_effect_reverb, TEST_RATE));

	/* Orden de los deslizadores de la reverb: cantidad, sala, amortiguacion, ancho. */
	float values[4] = {0.0f, 80.0f, 30.0f, 100.0f};
	vfx_processor_set_params(p, values);

	/* Tamanos de bloque raros para probar el corte en bloques internos. */
	const size_t sizes[] = {1, 7, 1023, 1024, 1025, 4800, 10000};
	bool all_same = true;
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		all_same = all_same && run_and_compare(p, sizes[i], true);
		all_same = all_same && run_and_compare(p, sizes[i], false);
	}
	CHECK(all_same);

	/* Con cantidad normal, el audio cambia (hay reverb) y respeta el techo. */
	values[0] = 20.0f;
	vfx_processor_set_params(p, values);

	static float l[TEST_BLOCK], r[TEST_BLOCK], l0[TEST_BLOCK];
	bool changed = false, within = true;
	for (int b = 0; b < 10; b++) {
		for (int i = 0; i < TEST_BLOCK; i++) {
			l[i] = l0[i] = rand_range(-1.2f, 1.2f);
			r[i] = rand_range(-1.2f, 1.2f);
		}
		vfx_processor_run(p, l, r, TEST_BLOCK);
		for (int i = 0; i < TEST_BLOCK; i++) {
			float limit = fabsf(l0[i]) > VFX_CEILING ? fabsf(l0[i]) : VFX_CEILING;
			if (l[i] != l0[i])
				changed = true;
			if (fabsf(l[i]) > limit)
				within = false;
		}
	}
	CHECK(changed);
	CHECK(within);

	vfx_processor_free(p);
	free(p);
}

static void test_processor_failures(void)
{
	printf("motor: efecto que no se crea o que produce NaN deja pasar la voz\n");
	struct vfx_processor *p = malloc(sizeof(*p));
	CHECK(p != NULL);
	if (!p)
		return;

	/* Efecto que no se puede crear. */
	struct vfx_effect_info fails = make_fake_info();
	fails.create = fake_create_fails;
	CHECK(vfx_processor_init(p, &fails, TEST_RATE) == false);
	CHECK(run_and_compare(p, 3000, true));
	vfx_processor_free(p);

	/* Descripcion invalida: tampoco se crea. */
	struct vfx_effect_info invalid = make_fake_info();
	invalid.clear = NULL;
	CHECK(vfx_processor_init(p, &invalid, TEST_RATE) == false);
	CHECK(run_and_compare(p, 3000, true));
	vfx_processor_free(p);

	/* Efecto que produce NaN: pasa la voz, se avisa y se reinicia. */
	struct vfx_effect_info nan_fx = make_fake_info();
	g_fake_clears = 0;
	CHECK(vfx_processor_init(p, &nan_fx, TEST_RATE));

	static float l[3000], r[3000], l0[3000], r0[3000];
	for (int i = 0; i < 3000; i++) {
		l[i] = l0[i] = rand_range(-1.0f, 1.0f);
		r[i] = r0[i] = rand_range(-1.0f, 1.0f);
	}
	CHECK(vfx_processor_run(p, l, r, 3000) == true);
	CHECK(memcmp(l, l0, sizeof(l)) == 0);
	CHECK(memcmp(r, r0, sizeof(r)) == 0);
	CHECK(g_fake_clears == 3); /* 3000 muestras = 3 bloques, se reinicia en cada uno */

	vfx_processor_free(p);
	free(p);
}

static void test_reverb_level(void)
{
	printf("reverb con valores por defecto: nivel del efecto respecto de la voz\n");
	struct vfx_processor *p = malloc(sizeof(*p));
	CHECK(p != NULL);
	if (!p)
		return;
	CHECK(vfx_processor_init(p, &vfx_effect_reverb, TEST_RATE));

	/* Ruido a -20 dBFS durante 3 segundos; se mide el ultimo segundo. */
	static float l[TEST_BLOCK], r[TEST_BLOCK], l0[TEST_BLOCK];
	double dry_e = 0.0, wet_e = 0.0;
	for (int b = 0; b < 30; b++) {
		for (int i = 0; i < TEST_BLOCK; i++) {
			l[i] = l0[i] = rand_range(-0.17f, 0.17f);
			r[i] = rand_range(-0.17f, 0.17f);
		}
		vfx_processor_run(p, l, r, TEST_BLOCK);
		if (b >= 20) {
			for (int i = 0; i < TEST_BLOCK; i++) {
				double wet = (double)l[i] - (double)l0[i];
				dry_e += (double)l0[i] * (double)l0[i];
				wet_e += wet * wet;
			}
		}
	}

	double ratio_db = 10.0 * log10(wet_e / dry_e);
	printf("  reverb a %.1f dB respecto de la voz\n", ratio_db);
	CHECK(ratio_db < 0.0);   /* la reverb queda por debajo de la voz */
	CHECK(ratio_db > -30.0); /* pero se escucha */

	vfx_processor_free(p);
	free(p);
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

	test_effect_info_validation();
	test_all_effects_follow_contract();
	test_processor_voice_untouched();
	test_processor_failures();
	test_reverb_level();

	printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
