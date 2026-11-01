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

#include "effects/reverb/freeverb.h"
#include "core/safety.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Constantes del algoritmo Freeverb (afinadas originalmente para 44.1 kHz)   */

#define FV_FIXED_GAIN 0.015f       /* atenua la entrada para que la cola no sature */
#define FV_SCALE_WET 3.0f          /* escala de la parte mojada */
#define FV_SCALE_DAMP 0.4f         /* escala de la amortiguacion */
#define FV_SCALE_ROOM 0.28f        /* escala del tamano de sala */
#define FV_OFFSET_ROOM 0.7f        /* tamano minimo de sala */
#define FV_ALLPASS_FEEDBACK 0.5f   /* realimentacion de los allpass */
#define FV_STEREO_SPREAD 23        /* diferencia entre canal izquierdo y derecho */
#define FV_REFERENCE_RATE 44100.0  /* frecuencia para la que estan las medidas */

/*
 * Realimentacion maxima posible: 1.0 * 0.28 + 0.7 = 0.98.
 * Siempre menor que 1, por eso la cola SIEMPRE se apaga sola y nunca crece
 * sin control, aunque el tamano de sala este al maximo.
 */

/* Largo de cada filtro en muestras a 44.1 kHz (valores originales de Freeverb). */
static const unsigned comb_tuning[FREEVERB_NUM_COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const unsigned allpass_tuning[FREEVERB_NUM_ALLPASSES] = {556, 441, 341, 225};

/* ------------------------------------------------------------------------- */
/* Lineas de retardo                                                          */

static size_t scaled_length(unsigned samples_at_44k, uint32_t sample_rate)
{
	double len = (double)samples_at_44k * (double)sample_rate / FV_REFERENCE_RATE;
	return len < 1.0 ? 1 : (size_t)len;
}

static bool delay_alloc(struct freeverb_delay *d, size_t size)
{
	/* calloc verifica el desbordamiento de size * sizeof(float) y llena con ceros. */
	d->buffer = calloc(size, sizeof(float));
	d->size = d->buffer ? size : 0;
	d->pos = 0;
	return d->buffer != NULL;
}

static void delay_free(struct freeverb_delay *d)
{
	free(d->buffer);
	d->buffer = NULL;
	d->size = 0;
	d->pos = 0;
}

static void delay_clear(struct freeverb_delay *d)
{
	if (d->buffer)
		memset(d->buffer, 0, d->size * sizeof(float));
	d->pos = 0;
}

static inline void delay_advance(struct freeverb_delay *d)
{
	d->pos++;
	if (d->pos >= d->size)
		d->pos = 0;
}

/* ------------------------------------------------------------------------- */
/* Filtros                                                                    */

static inline float comb_process(struct freeverb_comb *c, float input, float feedback, float damp1, float damp2)
{
	struct freeverb_delay *d = &c->delay;
	float output = d->buffer[d->pos];

	c->store = vfx_flush_denormal(output * damp2 + c->store * damp1);
	d->buffer[d->pos] = vfx_flush_denormal(input + c->store * feedback);

	delay_advance(d);
	return output;
}

static inline float allpass_process(struct freeverb_delay *d, float input)
{
	float buffered = d->buffer[d->pos];
	float output = buffered - input;

	d->buffer[d->pos] = vfx_flush_denormal(input + buffered * FV_ALLPASS_FEEDBACK);

	delay_advance(d);
	return output;
}

static inline float channel_process(struct freeverb_channel *ch, float input, float feedback, float damp1,
				    float damp2)
{
	float out = 0.0f;

	for (int i = 0; i < FREEVERB_NUM_COMBS; i++)
		out += comb_process(&ch->combs[i], input, feedback, damp1, damp2);

	for (int i = 0; i < FREEVERB_NUM_ALLPASSES; i++)
		out = allpass_process(&ch->allpasses[i], out);

	return out;
}

/* ------------------------------------------------------------------------- */
/* API publica                                                                */

bool freeverb_init(struct freeverb *fv, uint32_t sample_rate)
{
	memset(fv, 0, sizeof(*fv));

	if (sample_rate < FREEVERB_MIN_SAMPLE_RATE || sample_rate > FREEVERB_MAX_SAMPLE_RATE)
		return false;

	fv->sample_rate = sample_rate;

	for (int c = 0; c < 2; c++) {
		/* El canal derecho es un poco mas largo: eso crea la sensacion estereo. */
		unsigned spread = (c == 0) ? 0u : (unsigned)FV_STEREO_SPREAD;
		struct freeverb_channel *ch = &fv->channels[c];

		for (int i = 0; i < FREEVERB_NUM_COMBS; i++) {
			if (!delay_alloc(&ch->combs[i].delay, scaled_length(comb_tuning[i] + spread, sample_rate))) {
				freeverb_free(fv);
				return false;
			}
		}

		for (int i = 0; i < FREEVERB_NUM_ALLPASSES; i++) {
			if (!delay_alloc(&ch->allpasses[i], scaled_length(allpass_tuning[i] + spread, sample_rate))) {
				freeverb_free(fv);
				return false;
			}
		}
	}

	freeverb_set_params(fv, 0.5f, 0.5f, 1.0f, 0.2f);
	fv->ready = true;
	return true;
}

void freeverb_free(struct freeverb *fv)
{
	for (int c = 0; c < 2; c++) {
		struct freeverb_channel *ch = &fv->channels[c];

		for (int i = 0; i < FREEVERB_NUM_COMBS; i++) {
			delay_free(&ch->combs[i].delay);
			ch->combs[i].store = 0.0f;
		}

		for (int i = 0; i < FREEVERB_NUM_ALLPASSES; i++)
			delay_free(&ch->allpasses[i]);
	}

	fv->ready = false;
}

void freeverb_clear(struct freeverb *fv)
{
	for (int c = 0; c < 2; c++) {
		struct freeverb_channel *ch = &fv->channels[c];

		for (int i = 0; i < FREEVERB_NUM_COMBS; i++) {
			delay_clear(&ch->combs[i].delay);
			ch->combs[i].store = 0.0f;
		}

		for (int i = 0; i < FREEVERB_NUM_ALLPASSES; i++)
			delay_clear(&ch->allpasses[i]);
	}
}

void freeverb_set_params(struct freeverb *fv, float room_size, float damping, float width, float amount)
{
	fv->room_size = vfx_param_clamp((double)room_size, 0.0, 1.0, 0.5);
	fv->damping = vfx_param_clamp((double)damping, 0.0, 1.0, 0.5);
	fv->width = vfx_param_clamp((double)width, 0.0, 1.0, 1.0);
	fv->amount = vfx_param_clamp((double)amount, 0.0, 1.0, 0.2);

	float scaled_wet = fv->amount * FV_SCALE_WET;

	fv->feedback = fv->room_size * FV_SCALE_ROOM + FV_OFFSET_ROOM;
	fv->damp1 = fv->damping * FV_SCALE_DAMP;
	fv->damp2 = 1.0f - fv->damp1;
	fv->wet1 = scaled_wet * (fv->width / 2.0f + 0.5f);
	fv->wet2 = scaled_wet * ((1.0f - fv->width) / 2.0f);
}

void freeverb_process_wet(struct freeverb *fv, const float *in_left, const float *in_right, float *wet_left,
			  float *wet_right, size_t frames)
{
	if (!wet_left)
		return;

	/* Sin reverb lista o sin entrada: silencio (la voz pasa igual, sin efecto). */
	if (!fv->ready || !in_left) {
		memset(wet_left, 0, frames * sizeof(float));
		if (wet_right)
			memset(wet_right, 0, frames * sizeof(float));
		return;
	}

	const bool stereo = (in_right != NULL && wet_right != NULL);
	const float feedback = fv->feedback;
	const float damp1 = fv->damp1;
	const float damp2 = fv->damp2;
	const float wet1 = fv->wet1;
	const float wet2 = fv->wet2;

	for (size_t n = 0; n < frames; n++) {
		float l = vfx_sanitize_input(in_left[n]);
		float r = stereo ? vfx_sanitize_input(in_right[n]) : l;
		float input = (l + r) * FV_FIXED_GAIN;

		float out_l = channel_process(&fv->channels[0], input, feedback, damp1, damp2);

		if (stereo) {
			float out_r = channel_process(&fv->channels[1], input, feedback, damp1, damp2);
			wet_left[n] = out_l * wet1 + out_r * wet2;
			wet_right[n] = out_r * wet1 + out_l * wet2;
		} else {
			wet_left[n] = out_l * (wet1 + wet2);
			if (wet_right)
				wet_right[n] = wet_left[n];
		}
	}
}
