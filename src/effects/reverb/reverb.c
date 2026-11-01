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
 * core/effect.h. Aca solo se describen los deslizadores y se traducen sus
 * valores (0-100 %) a los que usa el algoritmo (0-1).
 */

#include "core/effect.h"
#include "effects/effects-list.h"
#include "effects/reverb/freeverb.h"

#include <stdlib.h>

/* Orden de los deslizadores (tambien es el orden en pantalla). */
enum {
	P_AMOUNT,
	P_ROOM_SIZE,
	P_DAMPING,
	P_WIDTH,
	P_COUNT,
};

static const struct vfx_param_info reverb_params[P_COUNT] = {
	/* {clave, texto, minimo, maximo, por defecto, paso, sufijo} */
	[P_AMOUNT] = {"amount", "Reverb.Amount", 0.0, 100.0, 20.0, 1.0, " %"},
	[P_ROOM_SIZE] = {"room_size", "Reverb.RoomSize", 0.0, 100.0, 50.0, 1.0, " %"},
	[P_DAMPING] = {"damping", "Reverb.Damping", 0.0, 100.0, 50.0, 1.0, " %"},
	[P_WIDTH] = {"width", "Reverb.Width", 0.0, 100.0, 100.0, 1.0, " %"},
};

static void *reverb_create(uint32_t sample_rate)
{
	struct freeverb *fv = malloc(sizeof(*fv));
	if (!fv)
		return NULL;

	if (!freeverb_init(fv, sample_rate)) {
		freeverb_free(fv);
		free(fv);
		return NULL;
	}

	return fv;
}

static void reverb_destroy(void *state)
{
	struct freeverb *fv = state;
	if (!fv)
		return;

	freeverb_free(fv);
	free(fv);
}

static void reverb_set_params(void *state, const float *values)
{
	freeverb_set_params(state, values[P_ROOM_SIZE] / 100.0f, values[P_DAMPING] / 100.0f,
			    values[P_WIDTH] / 100.0f, values[P_AMOUNT] / 100.0f);
}

static void reverb_clear(void *state)
{
	freeverb_clear(state);
}

static void reverb_process(void *state, const float *in_left, const float *in_right, float *out_left,
			   float *out_right, size_t frames)
{
	freeverb_process_wet(state, in_left, in_right, out_left, out_right, frames);
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
