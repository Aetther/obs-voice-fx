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
 * Reverb basada en el algoritmo Freeverb (Jezar at Dreampoint, dominio publico),
 * escrita desde cero en C para este proyecto.
 *
 * Por canal: 8 filtros "comb" en paralelo (las repeticiones que forman la cola)
 * seguidos de 4 filtros "allpass" en serie (difuminan las repeticiones).
 *
 * Este modulo NO conoce OBS y NO mezcla con la voz: solo calcula la parte
 * "mojada" (wet). La mezcla segura con la voz la hace core/safety.h.
 *
 * Uso:
 *   struct freeverb fv;
 *   freeverb_init(&fv, 48000);           // reserva memoria (fuera del hilo de audio)
 *   freeverb_set_params(&fv, ...);       // valores de 0 a 1
 *   freeverb_process_wet(&fv, ...);      // en el hilo de audio, sin reservar memoria
 *   freeverb_free(&fv);
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREEVERB_NUM_COMBS 8
#define FREEVERB_NUM_ALLPASSES 4

/* Frecuencias de muestreo aceptadas (evita reservas de memoria absurdas). */
#define FREEVERB_MIN_SAMPLE_RATE 8000u
#define FREEVERB_MAX_SAMPLE_RATE 384000u

/* Linea de retardo circular. */
struct freeverb_delay {
	float *buffer;
	size_t size;
	size_t pos;
};

struct freeverb_comb {
	struct freeverb_delay delay;
	float store; /* memoria del filtro de amortiguacion */
};

struct freeverb_channel {
	struct freeverb_comb combs[FREEVERB_NUM_COMBS];
	struct freeverb_delay allpasses[FREEVERB_NUM_ALLPASSES];
};

struct freeverb {
	struct freeverb_channel channels[2]; /* 0 = izquierdo, 1 = derecho */
	uint32_t sample_rate;
	bool ready;

	/* Parametros elegidos por el usuario (0 a 1). */
	float room_size;
	float damping;
	float width;
	float amount;

	/* Valores internos calculados a partir de los parametros. */
	float feedback;
	float damp1;
	float damp2;
	float wet1;
	float wet2;
};

/*
 * Prepara la reverb para una frecuencia de muestreo y reserva su memoria.
 * Devuelve false si la frecuencia esta fuera de rango o falta memoria;
 * en ese caso la reverb queda "apagada" y solo produce silencio.
 */
bool freeverb_init(struct freeverb *fv, uint32_t sample_rate);

/* Libera la memoria. Se puede llamar aunque init haya fallado. */
void freeverb_free(struct freeverb *fv);

/* Borra la cola de reverb (como si la sala quedara en silencio). */
void freeverb_clear(struct freeverb *fv);

/*
 * Cambia los parametros. Todos van de 0 a 1 y se limitan a ese rango.
 *   room_size: tamano de la sala (largo de la cola).
 *   damping:   amortiguacion de agudos (paredes mas "blandas").
 *   width:     ancho estereo (0 = mono, 1 = amplio).
 *   amount:    cantidad de reverb que se suma a la voz.
 */
void freeverb_set_params(struct freeverb *fv, float room_size, float damping, float width, float amount);

/*
 * Calcula solo la parte "mojada" (la reverb), sin la voz.
 *
 *   in_left:   audio de entrada, canal izquierdo (obligatorio).
 *   in_right:  canal derecho, o NULL si el audio es mono.
 *   wet_left:  donde se escribe la reverb del canal izquierdo (obligatorio).
 *   wet_right: reverb del canal derecho, o NULL si el audio es mono.
 *   frames:    cantidad de muestras por canal.
 *
 * Los valores invalidos de entrada (NaN, infinito) se tratan como silencio.
 * Si la reverb no esta lista, escribe silencio en la salida.
 */
void freeverb_process_wet(struct freeverb *fv, const float *in_left, const float *in_right, float *wet_left,
			  float *wet_right, size_t frames);

#ifdef __cplusplus
}
#endif
