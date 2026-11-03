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
 * Efecto "Eco": repite la voz cada cierto tiempo, cada vez mas suave.
 *
 *   entrada --> (+) --> [ tono ] --> linea de retardo --+--> eco (se suma a la voz)
 *                ^                                     |
 *                +--------- x repeticiones <-----------+
 *
 * - Tono: un paso bajo antes de la linea de retardo, asi cada repeticion
 *   sale un poco mas apagada que la anterior (como un eco real).
 * - Ancho estereo: en 0 cada canal tiene su eco; al subirlo, las
 *   repeticiones van alternando entre izquierda y derecha ("ping-pong").
 * - Repeticiones como maximo 90 %: cada vuelta pierde al menos un 10 %, asi
 *   el eco siempre se apaga solo y nunca se acumula sin fin.
 * - Al cambiar el tiempo mientras suena, el retardo se desliza en unos 80 ms
 *   en lugar de saltar (evita clics).
 *
 * El eco se SUMA por debajo de la voz (modo aditivo): la voz no cambia.
 */

#include "core/effect.h"
#include "core/safety.h"
#include "dsp/biquad.h"
#include "dsp/delay-line.h"
#include "effects/effects-list.h"

#include <math.h>
#include <stdlib.h>

/* Orden de los deslizadores (tambien es el orden en pantalla). */
enum {
	P_AMOUNT,
	P_TIME,
	P_FEEDBACK,
	P_TONE,
	P_WIDTH,
	P_COUNT,
};

#define TIME_MIN_MS 20.0
#define TIME_MAX_MS 2000.0
#define FEEDBACK_MAX 90.0
#define TONE_MIN_HZ 1000.0
#define TONE_MAX_HZ 20000.0

#define MIN_SAMPLE_RATE 8000u
#define MAX_SAMPLE_RATE 384000u

/* Tiempo que tarda el retardo en deslizarse hasta un tiempo nuevo. */
#define TIME_GLIDE_SECONDS 0.08

static const struct vfx_param_info echo_params[P_COUNT] = {
	/* {clave, texto, minimo, maximo, por defecto, paso, sufijo} */
	[P_AMOUNT] = {"amount", "Echo.Amount", 0.0, 100.0, 25.0, 1.0, " %"},
	[P_TIME] = {"time", "Echo.Time", TIME_MIN_MS, TIME_MAX_MS, 350.0, 10.0, " ms"},
	[P_FEEDBACK] = {"feedback", "Echo.Feedback", 0.0, FEEDBACK_MAX, 35.0, 1.0, " %"},
	[P_TONE] = {"tone", "Echo.Tone", TONE_MIN_HZ, TONE_MAX_HZ, 6000.0, 100.0, " Hz"},
	[P_WIDTH] = {"width", "Echo.Width", 0.0, 100.0, 0.0, 1.0, " %"},
};

struct echo_state {
	uint32_t sample_rate;

	struct vfx_delay_line line[2];
	struct vfx_biquad tone[2];
	bool tone_on;

	/* Valores ya traducidos para el hilo de audio. */
	float amount;   /* 0..1 */
	float feedback; /* 0..0.9 */
	float width;    /* 0..1 */

	float target_delay;  /* retardo pedido, en muestras */
	float current_delay; /* retardo que se usa ahora (se desliza hacia el pedido) */
	float glide;         /* cuanto se acerca por muestra (0..1) */
	bool delay_ready;    /* false hasta el primer set_params */
};

static void echo_destroy(void *state)
{
	struct echo_state *st = state;
	if (!st)
		return;

	vfx_delay_line_free(&st->line[0]);
	vfx_delay_line_free(&st->line[1]);
	free(st);
}

static void *echo_create(uint32_t sample_rate)
{
	/* Frecuencia fuera de rango: no se crea y la voz pasa sin cambios. */
	if (sample_rate < MIN_SAMPLE_RATE || sample_rate > MAX_SAMPLE_RATE)
		return NULL;

	struct echo_state *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	st->sample_rate = sample_rate;

	/* Como maximo 2 s (768.000 muestras a 384 kHz: unos 3 MB por canal). */
	const size_t max_delay = (size_t)ceil(TIME_MAX_MS * (double)sample_rate / 1000.0);
	if (!vfx_delay_line_init(&st->line[0], max_delay) || !vfx_delay_line_init(&st->line[1], max_delay)) {
		echo_destroy(st);
		return NULL;
	}

	vfx_biquad_init(&st->tone[0]);
	vfx_biquad_init(&st->tone[1]);

	st->glide = (float)(1.0 - exp(-1.0 / (TIME_GLIDE_SECONDS * (double)sample_rate)));
	return st;
}

static void echo_set_params(void *state, const float *values)
{
	struct echo_state *st = state;

	st->amount = values[P_AMOUNT] / 100.0f;
	st->feedback = values[P_FEEDBACK] / 100.0f;
	st->width = values[P_WIDTH] / 100.0f;

	/* Retardo en muestras, dentro de lo que admite la linea. */
	float delay = (float)((double)values[P_TIME] * (double)st->sample_rate / 1000.0);
	const float max_delay = (float)st->line[0].max_delay;
	if (delay < 1.0f)
		delay = 1.0f;
	if (delay > max_delay)
		delay = max_delay;
	st->target_delay = delay;

	/* La primera vez no hay nada que deslizar. */
	if (!st->delay_ready) {
		st->current_delay = delay;
		st->delay_ready = true;
	}

	/* Tono: en su maximo queda apagado (las repeticiones no se oscurecen). */
	const double tone = (double)values[P_TONE];
	const double limit = (double)st->sample_rate * VFX_BIQUAD_MAX_FREQ_RATIO;
	bool on = tone < TONE_MAX_HZ;
	for (int c = 0; c < 2 && on; c++)
		on = vfx_biquad_set(&st->tone[c], VFX_BIQUAD_LOWPASS, (double)st->sample_rate,
				    tone > limit ? limit : tone, 0.7071, 0.0);
	if (!on && st->tone_on) {
		vfx_biquad_clear(&st->tone[0]);
		vfx_biquad_clear(&st->tone[1]);
	}
	st->tone_on = on;
}

static void echo_clear(void *state)
{
	struct echo_state *st = state;

	for (int c = 0; c < 2; c++) {
		vfx_delay_line_clear(&st->line[c]);
		vfx_biquad_clear(&st->tone[c]);
	}
	st->current_delay = st->target_delay;
}

static inline float apply_tone(struct echo_state *st, int c, float x)
{
	return st->tone_on ? vfx_biquad_process(&st->tone[c], x) : x;
}

static void echo_process(void *state, const float *in_left, const float *in_right, float *out_left,
			 float *out_right, size_t frames)
{
	struct echo_state *st = state;

	const bool stereo = (in_right != NULL && out_right != NULL);
	const float amount = st->amount;
	const float fb = st->feedback;
	const float w = stereo ? st->width : 0.0f;

	for (size_t n = 0; n < frames; n++) {
		/* El retardo se acerca de a poco al tiempo elegido. */
		st->current_delay += (st->target_delay - st->current_delay) * st->glide;

		/*
		 * Se lee ANTES de guardar la muestra nueva: leer "d - 1" despues
		 * del ultimo guardado equivale a un retardo de exactamente d muestras.
		 */
		const float read_at = st->current_delay - 1.0f;
		const float l = vfx_sanitize_input(in_left[n]);

		if (!stereo) {
			const float y = vfx_delay_line_read_frac(&st->line[0], read_at);
			vfx_delay_line_push(&st->line[0], vfx_flush_denormal(apply_tone(st, 0, l + fb * y)));
			out_left[n] = y * amount;
			if (out_right)
				out_right[n] = out_left[n];
			continue;
		}

		const float r = vfx_sanitize_input(in_right[n]);
		const float yl = vfx_delay_line_read_frac(&st->line[0], read_at);
		const float yr = vfx_delay_line_read_frac(&st->line[1], read_at);

		/*
		 * Mezcla entre eco normal (cada canal se repite a si mismo) y
		 * ping-pong (la voz entra por la izquierda y las repeticiones
		 * cruzan de un lado al otro). Cada fila suma "fb" en total, asi
		 * la realimentacion nunca supera el 90 %.
		 */
		const float mono = 0.5f * (l + r);
		const float in_l = (1.0f - w) * (l + fb * yl) + w * (mono + fb * yr);
		const float in_r = (1.0f - w) * (r + fb * yr) + w * (fb * yl);

		vfx_delay_line_push(&st->line[0], vfx_flush_denormal(apply_tone(st, 0, in_l)));
		vfx_delay_line_push(&st->line[1], vfx_flush_denormal(apply_tone(st, 1, in_r)));

		out_left[n] = yl * amount;
		out_right[n] = yr * amount;
	}
}

const struct vfx_effect_info vfx_effect_echo = {
	.id = "obs_voice_fx_echo",
	.name_key = "Echo.Name",
	.mix_mode = VFX_MIX_ADDITIVE,
	.params = echo_params,
	.num_params = P_COUNT,
	.create = echo_create,
	.destroy = echo_destroy,
	.set_params = echo_set_params,
	.clear = echo_clear,
	.process = echo_process,
};
