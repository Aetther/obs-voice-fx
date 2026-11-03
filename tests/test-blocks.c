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
 * Pruebas de las piezas comunes de audio (src/dsp/). No necesitan OBS.
 *
 * Ejecutar:  ctest --test-dir build -C Release --output-on-failure
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "dsp/biquad.h"
#include "dsp/delay-line.h"

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
static uint32_t g_rng = 0x9e3779b9u;

static float rand_range(float min, float max)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	float t = (float)(g_rng >> 8) / (float)(1u << 24);
	return min + (max - min) * t;
}

/* ------------------------------------------------------------------------- */
/* dsp/biquad.h                                                               */

#define TEST_RATE 48000.0
#define TEST_PI 3.14159265358979323846

/* Pasa un seno por el filtro y devuelve el pico de la salida ya estabilizada. */
static float biquad_gain_at(struct vfx_biquad *f, double freq)
{
	vfx_biquad_clear(f);
	const int total = 48000; /* 1 segundo */
	float peak = 0.0f;
	for (int n = 0; n < total; n++) {
		float x = (float)sin(2.0 * TEST_PI * freq * (double)n / TEST_RATE);
		float y = vfx_biquad_process(f, x);
		if (n >= total / 2 && fabsf(y) > peak)
			peak = fabsf(y);
	}
	return peak;
}

static bool approx(float value, float expected, float tolerance)
{
	return fabsf(value - expected) <= tolerance;
}

static void test_biquad_passthrough_and_invalid(void)
{
	printf("biquad: sin cambios y valores invalidos\n");
	struct vfx_biquad f;
	vfx_biquad_init(&f);

	bool same = true;
	for (int i = 0; i < 1000; i++) {
		float x = rand_range(-1.0f, 1.0f);
		if (vfx_biquad_process(&f, x) != x)
			same = false;
	}
	CHECK(same);

	/* Cada valor invalido deja el filtro en "sin cambios". */
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, TEST_RATE, 0.0, 0.707, 0.0));
	CHECK(f.b0 == 1.0f && f.b1 == 0.0f && f.b2 == 0.0f && f.a1 == 0.0f && f.a2 == 0.0f);
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, TEST_RATE, 30000.0, 0.707, 0.0));
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, TEST_RATE, (double)make_nan(), 0.707, 0.0));
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, TEST_RATE, 1000.0, 0.0, 0.0));
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, TEST_RATE, 1000.0, (double)make_inf(), 0.0));
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_PEAK, TEST_RATE, 1000.0, 1.0, 40.0));
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, 0.0, 1000.0, 0.707, 0.0));
	CHECK(!vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, (double)make_nan(), 1000.0, 0.707, 0.0));
	CHECK(!vfx_biquad_set(&f, (enum vfx_biquad_type)99, TEST_RATE, 1000.0, 0.707, 0.0));
	CHECK(f.b0 == 1.0f && f.a1 == 0.0f);
}

static void test_biquad_responses(void)
{
	printf("biquad: respuesta de cada tipo de filtro\n");
	struct vfx_biquad f;
	vfx_biquad_init(&f);

	/* Paso bajo a 1 kHz: graves pasan, agudos se cortan. */
	CHECK(vfx_biquad_set(&f, VFX_BIQUAD_LOWPASS, TEST_RATE, 1000.0, 0.707, 0.0));
	CHECK(approx(biquad_gain_at(&f, 100.0), 1.0f, 0.02f));
	CHECK(approx(biquad_gain_at(&f, 1000.0), 0.707f, 0.03f));
	CHECK(biquad_gain_at(&f, 10000.0) < 0.02f);

	/* Paso alto a 1 kHz: al reves. */
	CHECK(vfx_biquad_set(&f, VFX_BIQUAD_HIGHPASS, TEST_RATE, 1000.0, 0.707, 0.0));
	CHECK(biquad_gain_at(&f, 100.0) < 0.02f);
	CHECK(approx(biquad_gain_at(&f, 1000.0), 0.707f, 0.03f));
	CHECK(approx(biquad_gain_at(&f, 10000.0), 1.0f, 0.02f));

	/* Pasa banda en 1 kHz: solo pasa el centro, a 0 dB. */
	CHECK(vfx_biquad_set(&f, VFX_BIQUAD_BANDPASS, TEST_RATE, 1000.0, 2.0, 0.0));
	CHECK(approx(biquad_gain_at(&f, 1000.0), 1.0f, 0.02f));
	CHECK(biquad_gain_at(&f, 100.0) < 0.1f);
	CHECK(biquad_gain_at(&f, 10000.0) < 0.1f);

	/* Pico de +6 dB en 1 kHz: sube el centro (x2) y deja el resto igual. */
	CHECK(vfx_biquad_set(&f, VFX_BIQUAD_PEAK, TEST_RATE, 1000.0, 1.0, 6.0));
	CHECK(approx(biquad_gain_at(&f, 1000.0), 1.995f, 0.03f));
	CHECK(approx(biquad_gain_at(&f, 50.0), 1.0f, 0.03f));
	CHECK(approx(biquad_gain_at(&f, 15000.0), 1.0f, 0.03f));

	/* Pico de -6 dB: baja el centro a la mitad. */
	CHECK(vfx_biquad_set(&f, VFX_BIQUAD_PEAK, TEST_RATE, 1000.0, 1.0, -6.0));
	CHECK(approx(biquad_gain_at(&f, 1000.0), 0.501f, 0.02f));

	/* Estante grave +6 dB bajo 200 Hz. */
	CHECK(vfx_biquad_set(&f, VFX_BIQUAD_LOWSHELF, TEST_RATE, 200.0, 0.707, 6.0));
	CHECK(approx(biquad_gain_at(&f, 30.0), 1.995f, 0.05f));
	CHECK(approx(biquad_gain_at(&f, 5000.0), 1.0f, 0.03f));

	/* Estante agudo -6 dB sobre 3 kHz. */
	CHECK(vfx_biquad_set(&f, VFX_BIQUAD_HIGHSHELF, TEST_RATE, 3000.0, 0.707, -6.0));
	CHECK(approx(biquad_gain_at(&f, 18000.0), 0.501f, 0.03f));
	CHECK(approx(biquad_gain_at(&f, 100.0), 1.0f, 0.03f));
}

static void test_biquad_stability(void)
{
	printf("biquad: estable con valores al azar\n");
	struct vfx_biquad f;
	vfx_biquad_init(&f);

	bool ok = true;
	const double rates[] = {8000.0, 44100.0, 48000.0, 96000.0, 192000.0};

	for (int round = 0; round < 200 && ok; round++) {
		double rate = rates[round % 5];
		enum vfx_biquad_type type = (enum vfx_biquad_type)(round % 6);
		double freq = (double)rand_range(10.0f, (float)(rate * 0.45));
		double q = (double)rand_range(0.1f, 20.0f);
		double gain = (double)rand_range(-24.0f, 24.0f);

		/* Se cambian los coeficientes sin borrar la memoria, como al mover un deslizador. */
		if (!vfx_biquad_set(&f, type, rate, freq, q, gain))
			ok = false;

		for (int n = 0; n < 4800; n++) {
			float y = vfx_biquad_process(&f, rand_range(-1.0f, 1.0f));
			/* Con Q 20 y +24 dB la ganancia maxima ronda x320: el limite es holgado. */
			if (!isfinite(y) || fabsf(y) > 2000.0f) {
				ok = false;
				break;
			}
		}
	}
	CHECK(ok);

	/* Tras borrar la memoria, el silencio da silencio exacto. */
	vfx_biquad_clear(&f);
	bool silent = true;
	for (int n = 0; n < 1000; n++)
		if (vfx_biquad_process(&f, 0.0f) != 0.0f)
			silent = false;
	CHECK(silent);
}

/* ------------------------------------------------------------------------- */
/* dsp/delay-line.h                                                           */

static void test_delay_line_init(void)
{
	printf("delay-line: reserva de memoria\n");
	struct vfx_delay_line d;

	CHECK(!vfx_delay_line_init(&d, 0));
	CHECK(d.buffer == NULL);
	CHECK(!vfx_delay_line_init(&d, VFX_DELAY_LINE_MAX_SAMPLES + 1));
	CHECK(d.buffer == NULL);

	/* Una linea que fallo o sin iniciar no hace nada y devuelve silencio. */
	vfx_delay_line_push(&d, 0.5f);
	CHECK(vfx_delay_line_read(&d, 0) == 0.0f);
	CHECK(vfx_delay_line_read_frac(&d, 3.5f) == 0.0f);
	vfx_delay_line_clear(&d);
	vfx_delay_line_free(&d);
	vfx_delay_line_free(&d); /* liberar dos veces no rompe nada */

	CHECK(vfx_delay_line_init(&d, 100));
	CHECK(d.buffer != NULL);
	CHECK(d.mask + 1 >= 102);
	CHECK(((d.mask + 1) & d.mask) == 0); /* potencia de 2 */
	vfx_delay_line_free(&d);
	CHECK(d.buffer == NULL);
}

static void test_delay_line_read(void)
{
	printf("delay-line: lectura exacta, limites y vuelta del buffer\n");
	struct vfx_delay_line d;
	CHECK(vfx_delay_line_init(&d, 100));

	/* Se guardan mas muestras que el tamano para probar la vuelta del buffer. */
	const int total = 1000;
	for (int i = 1; i <= total; i++)
		vfx_delay_line_push(&d, (float)i);

	CHECK(vfx_delay_line_read(&d, 0) == (float)total);
	CHECK(vfx_delay_line_read(&d, 1) == (float)(total - 1));
	CHECK(vfx_delay_line_read(&d, 100) == (float)(total - 100));

	/* Retardos fuera de rango se limitan al maximo: nunca se lee fuera. */
	CHECK(vfx_delay_line_read(&d, 101) == (float)(total - 100));
	CHECK(vfx_delay_line_read(&d, (size_t)-1) == (float)(total - 100));

	/* Fraccionario: interpola entre las vecinas. */
	CHECK(vfx_delay_line_read_frac(&d, 2.0f) == (float)(total - 2));
	CHECK(approx(vfx_delay_line_read_frac(&d, 2.5f), (float)total - 2.5f, 0.001f));
	CHECK(approx(vfx_delay_line_read_frac(&d, 10.25f), (float)total - 10.25f, 0.001f));

	/* Valores danados: se limitan, sin leer fuera de la memoria. */
	CHECK(vfx_delay_line_read_frac(&d, make_nan()) == (float)total);
	CHECK(vfx_delay_line_read_frac(&d, -5.0f) == (float)total);
	CHECK(vfx_delay_line_read_frac(&d, make_inf()) == (float)(total - 100));
	CHECK(vfx_delay_line_read_frac(&d, 1.0e9f) == (float)(total - 100));
	CHECK(vfx_delay_line_read_frac(&d, 99.9f) <= (float)(total - 99));

	/* Borrar deja silencio. */
	vfx_delay_line_clear(&d);
	bool silent = true;
	for (size_t k = 0; k <= 100; k++)
		if (vfx_delay_line_read(&d, k) != 0.0f)
			silent = false;
	CHECK(silent);

	vfx_delay_line_free(&d);
}

static void test_delay_line_echo(void)
{
	printf("delay-line: un impulso sale exactamente N muestras despues\n");
	struct vfx_delay_line d;
	CHECK(vfx_delay_line_init(&d, 48000));

	const size_t delay = 12000; /* 250 ms a 48 kHz */
	size_t found_at = 0;
	int found_count = 0;

	for (size_t n = 0; n < 20000; n++) {
		vfx_delay_line_push(&d, n == 0 ? 1.0f : 0.0f);
		if (vfx_delay_line_read(&d, delay) != 0.0f) {
			found_at = n;
			found_count++;
		}
	}
	CHECK(found_count == 1);
	CHECK(found_at == delay);

	vfx_delay_line_free(&d);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
	test_biquad_passthrough_and_invalid();
	test_biquad_responses();
	test_biquad_stability();

	test_delay_line_init();
	test_delay_line_read();
	test_delay_line_echo();

	printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
