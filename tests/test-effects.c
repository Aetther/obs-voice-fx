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
 * Pruebas del comportamiento de cada efecto (lo que el usuario escucha).
 * No necesitan OBS.
 *
 * Ejecutar:  ctest --test-dir build -C Release --output-on-failure
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/effect.h"
#include "effects/effects-list.h"
#include "effects/reverb/freeverb.h"

/* ------------------------------------------------------------------------- */
/* Mini sistema de pruebas (igual que en test-dsp.c)                          */

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

/* Generador pseudoaleatorio simple y reproducible (xorshift32). */
static uint32_t g_rng = 0x2545f491u;

static float rand_range(float min, float max)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	float t = (float)(g_rng >> 8) / (float)(1u << 24);
	return min + (max - min) * t;
}

#define RATE 48000u
#define BLOCK 480
#define TEST_PI 3.14159265358979323846

/* ------------------------------------------------------------------------- */
/* Reverb                                                                     */

/* Mismo orden que en reverb.c. */
enum { R_AMOUNT, R_ROOM, R_DAMPING, R_WIDTH, R_PREDELAY, R_LOW_CUT, R_HIGH_CUT, R_COUNT };

static void reverb_defaults(float *values)
{
	const struct vfx_effect_info *info = &vfx_effect_reverb;
	for (size_t i = 0; i < info->num_params; i++)
		values[i] = (float)info->params[i].default_value;
}

/* Busca un deslizador por su clave (para no depender solo del orden). */
static const struct vfx_param_info *reverb_param(const char *key)
{
	const struct vfx_effect_info *info = &vfx_effect_reverb;
	for (size_t i = 0; i < info->num_params; i++)
		if (strcmp(info->params[i].key, key) == 0)
			return &info->params[i];
	return NULL;
}

static void test_reverb_params_layout(void)
{
	printf("reverb: deslizadores y valores neutros\n");
	CHECK(vfx_effect_reverb.num_params == R_COUNT);
	CHECK(vfx_effect_info_is_valid(&vfx_effect_reverb));

	/* Las claves de la version 0.1.0 no pueden cambiar (se perderia la configuracion). */
	CHECK(reverb_param("amount") == &vfx_effect_reverb.params[R_AMOUNT]);
	CHECK(reverb_param("room_size") == &vfx_effect_reverb.params[R_ROOM]);
	CHECK(reverb_param("damping") == &vfx_effect_reverb.params[R_DAMPING]);
	CHECK(reverb_param("width") == &vfx_effect_reverb.params[R_WIDTH]);
	CHECK(reverb_param("predelay") == &vfx_effect_reverb.params[R_PREDELAY]);
	CHECK(reverb_param("low_cut") == &vfx_effect_reverb.params[R_LOW_CUT]);
	CHECK(reverb_param("high_cut") == &vfx_effect_reverb.params[R_HIGH_CUT]);

	/* Los extras arrancan en neutro: pre-delay 0, cortes en sus extremos. */
	const struct vfx_param_info *pre = reverb_param("predelay");
	const struct vfx_param_info *low = reverb_param("low_cut");
	const struct vfx_param_info *high = reverb_param("high_cut");
	CHECK(pre && pre->default_value == 0.0);
	CHECK(low && low->default_value == low->min);
	CHECK(high && high->default_value == high->max);
}

/*
 * Lo mas importante: con los extras en neutro, la reverb da EXACTAMENTE lo
 * mismo que la de la version 0.1.0 (freeverb directo), muestra por muestra.
 */
static void test_reverb_neutral_is_identical(void)
{
	printf("reverb: con extras en neutro suena identica a la version 0.1.0\n");

	for (int stereo = 0; stereo <= 1; stereo++) {
		void *state = vfx_effect_reverb.create(RATE);
		CHECK(state != NULL);
		if (!state)
			return;

		float values[R_COUNT];
		reverb_defaults(values);
		values[R_AMOUNT] = 35.0f;
		values[R_ROOM] = 92.0f;
		values[R_DAMPING] = 55.0f;
		vfx_effect_reverb.set_params(state, values);

		struct freeverb fv;
		CHECK(freeverb_init(&fv, RATE));
		freeverb_set_params(&fv, 0.92f, 0.55f, 1.0f, 0.35f);

		float in_l[BLOCK], in_r[BLOCK], a_l[BLOCK], a_r[BLOCK], b_l[BLOCK], b_r[BLOCK];
		bool identical = true;

		for (int block = 0; block < 200; block++) {
			for (int n = 0; n < BLOCK; n++) {
				in_l[n] = rand_range(-0.5f, 0.5f);
				in_r[n] = rand_range(-0.5f, 0.5f);
			}
			vfx_effect_reverb.process(state, in_l, stereo ? in_r : NULL, a_l, stereo ? a_r : NULL, BLOCK);
			freeverb_process_wet(&fv, in_l, stereo ? in_r : NULL, b_l, stereo ? b_r : NULL, BLOCK);

			if (memcmp(a_l, b_l, sizeof(a_l)) != 0)
				identical = false;
			if (stereo && memcmp(a_r, b_r, sizeof(a_r)) != 0)
				identical = false;
		}
		CHECK(identical);

		freeverb_free(&fv);
		vfx_effect_reverb.destroy(state);
	}
}

/* Primera muestra (de las primeras "total") en la que la reverb deja de ser silencio. */
static size_t reverb_onset(float predelay_ms)
{
	void *state = vfx_effect_reverb.create(RATE);
	if (!state)
		return 0;

	float values[R_COUNT];
	reverb_defaults(values);
	values[R_PREDELAY] = predelay_ms;
	vfx_effect_reverb.set_params(state, values);

	float in[BLOCK], out_l[BLOCK], out_r[BLOCK];
	size_t onset = (size_t)-1;

	for (size_t block = 0; block < 40 && onset == (size_t)-1; block++) {
		for (int n = 0; n < BLOCK; n++)
			in[n] = (block == 0 && n == 0) ? 1.0f : 0.0f;
		vfx_effect_reverb.process(state, in, in, out_l, out_r, BLOCK);
		for (size_t n = 0; n < BLOCK; n++) {
			if (out_l[n] != 0.0f || out_r[n] != 0.0f) {
				onset = block * BLOCK + n;
				break;
			}
		}
	}

	vfx_effect_reverb.destroy(state);
	return onset;
}

static void test_reverb_predelay(void)
{
	printf("reverb: el pre-delay retrasa el eco exactamente\n");
	const size_t base = reverb_onset(0.0f);
	CHECK(base != (size_t)-1);
	CHECK(reverb_onset(50.0f) == base + 2400);  /* 50 ms a 48 kHz */
	CHECK(reverb_onset(100.0f) == base + 4800); /* el maximo */
}

/* Energia (RMS) de la reverb para un seno de entrada, con los valores dados. */
static float reverb_rms_for_sine(const float *values, double freq)
{
	void *state = vfx_effect_reverb.create(RATE);
	if (!state)
		return -1.0f;
	vfx_effect_reverb.set_params(state, values);

	float in[BLOCK], out_l[BLOCK], out_r[BLOCK];
	double sum = 0.0;
	size_t count = 0;

	for (int block = 0; block < 100; block++) {
		for (int n = 0; n < BLOCK; n++) {
			double t = (double)(block * BLOCK + n) / (double)RATE;
			in[n] = 0.5f * (float)sin(2.0 * TEST_PI * freq * t);
		}
		vfx_effect_reverb.process(state, in, in, out_l, out_r, BLOCK);
		if (block >= 50) { /* medir con la cola ya formada */
			for (int n = 0; n < BLOCK; n++) {
				sum += (double)out_l[n] * (double)out_l[n];
				count++;
			}
		}
	}

	vfx_effect_reverb.destroy(state);
	return (float)sqrt(sum / (double)count);
}

static void test_reverb_cuts(void)
{
	printf("reverb: los cortes quitan graves y agudos del eco\n");
	float values[R_COUNT];
	reverb_defaults(values);
	values[R_AMOUNT] = 50.0f;

	/* Agudos (8 kHz): el corte de agudos en 1 kHz los baja mucho. */
	const float high_open = reverb_rms_for_sine(values, 8000.0);
	values[R_HIGH_CUT] = 1000.0f;
	const float high_cut = reverb_rms_for_sine(values, 8000.0);
	values[R_HIGH_CUT] = 20000.0f;
	CHECK(high_open > 0.0f);
	CHECK(high_cut < high_open * 0.1f);

	/* Graves (60 Hz): el corte de graves en 800 Hz los baja mucho. */
	const float low_open = reverb_rms_for_sine(values, 60.0);
	values[R_LOW_CUT] = 800.0f;
	const float low_cut = reverb_rms_for_sine(values, 60.0);
	values[R_LOW_CUT] = 20.0f;
	CHECK(low_open > 0.0f);
	CHECK(low_cut < low_open * 0.1f);

	/* La zona media de la voz (1 kHz) casi no cambia con cortes moderados. */
	const float mid_open = reverb_rms_for_sine(values, 1000.0);
	values[R_LOW_CUT] = 150.0f;
	values[R_HIGH_CUT] = 8000.0f;
	const float mid_cut = reverb_rms_for_sine(values, 1000.0);
	CHECK(mid_cut > mid_open * 0.85f);
}

static void test_reverb_extras_robust(void)
{
	printf("reverb: estable cambiando todo al azar, en varias frecuencias de muestreo\n");
	const uint32_t rates[] = {8000u, 44100u, 48000u, 96000u, 192000u, 384000u};
	bool ok = true;

	for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]) && ok; r++) {
		void *state = vfx_effect_reverb.create(rates[r]);
		if (!state) {
			ok = false;
			break;
		}

		float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
		for (int block = 0; block < 300 && ok; block++) {
			/* Cada 10 bloques, valores al azar dentro de cada rango (incluidos los extremos). */
			if (block % 10 == 0) {
				float values[R_COUNT];
				for (size_t i = 0; i < R_COUNT; i++) {
					const struct vfx_param_info *p = &vfx_effect_reverb.params[i];
					float v = rand_range((float)p->min, (float)p->max);
					if (block % 30 == 0)
						v = (float)p->min;
					if (block % 30 == 10)
						v = (float)p->max;
					values[i] = v;
				}
				vfx_effect_reverb.set_params(state, values);
			}

			for (int n = 0; n < BLOCK; n++) {
				in_l[n] = rand_range(-1.0f, 1.0f);
				in_r[n] = rand_range(-1.0f, 1.0f);
			}
			vfx_effect_reverb.process(state, in_l, in_r, out_l, out_r, BLOCK);

			for (int n = 0; n < BLOCK; n++) {
				if (!isfinite(out_l[n]) || !isfinite(out_r[n]) || fabsf(out_l[n]) > 50.0f ||
				    fabsf(out_r[n]) > 50.0f) {
					ok = false;
					break;
				}
			}
		}

		vfx_effect_reverb.destroy(state);
	}
	CHECK(ok);
}

static void test_reverb_clear_with_extras(void)
{
	printf("reverb: al reactivar no quedan restos (tampoco en el pre-delay)\n");
	void *state = vfx_effect_reverb.create(RATE);
	CHECK(state != NULL);
	if (!state)
		return;

	float values[R_COUNT];
	reverb_defaults(values);
	values[R_PREDELAY] = 80.0f;
	values[R_LOW_CUT] = 200.0f;
	values[R_HIGH_CUT] = 5000.0f;
	vfx_effect_reverb.set_params(state, values);

	float in[BLOCK], out_l[BLOCK], out_r[BLOCK];
	for (int block = 0; block < 50; block++) {
		for (int n = 0; n < BLOCK; n++)
			in[n] = rand_range(-1.0f, 1.0f);
		vfx_effect_reverb.process(state, in, in, out_l, out_r, BLOCK);
	}

	vfx_effect_reverb.clear(state);

	memset(in, 0, sizeof(in));
	bool silent = true;
	for (int block = 0; block < 20; block++) {
		vfx_effect_reverb.process(state, in, in, out_l, out_r, BLOCK);
		for (int n = 0; n < BLOCK; n++)
			if (out_l[n] != 0.0f || out_r[n] != 0.0f)
				silent = false;
	}
	CHECK(silent);

	vfx_effect_reverb.destroy(state);
}


/* ------------------------------------------------------------------------- */
/* Eco                                                                        */

/* Mismo orden que en echo.c. */
enum { E_AMOUNT, E_TIME, E_FEEDBACK, E_TONE, E_WIDTH, E_COUNT };

#define ECHO_LEN 24000 /* medio segundo a 48 kHz */

/* Valores "limpios" para medir: cantidad 100 %, sin repeticiones, sin tono. */
static void echo_clean_values(float *values, float time_ms, float feedback, float width)
{
	values[E_AMOUNT] = 100.0f;
	values[E_TIME] = time_ms;
	values[E_FEEDBACK] = feedback;
	values[E_TONE] = 20000.0f;
	values[E_WIDTH] = width;
}

/* Pasa un impulso (en el canal izquierdo, o en los dos) y guarda la salida. */
static bool echo_impulse(const float *values, bool both_channels, float *out_l, float *out_r, size_t len)
{
	void *state = vfx_effect_echo.create(RATE);
	if (!state)
		return false;
	vfx_effect_echo.set_params(state, values);

	float in_l[BLOCK], in_r[BLOCK];
	for (size_t done = 0; done < len; done += BLOCK) {
		for (int n = 0; n < BLOCK; n++) {
			in_l[n] = (done == 0 && n == 0) ? 1.0f : 0.0f;
			in_r[n] = both_channels ? in_l[n] : 0.0f;
		}
		vfx_effect_echo.process(state, in_l, in_r, out_l + done, out_r + done, BLOCK);
	}

	vfx_effect_echo.destroy(state);
	return true;
}

static bool only_nonzero_at(const float *buf, size_t len, const size_t *positions, size_t count)
{
	for (size_t i = 0; i < len; i++) {
		bool expected = false;
		for (size_t k = 0; k < count; k++)
			if (positions[k] == i)
				expected = true;
		if (!expected && buf[i] != 0.0f)
			return false;
	}
	return true;
}

static void test_echo_params_layout(void)
{
	printf("eco: deslizadores validos y en el desplegable\n");
	CHECK(vfx_effect_echo.num_params == E_COUNT);
	CHECK(vfx_effect_info_is_valid(&vfx_effect_echo));
	CHECK(vfx_effect_echo.mix_mode == VFX_MIX_ADDITIVE);
	CHECK(strcmp(vfx_effect_echo.params[E_AMOUNT].key, "amount") == 0);
	CHECK(strcmp(vfx_effect_echo.params[E_TIME].key, "time") == 0);
	CHECK(strcmp(vfx_effect_echo.params[E_FEEDBACK].key, "feedback") == 0);
	CHECK(strcmp(vfx_effect_echo.params[E_TONE].key, "tone") == 0);
	CHECK(strcmp(vfx_effect_echo.params[E_WIDTH].key, "width") == 0);
	CHECK(vfx_effect_echo.params[E_FEEDBACK].max <= 90.0); /* el eco siempre se apaga */

	/* Esta en la lista de efectos (y por lo tanto en el panel y en los atajos). */
	size_t count = 0;
	const struct vfx_effect_info *const *effects = vfx_get_effects(&count);
	bool listed = false;
	for (size_t i = 0; i < count; i++)
		if (effects[i] == &vfx_effect_echo)
			listed = true;
	CHECK(listed);

	/* Frecuencias fuera de rango: no se crea (la voz pasa sin cambios). */
	CHECK(vfx_effect_echo.create(7999u) == NULL);
	CHECK(vfx_effect_echo.create(400000u) == NULL);
}

static void test_echo_timing(void)
{
	printf("eco: la repeticion llega exactamente a tiempo\n");
	static float l[ECHO_LEN], r[ECHO_LEN];
	float values[E_COUNT];

	/* 100 ms, una sola repeticion: un unico 1.0 en la muestra 4800. */
	echo_clean_values(values, 100.0f, 0.0f, 0.0f);
	CHECK(echo_impulse(values, true, l, r, ECHO_LEN));
	const size_t at_100[] = {4800};
	CHECK(l[4800] == 1.0f && r[4800] == 1.0f);
	CHECK(only_nonzero_at(l, ECHO_LEN, at_100, 1));
	CHECK(only_nonzero_at(r, ECHO_LEN, at_100, 1));

	/* 20 ms (el minimo): muestra 960. */
	echo_clean_values(values, 20.0f, 0.0f, 0.0f);
	CHECK(echo_impulse(values, true, l, r, ECHO_LEN));
	const size_t at_20[] = {960};
	CHECK(l[960] == 1.0f);
	CHECK(only_nonzero_at(l, ECHO_LEN, at_20, 1));
}

static void test_echo_feedback(void)
{
	printf("eco: cada repeticion es mas suave que la anterior\n");
	static float l[ECHO_LEN], r[ECHO_LEN];
	float values[E_COUNT];

	/* 100 ms con 50 % de repeticiones: 1, 0.5, 0.25, 0.125... */
	echo_clean_values(values, 100.0f, 50.0f, 0.0f);
	CHECK(echo_impulse(values, true, l, r, ECHO_LEN));
	CHECK(l[4800] == 1.0f);
	CHECK(l[9600] == 0.5f);
	CHECK(l[14400] == 0.25f);
	CHECK(l[19200] == 0.125f);
	const size_t at[] = {4800, 9600, 14400, 19200};
	CHECK(only_nonzero_at(l, ECHO_LEN, at, 4));

	/* Con tono, las repeticiones pierden brillo: el pico de cada una baja mas. */
	values[E_TONE] = 2000.0f;
	CHECK(echo_impulse(values, true, l, r, ECHO_LEN));
	float peak1 = 0.0f, peak2 = 0.0f;
	for (size_t i = 4700; i < 9500; i++)
		peak1 = fabsf(l[i]) > peak1 ? fabsf(l[i]) : peak1;
	for (size_t i = 9500; i < 14300; i++)
		peak2 = fabsf(l[i]) > peak2 ? fabsf(l[i]) : peak2;
	CHECK(peak1 > 0.0f && peak1 < 1.0f);
	CHECK(peak2 < peak1 * 0.5f);
}

static void test_echo_ping_pong(void)
{
	printf("eco: con ancho 100 %% las repeticiones alternan izquierda / derecha\n");
	static float l[ECHO_LEN], r[ECHO_LEN];
	float values[E_COUNT];

	echo_clean_values(values, 100.0f, 50.0f, 100.0f);
	CHECK(echo_impulse(values, true, l, r, ECHO_LEN));

	/* 1a repeticion a la izquierda, 2a a la derecha, 3a a la izquierda... */
	CHECK(l[4800] == 1.0f && r[4800] == 0.0f);
	CHECK(l[9600] == 0.0f && r[9600] == 0.5f);
	CHECK(l[14400] == 0.25f && r[14400] == 0.0f);
	CHECK(l[19200] == 0.0f && r[19200] == 0.125f);
}

static void test_echo_amount_and_mono(void)
{
	printf("eco: cantidad 0 = silencio exacto; audio mono\n");
	void *state = vfx_effect_echo.create(RATE);
	CHECK(state != NULL);
	if (!state)
		return;

	float values[E_COUNT];
	echo_clean_values(values, 50.0f, 60.0f, 100.0f);
	values[E_AMOUNT] = 0.0f;
	vfx_effect_echo.set_params(state, values);

	float in[BLOCK], out_l[BLOCK], out_r[BLOCK];
	bool silent = true;
	for (int block = 0; block < 50; block++) {
		for (int n = 0; n < BLOCK; n++)
			in[n] = rand_range(-1.0f, 1.0f);
		vfx_effect_echo.process(state, in, in, out_l, out_r, BLOCK);
		for (int n = 0; n < BLOCK; n++)
			if (out_l[n] != 0.0f || out_r[n] != 0.0f)
				silent = false;
	}
	CHECK(silent);

	/* Mono (sin canal derecho): hay eco y es finito. */
	values[E_AMOUNT] = 50.0f;
	vfx_effect_echo.set_params(state, values);
	float peak = 0.0f;
	bool finite = true;
	for (int block = 0; block < 50; block++) {
		for (int n = 0; n < BLOCK; n++)
			in[n] = rand_range(-1.0f, 1.0f);
		vfx_effect_echo.process(state, in, NULL, out_l, NULL, BLOCK);
		for (int n = 0; n < BLOCK; n++) {
			if (!isfinite(out_l[n]))
				finite = false;
			if (fabsf(out_l[n]) > peak)
				peak = fabsf(out_l[n]);
		}
	}
	CHECK(finite);
	CHECK(peak > 0.0f);

	vfx_effect_echo.destroy(state);
}

static void test_echo_dies_out(void)
{
	printf("eco: con repeticiones al maximo, se apaga solo\n");
	void *state = vfx_effect_echo.create(RATE);
	CHECK(state != NULL);
	if (!state)
		return;

	/* El peor caso: repeticiones 90 %, sin tono, tiempo minimo, ping-pong a medias. */
	float values[E_COUNT];
	echo_clean_values(values, 20.0f, 90.0f, 50.0f);
	vfx_effect_echo.set_params(state, values);

	float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
	float peak_loud = 0.0f;
	for (int block = 0; block < 100; block++) { /* 1 s de ruido a todo volumen */
		for (int n = 0; n < BLOCK; n++) {
			in_l[n] = rand_range(-1.0f, 1.0f);
			in_r[n] = rand_range(-1.0f, 1.0f);
		}
		vfx_effect_echo.process(state, in_l, in_r, out_l, out_r, BLOCK);
		for (int n = 0; n < BLOCK; n++) {
			if (fabsf(out_l[n]) > peak_loud)
				peak_loud = fabsf(out_l[n]);
			if (fabsf(out_r[n]) > peak_loud)
				peak_loud = fabsf(out_r[n]);
		}
	}
	printf("  pico del eco con ruido a todo volumen: %.2f\n", (double)peak_loud);
	CHECK(peak_loud < 12.0f); /* acotado: como mucho 1 / (1 - 0.9) = 10 veces */

	memset(in_l, 0, sizeof(in_l));
	memset(in_r, 0, sizeof(in_r));
	float peak_after = 0.0f;
	for (int block = 0; block < 1000; block++) { /* 10 s de silencio */
		vfx_effect_echo.process(state, in_l, in_r, out_l, out_r, BLOCK);
		if (block >= 990)
			for (int n = 0; n < BLOCK; n++)
				peak_after = fmaxf(peak_after, fmaxf(fabsf(out_l[n]), fabsf(out_r[n])));
	}
	CHECK(peak_after < 1.0e-6f);

	vfx_effect_echo.destroy(state);
}

static void test_echo_robust_and_clear(void)
{
	printf("eco: estable cambiando todo al azar; al reactivar no quedan restos\n");
	const uint32_t rates[] = {8000u, 44100u, 48000u, 96000u, 192000u, 384000u};
	bool ok = true;

	for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]) && ok; r++) {
		void *state = vfx_effect_echo.create(rates[r]);
		if (!state) {
			ok = false;
			break;
		}

		float in_l[BLOCK], in_r[BLOCK], out_l[BLOCK], out_r[BLOCK];
		for (int block = 0; block < 300 && ok; block++) {
			if (block % 7 == 0) {
				float values[E_COUNT];
				for (size_t i = 0; i < E_COUNT; i++) {
					const struct vfx_param_info *p = &vfx_effect_echo.params[i];
					float v = rand_range((float)p->min, (float)p->max);
					if (block % 21 == 0)
						v = (float)p->min;
					if (block % 21 == 7)
						v = (float)p->max;
					values[i] = v;
				}
				vfx_effect_echo.set_params(state, values);
			}

			for (int n = 0; n < BLOCK; n++) {
				in_l[n] = rand_range(-1.0f, 1.0f);
				in_r[n] = rand_range(-1.0f, 1.0f);
			}
			vfx_effect_echo.process(state, in_l, in_r, out_l, out_r, BLOCK);
			for (int n = 0; n < BLOCK; n++) {
				if (!isfinite(out_l[n]) || !isfinite(out_r[n]) || fabsf(out_l[n]) > 12.0f ||
				    fabsf(out_r[n]) > 12.0f) {
					ok = false;
					break;
				}
			}
		}

		/* Al reactivar: silencio exacto, sin repeticiones viejas. */
		vfx_effect_echo.clear(state);
		memset(in_l, 0, sizeof(in_l));
		memset(in_r, 0, sizeof(in_r));
		for (int block = 0; block < 10 && ok; block++) {
			vfx_effect_echo.process(state, in_l, in_r, out_l, out_r, BLOCK);
			for (int n = 0; n < BLOCK; n++)
				if (out_l[n] != 0.0f || out_r[n] != 0.0f)
					ok = false;
		}

		vfx_effect_echo.destroy(state);
	}
	CHECK(ok);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
	test_reverb_params_layout();
	test_reverb_neutral_is_identical();
	test_reverb_predelay();
	test_reverb_cuts();
	test_reverb_extras_robust();
	test_reverb_clear_with_extras();

	test_echo_params_layout();
	test_echo_timing();
	test_echo_feedback();
	test_echo_ping_pong();
	test_echo_amount_and_mono();
	test_echo_dies_out();
	test_echo_robust_and_clear();

	printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
