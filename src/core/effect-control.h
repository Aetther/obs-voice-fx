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
 * Control de los efectos: prender, apagar y atajos de teclado.
 *
 * Cada efecto tiene UN atajo ("<Efecto>: activar/desactivar") que aparece en
 * Ajustes -> Atajos de OBS. Al presionarlo, el efecto se prende o se apaga en
 * todas las fuentes de audio donde este agregado.
 *
 * Las teclas elegidas se guardan en el archivo de configuracion del plugin
 * (plugin_config/obs-voice-fx/hotkeys.json), asi se mantienen aunque cambies
 * de perfil o de coleccion de escenas.
 *
 * Todas estas funciones se usan desde el hilo principal de OBS.
 */

#pragma once

#include "core/effect.h"

#include <obs.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registra un atajo por efecto y carga las teclas guardadas. Se llama desde obs_module_load(). */
void vfx_control_init(const struct vfx_effect_info *const *effects, size_t count);

/* Quita los atajos. Se llama desde obs_module_unload(). */
void vfx_control_shutdown(void);

/* Cantidad de fuentes que tienen este efecto agregado (prendido o apagado). */
size_t vfx_control_count_filters(const struct vfx_effect_info *info);

/* true si el efecto esta prendido en al menos una fuente. */
bool vfx_control_is_enabled(const struct vfx_effect_info *info);

/* Prende o apaga el efecto en todas las fuentes donde esta agregado. */
void vfx_control_set_enabled(const struct vfx_effect_info *info, bool enabled);

/* Si esta prendido en alguna fuente, lo apaga en todas; si no, lo prende en todas. */
void vfx_control_toggle(const struct vfx_effect_info *info);

/* true si la fuente es una entrada que produce audio (microfono, audio del escritorio, etc.). */
bool vfx_control_is_audio_source(obs_source_t *source);

/*
 * Nombre de la fuente que tiene este efecto, o NULL si no esta en ninguna.
 * "count" recibe en cuantas fuentes esta (puede ser NULL).
 * El texto devuelto se libera con bfree().
 */
char *vfx_control_get_source_name(const struct vfx_effect_info *info, size_t *count);

/*
 * Deja el efecto SOLO en la fuente indicada, conservando su configuracion y si
 * estaba prendido. Con NULL o "" lo quita de todas las fuentes.
 * Devuelve false si la fuente no existe, no es de audio o no se pudo crear el filtro.
 */
bool vfx_control_attach(const struct vfx_effect_info *info, const char *source_name);

/*
 * Lee los valores de los deslizadores del efecto (uno por parametro, en el
 * orden de info->params). Si el efecto no esta en ninguna fuente, devuelve
 * false y llena "values" con los valores por defecto.
 */
bool vfx_control_get_params(const struct vfx_effect_info *info, float *values);

/* Cambia un deslizador (indice en info->params) en todas las fuentes que tienen el efecto. */
void vfx_control_set_param(const struct vfx_effect_info *info, size_t index, double value);

/* Atajo de OBS de este efecto (OBS_INVALID_HOTKEY_ID si no existe). */
obs_hotkey_id vfx_control_get_hotkey(const struct vfx_effect_info *info);

/*
 * Teclas asignadas al atajo del efecto como texto (por ejemplo "Ctrl + Shift + R"),
 * o NULL si no tiene ninguna. El texto devuelto se libera con bfree().
 */
char *vfx_control_get_hotkey_text(const struct vfx_effect_info *info);

/* Asigna una combinacion de teclas al atajo (reemplaza la anterior). */
bool vfx_control_set_hotkey(const struct vfx_effect_info *info, obs_key_combination_t combo);

/* Quita todas las teclas del atajo. */
void vfx_control_clear_hotkey(const struct vfx_effect_info *info);

#ifdef __cplusplus
}
#endif
