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

#include "dsp/delay-line.h"

#include <stdlib.h>
#include <string.h>

bool vfx_delay_line_init(struct vfx_delay_line *d, size_t max_delay)
{
	memset(d, 0, sizeof(*d));

	if (max_delay == 0 || max_delay > VFX_DELAY_LINE_MAX_SAMPLES)
		return false;

	/*
	 * Tamano: la potencia de 2 que alcance para max_delay + 2 muestras
	 * (la actual, el retardo maximo y una mas para interpolar).
	 */
	size_t size = 1;
	while (size < max_delay + 2)
		size <<= 1;

	/* calloc verifica el desbordamiento de size * sizeof(float) y llena con ceros. */
	d->buffer = calloc(size, sizeof(float));
	if (!d->buffer)
		return false;

	d->mask = size - 1;
	d->write = 0;
	d->max_delay = max_delay;
	return true;
}

void vfx_delay_line_free(struct vfx_delay_line *d)
{
	free(d->buffer);
	memset(d, 0, sizeof(*d));
}

void vfx_delay_line_clear(struct vfx_delay_line *d)
{
	if (d->buffer)
		memset(d->buffer, 0, (d->mask + 1) * sizeof(float));
	d->write = 0;
}
