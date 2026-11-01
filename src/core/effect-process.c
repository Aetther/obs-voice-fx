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

#include "core/effect-process.h"
#include "core/safety.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Validacion de la descripcion de un efecto                                  */

static bool param_is_valid(const struct vfx_param_info *p)
{
	if (!p->key || !p->key[0] || !p->text_key || !p->text_key[0])
		return false;
	if (!isfinite(p->min) || !isfinite(p->max) || !isfinite(p->default_value) || !isfinite(p->step))
		return false;
	if (!(p->min < p->max))
		return false;
	if (p->default_value < p->min || p->default_value > p->max)
		return false;
	if (!(p->step > 0.0))
		return false;
	return true;
}

bool vfx_effect_info_is_valid(const struct vfx_effect_info *info)
{
	if (!info)
		return false;
	if (!info->id || !info->id[0] || !info->name_key || !info->name_key[0])
		return false;
	if (info->mix_mode != VFX_MIX_ADDITIVE && info->mix_mode != VFX_MIX_REPLACE)
		return false;
	if (!info->create || !info->destroy || !info->set_params || !info->clear || !info->process)
		return false;
	if (info->num_params > VFX_MAX_PARAMS)
		return false;
	if (info->num_params > 0 && !info->params)
		return false;

	for (size_t i = 0; i < info->num_params; i++) {
		if (!param_is_valid(&info->params[i]))
			return false;

		/* Claves repetidas mezclarian la configuracion de dos deslizadores. */
		for (size_t j = 0; j < i; j++) {
			if (strcmp(info->params[i].key, info->params[j].key) == 0)
				return false;
		}
	}

	return true;
}

/* ------------------------------------------------------------------------- */
/* Procesador                                                                 */

bool vfx_processor_init(struct vfx_processor *p, const struct vfx_effect_info *info, uint32_t sample_rate)
{
	memset(p, 0, sizeof(*p));
	p->info = info;

	if (!vfx_effect_info_is_valid(info))
		return false;

	p->state = info->create(sample_rate);
	if (!p->state)
		return false;

	float defaults[VFX_MAX_PARAMS] = {0};
	for (size_t i = 0; i < info->num_params; i++)
		defaults[i] = (float)info->params[i].default_value;

	info->set_params(p->state, defaults);
	return true;
}

void vfx_processor_free(struct vfx_processor *p)
{
	if (p->info && p->state)
		p->info->destroy(p->state);
	p->state = NULL;
}

void vfx_processor_set_params(struct vfx_processor *p, const float *values)
{
	if (!p->state || !values)
		return;

	const struct vfx_effect_info *info = p->info;
	float safe[VFX_MAX_PARAMS] = {0};

	for (size_t i = 0; i < info->num_params; i++) {
		const struct vfx_param_info *param = &info->params[i];
		safe[i] = vfx_param_clamp((double)values[i], param->min, param->max, param->default_value);
	}

	info->set_params(p->state, safe);
}

void vfx_processor_clear(struct vfx_processor *p)
{
	if (p->state)
		p->info->clear(p->state);
}

bool vfx_processor_run(struct vfx_processor *p, float *left, float *right, size_t frames)
{
	/* Sin efecto o sin audio: la voz pasa exactamente igual. */
	if (!p->state || !left)
		return false;

	const struct vfx_effect_info *info = p->info;
	const bool additive = (info->mix_mode == VFX_MIX_ADDITIVE);
	bool fault = false;
	size_t done = 0;

	while (done < frames) {
		size_t n = frames - done;
		if (n > VFX_BLOCK_FRAMES)
			n = VFX_BLOCK_FRAMES;

		float *l = left + done;
		float *r = right ? right + done : NULL;

		info->process(p->state, l, r, p->scratch_left, r ? p->scratch_right : NULL, n);

		bool block_fault = false;

		for (size_t i = 0; i < n; i++) {
			l[i] = additive ? vfx_mix_additive(l[i], p->scratch_left[i], &block_fault)
					: vfx_mix_replace(l[i], p->scratch_left[i], &block_fault);
		}

		if (r) {
			for (size_t i = 0; i < n; i++) {
				r[i] = additive ? vfx_mix_additive(r[i], p->scratch_right[i], &block_fault)
						: vfx_mix_replace(r[i], p->scratch_right[i], &block_fault);
			}
		}

		/* Si el efecto se "rompio", se reinicia para el bloque siguiente. */
		if (block_fault) {
			info->clear(p->state);
			fault = true;
		}

		done += n;
	}

	return fault;
}
