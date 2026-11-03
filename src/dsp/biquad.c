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

#include "dsp/biquad.h"

#include <math.h>

#define VFX_PI 3.14159265358979323846

static void set_passthrough(struct vfx_biquad *f)
{
	f->b0 = 1.0f;
	f->b1 = 0.0f;
	f->b2 = 0.0f;
	f->a1 = 0.0f;
	f->a2 = 0.0f;
}

void vfx_biquad_init(struct vfx_biquad *f)
{
	set_passthrough(f);
	vfx_biquad_clear(f);
}

void vfx_biquad_clear(struct vfx_biquad *f)
{
	f->z1 = 0.0f;
	f->z2 = 0.0f;
}

bool vfx_biquad_set(struct vfx_biquad *f, enum vfx_biquad_type type, double sample_rate, double freq, double q,
		    double gain_db)
{
	/* Validacion: ante cualquier duda, el filtro no toca el audio. */
	if (!isfinite(sample_rate) || !isfinite(freq) || !isfinite(q) || !isfinite(gain_db) || sample_rate <= 0.0 ||
	    freq < VFX_BIQUAD_MIN_FREQ || freq > sample_rate * VFX_BIQUAD_MAX_FREQ_RATIO || q < VFX_BIQUAD_MIN_Q ||
	    q > VFX_BIQUAD_MAX_Q || fabs(gain_db) > VFX_BIQUAD_MAX_GAIN_DB) {
		set_passthrough(f);
		return false;
	}

	const double w0 = 2.0 * VFX_PI * freq / sample_rate;
	const double cosw = cos(w0);
	const double alpha = sin(w0) / (2.0 * q);
	const double A = pow(10.0, gain_db / 40.0); /* raiz de la ganancia lineal */
	const double sqA2alpha = 2.0 * sqrt(A) * alpha;

	double b0, b1, b2, a0, a1, a2;

	switch (type) {
	case VFX_BIQUAD_LOWPASS:
		b0 = (1.0 - cosw) / 2.0;
		b1 = 1.0 - cosw;
		b2 = (1.0 - cosw) / 2.0;
		a0 = 1.0 + alpha;
		a1 = -2.0 * cosw;
		a2 = 1.0 - alpha;
		break;
	case VFX_BIQUAD_HIGHPASS:
		b0 = (1.0 + cosw) / 2.0;
		b1 = -(1.0 + cosw);
		b2 = (1.0 + cosw) / 2.0;
		a0 = 1.0 + alpha;
		a1 = -2.0 * cosw;
		a2 = 1.0 - alpha;
		break;
	case VFX_BIQUAD_BANDPASS:
		b0 = alpha;
		b1 = 0.0;
		b2 = -alpha;
		a0 = 1.0 + alpha;
		a1 = -2.0 * cosw;
		a2 = 1.0 - alpha;
		break;
	case VFX_BIQUAD_PEAK:
		b0 = 1.0 + alpha * A;
		b1 = -2.0 * cosw;
		b2 = 1.0 - alpha * A;
		a0 = 1.0 + alpha / A;
		a1 = -2.0 * cosw;
		a2 = 1.0 - alpha / A;
		break;
	case VFX_BIQUAD_LOWSHELF:
		b0 = A * ((A + 1.0) - (A - 1.0) * cosw + sqA2alpha);
		b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw);
		b2 = A * ((A + 1.0) - (A - 1.0) * cosw - sqA2alpha);
		a0 = (A + 1.0) + (A - 1.0) * cosw + sqA2alpha;
		a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw);
		a2 = (A + 1.0) + (A - 1.0) * cosw - sqA2alpha;
		break;
	case VFX_BIQUAD_HIGHSHELF:
		b0 = A * ((A + 1.0) + (A - 1.0) * cosw + sqA2alpha);
		b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw);
		b2 = A * ((A + 1.0) + (A - 1.0) * cosw - sqA2alpha);
		a0 = (A + 1.0) - (A - 1.0) * cosw + sqA2alpha;
		a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw);
		a2 = (A + 1.0) - (A - 1.0) * cosw - sqA2alpha;
		break;
	default:
		set_passthrough(f);
		return false;
	}

	const double nb0 = b0 / a0, nb1 = b1 / a0, nb2 = b2 / a0, na1 = a1 / a0, na2 = a2 / a0;

	if (!isfinite(nb0) || !isfinite(nb1) || !isfinite(nb2) || !isfinite(na1) || !isfinite(na2)) {
		set_passthrough(f);
		return false;
	}

	f->b0 = (float)nb0;
	f->b1 = (float)nb1;
	f->b2 = (float)nb2;
	f->a1 = (float)na1;
	f->a2 = (float)na2;
	return true;
}
