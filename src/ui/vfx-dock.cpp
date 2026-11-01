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
 * El panel solo MUESTRA el estado y llama a core/effect-control.h, que es
 * quien realmente toca las fuentes, filtros y atajos de OBS. No guarda
 * archivos: todo lo que muestra sale de OBS, asi nunca queda "desincronizado".
 *
 * Actualizacion instantanea: los filtros y atajos suben un contador cada vez
 * que cambian (core/effect-filter.h). El panel mira ese numero cada 50 ms,
 * algo que no cuesta practicamente nada, y solo si cambio vuelve a leer OBS.
 *
 * Todo corre en el hilo principal de OBS (el de la interfaz).
 */

#include "ui/vfx-dock.h"

#include "core/effect-control.h"
#include "core/effect-filter.h"
#include "effects/effects-list.h"
#include "plugin-info.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <math.h>

namespace {

constexpr int kCheckMs = 50;         /* cada cuanto se mira si algo cambio (casi gratis) */
constexpr int kFullRefreshMs = 1000; /* repaso completo por si cambia algo sin aviso (renombrar) */
constexpr const char *kDockId = "obs_voice_fx_dock";

/* Texto traducido (data/locale/<idioma>.ini) como QString. */
QString loc(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Nombres de todas las fuentes de audio, ordenados alfabeticamente. */
QStringList audio_source_names()
{
	QStringList names;

	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			if (vfx_control_is_audio_source(source)) {
				const char *name = obs_source_get_name(source);
				if (name && name[0])
					static_cast<QStringList *>(param)->append(QString::fromUtf8(name));
			}
			return true;
		},
		&names);

	names.sort(Qt::CaseInsensitive);
	return names;
}

/* Modificadores de Qt -> modificadores de OBS (igual que la ventana de atajos de OBS). */
uint32_t obs_modifiers_from_qt(Qt::KeyboardModifiers mods)
{
	uint32_t result = INTERACT_NONE;
	if (mods.testFlag(Qt::ShiftModifier))
		result |= INTERACT_SHIFT_KEY;
	if (mods.testFlag(Qt::AltModifier))
		result |= INTERACT_ALT_KEY;
	if (mods.testFlag(Qt::ControlModifier))
		result |= INTERACT_CONTROL_KEY;
	if (mods.testFlag(Qt::MetaModifier))
		result |= INTERACT_COMMAND_KEY; /* tecla Windows */
	return result;
}

class EffectBox;
class VoiceFxDock;

VoiceFxDock *g_dock = nullptr; /* el panel actual (OBS lo borra al cerrarse) */

/* Lista desplegable que actualiza las fuentes justo antes de abrirse. */
class SourceCombo : public QComboBox {
public:
	explicit SourceCombo(EffectBox *owner) : owner_(owner) {}
	void showPopup() override;

private:
	EffectBox *owner_;
};

/*
 * Boton "Cambiar atajo": al hacer clic espera la combinacion de teclas.
 * Esc cancela. Las teclas solas Shift/Ctrl/Alt/Win esperan a la tecla principal.
 */
class HotkeyButton : public QPushButton {
public:
	explicit HotkeyButton(EffectBox *owner) : owner_(owner)
	{
		setText(loc("Dock.HotkeyChange"));
		connect(this, &QPushButton::clicked, this, [this]() { startCapture(); });
	}

	void startCapture()
	{
		if (capturing_)
			return;
		capturing_ = true;
		setText(loc("Dock.HotkeyCapture"));
		setFocus(Qt::OtherFocusReason);
		grabKeyboard();
	}

	void stopCapture()
	{
		if (!capturing_)
			return;
		capturing_ = false;
		releaseKeyboard();
		setText(loc("Dock.HotkeyChange"));
	}

protected:
	bool event(QEvent *e) override
	{
		if (capturing_) {
			switch (e->type()) {
			case QEvent::KeyPress:
				handleKey(static_cast<QKeyEvent *>(e));
				return true;
			case QEvent::KeyRelease:
			case QEvent::ShortcutOverride: /* que ningun atajo de la ventana se lleve la tecla */
				e->accept();
				return true;
			default:
				break;
			}
		}
		return QPushButton::event(e);
	}

	void focusOutEvent(QFocusEvent *e) override
	{
		stopCapture();
		QPushButton::focusOutEvent(e);
	}

private:
	void handleKey(QKeyEvent *e);

	EffectBox *owner_;
	bool capturing_ = false;
};

/* Un deslizador: nombre y valor arriba, barra abajo a todo lo ancho. */
struct ParamRow {
	QSlider *slider = nullptr;
	QLabel *value = nullptr;
};

/* Bloque de un efecto dentro del panel. */
class EffectBox : public QGroupBox {
public:
	explicit EffectBox(const vfx_effect_info *info) : info_(info)
	{
		setTitle(loc(info->name_key));

		auto *layout = new QVBoxLayout(this);

		/* Fuente de audio */
		auto *sourceRow = new QHBoxLayout();
		auto *sourceLabel = new QLabel(loc("Dock.Source"), this);
		combo_ = new SourceCombo(this);
		combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo_->setMinimumContentsLength(6);
		combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		sourceRow->addWidget(sourceLabel);
		sourceRow->addWidget(combo_, 1);
		layout->addLayout(sourceRow);

		/* Activar / desactivar */
		toggle_ = new QPushButton(this);
		toggle_->setCheckable(true);
		layout->addWidget(toggle_);

		status_ = new QLabel(this);
		status_->setWordWrap(true);
		layout->addWidget(status_);

		/* Deslizadores (igual que en la ventana de filtros) */
		for (size_t i = 0; i < info->num_params && i < VFX_MAX_PARAMS; i++)
			addParamRow(layout, i);

		/* Atajo de teclado */
		auto *line = new QFrame(this);
		line->setFrameShape(QFrame::HLine);
		line->setFrameShadow(QFrame::Sunken);
		layout->addWidget(line);

		auto *hotkeyRow = new QHBoxLayout();
		auto *hotkeyLabel = new QLabel(loc("Dock.Hotkey"), this);
		hotkeyText_ = new QLabel(this);
		hotkeyText_->setWordWrap(true);
		hotkeyRow->addWidget(hotkeyLabel);
		hotkeyRow->addWidget(hotkeyText_, 1);
		layout->addLayout(hotkeyRow);

		auto *hotkeyButtons = new QHBoxLayout();
		hotkeyButton_ = new HotkeyButton(this);
		clearButton_ = new QPushButton(loc("Dock.HotkeyClear"), this);
		hotkeyButtons->addWidget(hotkeyButton_, 1);
		hotkeyButtons->addWidget(clearButton_);
		layout->addLayout(hotkeyButtons);

		/* Elegir una fuente: el efecto queda solo en esa fuente. */
		connect(combo_, &QComboBox::activated, this, [this](int index) {
			if (updating_)
				return;
			const QByteArray name = combo_->itemData(index).toString().toUtf8();
			vfx_control_attach(info_, name.isEmpty() ? nullptr : name.constData());
			refreshState();
		});

		/* Boton activar/desactivar. */
		connect(toggle_, &QPushButton::clicked, this, [this](bool checked) {
			vfx_control_set_enabled(info_, checked);
			refreshState();
		});

		/* Quitar las teclas del atajo. */
		connect(clearButton_, &QPushButton::clicked, this, [this]() {
			vfx_control_clear_hotkey(info_);
			refreshHotkey();
		});

		refreshSources();
		refreshState();
	}

	/* Lo llama el boton de atajo cuando el usuario presiono una combinacion. */
	void setHotkey(obs_key_combination_t combo)
	{
		vfx_control_set_hotkey(info_, combo);
		refreshHotkey();
	}

	/* Vuelve a llenar la lista de fuentes de audio (se agregan, borran o renombran). */
	void refreshSources()
	{
		updating_ = true;

		const QString current = attachedSource();
		combo_->clear();
		combo_->addItem(loc("Dock.NoSource"), QString());

		for (const QString &name : audio_source_names())
			combo_->addItem(name, name);

		int index = combo_->findData(current);
		combo_->setCurrentIndex(index >= 0 ? index : 0);

		updating_ = false;
	}

	/* Actualiza todo segun lo que hay realmente en OBS. */
	void refreshState()
	{
		size_t count = 0;
		char *name = vfx_control_get_source_name(info_, &count);
		const QString current = name ? QString::fromUtf8(name) : QString();
		bfree(name);

		const bool attached = count > 0;
		const bool enabled = attached && vfx_control_is_enabled(info_);

		/* No se toca la lista mientras el usuario la tiene abierta. */
		if (!combo_->view()->isVisible()) {
			int index = combo_->findData(current);
			if (index < 0 && !current.isEmpty()) {
				refreshSources();
			} else if (index >= 0 && index != combo_->currentIndex()) {
				updating_ = true;
				combo_->setCurrentIndex(index);
				updating_ = false;
			} else if (current.isEmpty() && combo_->currentIndex() != 0) {
				updating_ = true;
				combo_->setCurrentIndex(0);
				updating_ = false;
			}
		}

		toggle_->setEnabled(attached);
		toggle_->setChecked(enabled);
		toggle_->setText(enabled ? loc("Dock.Enabled") : loc("Dock.Disabled"));

		if (!attached)
			status_->setText(loc("Dock.NotAttached"));
		else if (count > 1)
			status_->setText(loc("Dock.MultipleSources"));
		else
			status_->clear();
		status_->setVisible(!status_->text().isEmpty());

		refreshParams(attached);
		refreshHotkey();
	}

private:
	void addParamRow(QVBoxLayout *layout, size_t index)
	{
		const vfx_param_info &param = info_->params[index];
		ParamRow &p = params_[index];

		auto *header = new QHBoxLayout();
		auto *name = new QLabel(loc(param.text_key), this);
		name->setWordWrap(true);
		p.value = new QLabel(this);
		p.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		header->addWidget(name, 1);
		header->addWidget(p.value);
		layout->addLayout(header);

		p.slider = new QSlider(Qt::Horizontal, this);
		p.slider->setRange(0, stepsOf(param));
		layout->addWidget(p.slider);

		/* Mover el deslizador cambia el filtro igual que en la ventana de filtros. */
		connect(p.slider, &QSlider::valueChanged, this, [this, index](int position) {
			const double value = valueOf(info_->params[index], position);
			params_[index].value->setText(formatValue(info_->params[index], value));
			if (!updating_)
				vfx_control_set_param(info_, index, value);
		});
	}

	void refreshParams(bool attached)
	{
		float values[VFX_MAX_PARAMS] = {0};
		vfx_control_get_params(info_, values);

		updating_ = true;
		for (size_t i = 0; i < info_->num_params && i < VFX_MAX_PARAMS; i++) {
			const vfx_param_info &param = info_->params[i];
			ParamRow &p = params_[i];

			p.slider->setEnabled(attached);

			/* No se mueve el deslizador mientras el usuario lo esta arrastrando. */
			if (!p.slider->isSliderDown()) {
				const QSignalBlocker block(p.slider);
				p.slider->setValue(positionOf(param, values[i]));
			}
			p.value->setText(formatValue(param, valueOf(param, p.slider->value())));
		}
		updating_ = false;
	}

	void refreshHotkey()
	{
		char *keys = vfx_control_get_hotkey_text(info_);
		hotkeyText_->setText(keys ? QString::fromUtf8(keys) : loc("Dock.HotkeyNone"));
		clearButton_->setEnabled(keys != nullptr);
		hotkeyButton_->setEnabled(vfx_control_get_hotkey(info_) != OBS_INVALID_HOTKEY_ID);
		bfree(keys);
	}

	QString attachedSource() const
	{
		char *name = vfx_control_get_source_name(info_, nullptr);
		const QString result = name ? QString::fromUtf8(name) : QString();
		bfree(name);
		return result;
	}

	/* El deslizador trabaja con pasos enteros: 0 .. (max - min) / paso. */
	static int stepsOf(const vfx_param_info &p) { return (int)lround((p.max - p.min) / p.step); }

	static double valueOf(const vfx_param_info &p, int position) { return p.min + position * p.step; }

	static int positionOf(const vfx_param_info &p, double value)
	{
		const int position = (int)lround((value - p.min) / p.step);
		return position < 0 ? 0 : (position > stepsOf(p) ? stepsOf(p) : position);
	}

	static QString formatValue(const vfx_param_info &p, double value)
	{
		const int decimals = p.step < 1.0 ? 1 : 0;
		return QString::number(value, 'f', decimals) + QString::fromUtf8(p.suffix ? p.suffix : "");
	}

	const vfx_effect_info *info_;
	SourceCombo *combo_ = nullptr;
	QPushButton *toggle_ = nullptr;
	QLabel *status_ = nullptr;
	ParamRow params_[VFX_MAX_PARAMS];
	QLabel *hotkeyText_ = nullptr;
	HotkeyButton *hotkeyButton_ = nullptr;
	QPushButton *clearButton_ = nullptr;
	bool updating_ = false; /* evita reaccionar a cambios hechos por el propio panel */
};

void SourceCombo::showPopup()
{
	owner_->refreshSources();
	QComboBox::showPopup();
}

void HotkeyButton::handleKey(QKeyEvent *e)
{
	if (e->isAutoRepeat())
		return;

	switch (e->key()) {
	case Qt::Key_Escape:
		stopCapture(); /* cancelar */
		return;
	case Qt::Key_Shift:
	case Qt::Key_Control:
	case Qt::Key_Alt:
	case Qt::Key_Meta:
		return; /* se espera la tecla principal */
	default:
		break;
	}

	obs_key_combination_t combo;
	combo.key = obs_key_from_virtual_key((int)e->nativeVirtualKey());
	combo.modifiers = obs_modifiers_from_qt(e->modifiers());

	if (combo.key == OBS_KEY_NONE)
		return; /* tecla que OBS no reconoce: se sigue esperando */

	stopCapture();
	owner_->setHotkey(combo);
}

/* El panel completo: un bloque por efecto, con barra de desplazamiento vertical. */
class VoiceFxDock : public QWidget {
public:
	VoiceFxDock()
	{
		auto *content = new QWidget();
		auto *list = new QVBoxLayout(content);

		size_t count = 0;
		const vfx_effect_info *const *effects = vfx_get_effects(&count);
		for (size_t i = 0; i < count; i++) {
			if (!vfx_effect_info_is_valid(effects[i]))
				continue;
			auto *box = new EffectBox(effects[i]);
			boxes_.append(box);
			list->addWidget(box);
		}
		list->addStretch(1);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); /* todo se adapta al ancho */
		scroll->setWidget(content);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->addWidget(scroll);

		lastVersion_ = vfx_filter_state_version();

		/* Cambios de filtros y atajos (atajo, ojito, deslizadores): se ven al instante. */
		checkTimer_ = new QTimer(this);
		connect(checkTimer_, &QTimer::timeout, this, [this]() {
			const long version = vfx_filter_state_version();
			if (version != lastVersion_ && isVisible()) {
				lastVersion_ = version;
				refreshAll();
			}
		});
		checkTimer_->start(kCheckMs);

		/* Repaso completo cada tanto (por ejemplo, si se renombra una fuente). */
		fullTimer_ = new QTimer(this);
		connect(fullTimer_, &QTimer::timeout, this, [this]() {
			if (isVisible())
				refreshAll();
		});
		fullTimer_->start(kFullRefreshMs);
	}

	~VoiceFxDock() override
	{
		if (g_dock == this)
			g_dock = nullptr;
	}

	/* OBS se esta cerrando: no se toca mas nada de OBS. */
	void stop()
	{
		checkTimer_->stop();
		fullTimer_->stop();
	}

private:
	void refreshAll()
	{
		for (EffectBox *box : boxes_)
			box->refreshState();
	}

	QList<EffectBox *> boxes_;
	QTimer *checkTimer_ = nullptr;
	QTimer *fullTimer_ = nullptr;
	long lastVersion_ = 0;
};

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT && g_dock)
		g_dock->stop();
}

} // namespace

extern "C" bool vfx_dock_create(void)
{
	if (!obs_frontend_get_main_window()) {
		obs_log(LOG_WARNING, "no se encontro la ventana de OBS: el panel no se crea");
		return false;
	}

	auto *dock = new VoiceFxDock();
	dock->setObjectName(kDockId);

	/* OBS se queda con el panel: lo acomoda, lo guarda y lo borra al cerrarse. */
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Dock.Title"), dock)) {
		delete dock;
		obs_log(LOG_WARNING, "OBS no acepto el panel");
		return false;
	}

	g_dock = dock;
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "panel creado");
	return true;
}

extern "C" void vfx_dock_shutdown(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	g_dock = nullptr;
}
