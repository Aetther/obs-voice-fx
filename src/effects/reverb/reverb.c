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
 * Efecto "Reverb": conecta el algoritmo de freeverb.c con el contrato de
 * core/effect.h. Aca se describen los deslizadores, se traducen sus valores
 * y se aplican los extras sobre la reverb ya calculada:
 *
 *   - Pre-delay: un pequeno silencio entre la voz y el comienzo del eco
 *     (da sensacion de lugar grande sin subir la cantidad).
 *   - Corte de graves y de agudos del eco: quitan lo que "embarra" (graves)
 *     y lo que suena metalico o "a tuberia" (agudos).
 *
 * Los extras solo tocan la reverb, nunca la voz.
 *
 * Valores neutros: pre-delay 0 ms, corte de graves en su minimo y corte de
 * agudos en su maximo. En esos valores el extra se saltea por completo, asi
 * la reverb suena EXACTAMENTE igual que en la version 0.1.0 para quien
 * actualiza sin tocar nada.
 */

#include "core/effect.h"
#include "dsp/biquad.h"
#include "dsp/delay-line.h"
#include "effects/effects-list.h"
#include "effects/reverb/freeverb.h"

#include <math.h>
#include <stdlib.h>

/* Orden de los deslizadores (tambien es el orden en pantalla). */
enum {
	P_AMOUNT,
	P_ROOM_SIZE,
	P_DAMPING,
	P_WIDTH,
	P_PREDELAY,
	P_LOW_CUT,
	P_HIGH_CUT,
	P_COUNT,
};

#define PREDELAY_MAX_MS 100.0
#define LOW_CUT_MIN_HZ 20.0
#define LOW_CUT_MAX_HZ 1000.0
#define HIGH_CUT_MIN_HZ 1000.0
#define HIGH_CUT_MAX_HZ 20000.0

/* Q de 0.707: corte suave, sin resonancia (no agrega "color"). */
#define CUT_Q 0.7071

static const struct vfx_param_info reverb_params[P_COUNT] = {
	/* {clave, texto, minimo, maximo, por defecto, paso, sufijo} */
	[P_AMOUNT] = {"amount", "Reverb.Amount", 0.0, 100.0, 20.0, 1.0, " %"},
	[P_ROOM_SIZE] = {"room_size", "Reverb.RoomSize", 0.0, 100.0, 50.0, 1.0, " %"},
	[P_DAMPING] = {"damping", "Reverb.Damping", 0.0, 100.0, 50.0, 1.0, " %"},
	[P_WIDTH] = {"width", "Reverb.Width", 0.0, 100.0, 100.0, 1.0, " %"},
	[P_PREDELAY] = {"predelay", "Reverb.PreDelay", 0.0, PREDELAY_MAX_MS, 0.0, 1.0, " ms"},
	[P_LOW_CUT] = {"low_cut", "Reverb.LowCut", LOW_CUT_MIN_HZ, LOW_CUT_MAX_HZ, LOW_CUT_MIN_HZ, 10.0, " Hz"},
	[P_HIGH_CUT] = {"high_cut", "Reverb.HighCut", HIGH_CUT_MIN_HZ, HIGH_CUT_MAX_HZ, HIGH_CUT_MAX_HZ, 100.0,
			" Hz"},
};

struct reverb_state {
	struct freeverb fv;
	uint32_t sample_rate;

	/* Pre-delay: una linea por canal; 0 muestras = desactivado. */
	struct vfx_delay_line predelay[2];
	size_t predelay_samples;

	/* Cortes de graves (paso alto) y agudos (paso bajo) sobre la reverb. */
	struct vfx_biquad low_cut[2];
	struct vfx_biquad high_cut[2];
	bool low_cut_on;
	bool high_cut_on;
};

static void reverb_destroy(void *state)
{
	struct reverb_state *st = state;
	if (!st)
		return;

	freeverb_free(&st->fv);
	vfx_delay_line_free(&st->predelay[0]);
	vfx_delay_line_free(&st->predelay[1]);
	free(st);
}

static void *reverb_create(uint32_t sample_rate)
{
	struct reverb_state *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	st->sample_rate = sample_rate;

	if (!freeverb_init(&st->fv, sample_rate)) {
		reverb_destroy(st);
		return NULL;
	}

	/* freeverb_init ya valido la frecuencia (8 a 384 kHz): el tamano es razonable. */
	const size_t max_predelay = (size_t)ceil(PREDELAY_MAX_MS * (double)sample_rate / 1000.0);
	if (!vfx_delay_line_init(&st->predelay[0], max_predelay) ||
	    !vfx_delay_line_init(&st->predelay[1], max_predelay)) {
		reverb_destroy(st);
		return NULL;
	}

	for (int c = 0; c < 2; c++) {
		vfx_biquad_init(&st->low_cut[c]);
		vfx_biquad_init(&st->high_cut[c]);
	}

	return st;
}

/* Frecuencia de corte segura: nunca mas alla de lo que admite el filtro a esta frecuencia de muestreo. */
static double safe_cut_freq(double freq, uint32_t sample_rate)
{
	const double limit = (double)sample_rate * VFX_BIQUAD_MAX_FREQ_RATIO;
	return freq > limit ? limit : freq;
}

static void reverb_set_params(void *state, const float *values)
{
	struct reverb_state *st = state;

	freeverb_set_params(&st->fv, values[P_ROOM_SIZE] / 100.0f, values[P_DAMPING] / 100.0f,
			    values[P_WIDTH] / 100.0f, values[P_AMOUNT] / 100.0f);

	/* Pre-delay en muestras (la linea de retardo limita igual al maximo). */
	st->predelay_samples = (size_t)lround((double)values[P_PREDELAY] * (double)st->sample_rate / 1000.0);

	/* Corte de graves: en su minimo queda apagado (valor neutro). */
	const double low = (double)values[P_LOW_CUT];
	bool low_on = low > LOW_CUT_MIN_HZ;
	for (int c = 0; c < 2 && low_on; c++)
		low_on = vfx_biquad_set(&st->low_cut[c], VFX_BIQUAD_HIGHPASS, (double)st->sample_rate,
					safe_cut_freq(low, st->sample_rate), CUT_Q, 0.0);
	if (!low_on && st->low_cut_on) {
		vfx_biquad_clear(&st->low_cut[0]);
		vfx_biquad_clear(&st->low_cut[1]);
	}
	st->low_cut_on = low_on;

	/* Corte de agudos: en su maximo queda apagado (valor neutro). */
	const double high = (double)values[P_HIGH_CUT];
	bool high_on = high < HIGH_CUT_MAX_HZ;
	for (int c = 0; c < 2 && high_on; c++)
		high_on = vfx_biquad_set(&st->high_cut[c], VFX_BIQUAD_LOWPASS, (double)st->sample_rate,
					 safe_cut_freq(high, st->sample_rate), CUT_Q, 0.0);
	if (!high_on && st->high_cut_on) {
		vfx_biquad_clear(&st->high_cut[0]);
		vfx_biquad_clear(&st->high_cut[1]);
	}
	st->high_cut_on = high_on;
}

static void reverb_clear(void *state)
{
	struct reverb_state *st = state;

	freeverb_clear(&st->fv);
	for (int c = 0; c < 2; c++) {
		vfx_delay_line_clear(&st->predelay[c]);
		vfx_biquad_clear(&st->low_cut[c]);
		vfx_biquad_clear(&st->high_cut[c]);
	}
}

/* Aplica pre-delay y cortes, en el lugar, sobre la reverb de un canal. */
static void apply_extras(struct reverb_state *st, int c, float *wet, size_t frames)
{
	const size_t predelay = st->predelay_samples;

	if (predelay == 0 && !st->low_cut_on && !st->high_cut_on)
		return; /* todo en neutro: la reverb queda intacta */

	for (size_t n = 0; n < frames; n++) {
		float x = wet[n];

		if (predelay > 0) {
			vfx_delay_line_push(&st->predelay[c], x);
			x = vfx_delay_line_read(&st->predelay[c], predelay);
		}
		if (st->low_cut_on)
			x = vfx_biquad_process(&st->low_cut[c], x);
		if (st->high_cut_on)
			x = vfx_biquad_process(&st->high_cut[c], x);

		wet[n] = x;
	}
}

static void reverb_process(void *state, const float *in_left, const float *in_right, float *out_left,
			   float *out_right, size_t frames)
{
	struct reverb_state *st = state;

	/*
	 * La reverb es un sistema lineal: retrasar o filtrar su salida es lo
	 * mismo que hacerlo con su entrada, y asi no hace falta memoria extra.
	 */
	freeverb_process_wet(&st->fv, in_left, in_right, out_left, out_right, frames);

	apply_extras(st, 0, out_left, frames);
	if (out_right)
		apply_extras(st, 1, out_right, frames);
}

const struct vfx_effect_info vfx_effect_reverb = {
	.id = "obs_voice_fx_reverb",
	.name_key = "Reverb.Name",
	.mix_mode = VFX_MIX_ADDITIVE,
	.params = reverb_params,
	.num_params = P_COUNT,
	.create = reverb_create,
	.destroy = reverb_destroy,
	.set_params = reverb_set_params,
	.clear = reverb_clear,
	.process = reverb_process,
};
