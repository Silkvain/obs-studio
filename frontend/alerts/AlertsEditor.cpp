/******************************************************************************
    Siegfried: alert editor. See AlertsEditor.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AlertsEditor.hpp"

#include <multichat/ChatHub.hpp>
#include <utility/display-helpers.hpp>
#include <widgets/OBSBasic.hpp>

#include <OBSApp.hpp>

#include <graphics/vec4.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <mutex>

#include "moc_AlertsEditor.cpp"

QPointer<AlertsEditor> AlertsEditor::open;

namespace {

std::mutex g_boxMutex;
QRectF g_boxSource; /* name box in source pixels, for the render thread */

QColor FromObs(uint32_t abgr)
{
	return QColor(int(abgr & 0xFF), int((abgr >> 8) & 0xFF), int((abgr >> 16) & 0xFF), int((abgr >> 24) & 0xFF));
}

uint32_t ToObs(const QColor &c)
{
	return uint32_t(c.red()) | (uint32_t(c.green()) << 8) | (uint32_t(c.blue()) << 16) |
	       (uint32_t(c.alpha()) << 24);
}

void Line(float x1, float y1, float x2, float y2, float t, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	vec4 c;
	vec4_from_rgba(&c, color);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
	gs_matrix_push();
	float x = std::min(x1, x2), y = std::min(y1, y2);
	float w = std::max(std::fabs(x2 - x1), t), h = std::max(std::fabs(y2 - y1), t);
	gs_matrix_translate3f(x - (w == t ? t / 2 : 0), y - (h == t ? t / 2 : 0), 0);
	gs_matrix_scale3f(w, h, 1);
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, 1, 1);
	}
	gs_matrix_pop();
}

QString Seconds(int ms)
{
	return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1);
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Preview                                                                  */

AlertPreview::AlertPreview(QWidget *parent, AlertsEditor *ed) : OBSQTDisplay(parent), editor(ed)
{
	setMinimumSize(320, 220);
	setMouseTracking(true);
	connect(this, &OBSQTDisplay::DisplayCreated, this,
		[this] { obs_display_add_draw_callback(GetDisplay(), Render, this); });
}

AlertPreview::~AlertPreview()
{
	if (GetDisplay()) {
		obs_display_remove_draw_callback(GetDisplay(), Render, this);
	}
}

void AlertPreview::Render(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<AlertPreview *>(data);
	obs_source_t *src = self->editor->Source();
	uint32_t sw = obs_source_get_width(src), sh = obs_source_get_height(src);
	if (!sw || !sh) {
		return;
	}
	int x, y;
	float scale;
	GetScaleAndCenterPos(int(sw), int(sh), int(cx), int(cy), x, y, scale);
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(x, y, int(sw * scale), int(sh * scale));
	gs_ortho(0.0f, float(sw), 0.0f, float(sh), -100.0f, 100.0f);

	/* Checker-ish backdrop so transparent parts are visible */
	Line(0, 0, float(sw), float(sh), 1.0f, 0xFF2A2A2A);
	Alerts::SetEditorRendering(true);
	obs_source_video_render(src);
	Alerts::SetEditorRendering(false);

	QRectF b;
	{
		std::lock_guard l(g_boxMutex);
		b = g_boxSource;
	}
	float t = 2.0f / scale;
	uint32_t c = 0xFF4DD0FF; /* amber */
	Line(float(b.left()), float(b.top()), float(b.right()), float(b.top()), t, c);
	Line(float(b.left()), float(b.bottom()), float(b.right()), float(b.bottom()), t, c);
	Line(float(b.left()), float(b.top()), float(b.left()), float(b.bottom()), t, c);
	Line(float(b.right()), float(b.top()), float(b.right()), float(b.bottom()), t, c);
	float hs = 10.0f / scale;
	Line(float(b.right()) - hs, float(b.bottom()) - hs, float(b.right()), float(b.bottom()), 1.0f, c);

	gs_viewport_pop();
	gs_projection_pop();
}

QRectF AlertPreview::BoxOnScreen() const
{
	obs_source_t *src = editor->Source();
	uint32_t sw = obs_source_get_width(src), sh = obs_source_get_height(src);
	if (!sw || !sh) {
		return {};
	}
	QSize px = GetPixelSize(const_cast<AlertPreview *>(this));
	int x, y;
	float scale;
	GetScaleAndCenterPos(int(sw), int(sh), px.width(), px.height(), x, y, scale);
	qreal dpr = devicePixelRatioF();
	QRectF b;
	{
		std::lock_guard l(g_boxMutex);
		b = g_boxSource;
	}
	return QRectF((x + b.x() * scale) / dpr, (y + b.y() * scale) / dpr, b.width() * scale / dpr,
		      b.height() * scale / dpr);
}

void AlertPreview::mousePressEvent(QMouseEvent *e)
{
	QRectF box = BoxOnScreen();
	QPointF p = e->position();
	QRectF corner(box.right() - 14, box.bottom() - 14, 18, 18);
	if (corner.contains(p)) {
		drag = Drag::Resize;
	} else if (box.contains(p)) {
		drag = Drag::Move;
	} else {
		drag = Drag::None;
		return;
	}
	dragStart = p;
	boxStart = editor->Style().textBox;
}

void AlertPreview::mouseMoveEvent(QMouseEvent *e)
{
	QRectF box = BoxOnScreen();
	QPointF p = e->position();
	QRectF corner(box.right() - 14, box.bottom() - 14, 18, 18);
	setCursor(corner.contains(p) ? Qt::SizeFDiagCursor : box.contains(p) ? Qt::SizeAllCursor : Qt::ArrowCursor);
	if (drag == Drag::None) {
		return;
	}
	/* Pixels on screen -> fraction of the animation's area */
	obs_source_t *src = editor->Source();
	uint32_t sw = obs_source_get_width(src), sh = obs_source_get_height(src);
	QSize px = GetPixelSize(this);
	int x, y;
	float scale;
	GetScaleAndCenterPos(int(sw), int(sh), px.width(), px.height(), x, y, scale);
	QRectF area = editor->MediaArea(int(sw), int(sh));
	qreal dpr = devicePixelRatioF();
	double fx = (p.x() - dragStart.x()) * dpr / scale / area.width();
	double fy = (p.y() - dragStart.y()) * dpr / scale / area.height();
	QRectF nb = boxStart;
	if (drag == Drag::Move) {
		nb.moveTo(std::clamp(boxStart.x() + fx, -0.5, 1.0), std::clamp(boxStart.y() + fy, -0.5, 1.0));
	} else {
		nb.setWidth(std::max(0.05, boxStart.width() + fx));
		nb.setHeight(std::max(0.03, boxStart.height() + fy));
	}
	emit BoxChanged(nb);
}

void AlertPreview::mouseReleaseEvent(QMouseEvent *)
{
	drag = Drag::None;
}

/* ------------------------------------------------------------------------ */
/* Editor                                                                   */

void AlertsEditor::Open(obs_source_t *alertsSource)
{
	if (open) {
		open->raise();
		open->activateWindow();
		return;
	}
	AlertsEditor *ed = new AlertsEditor(OBSBasic::Get(), alertsSource);
	ed->setAttribute(Qt::WA_DeleteOnClose);
	open = ed;
	ed->show();
}

AlertsEditor::AlertsEditor(QWidget *parent, obs_source_t *src) : QDialog(parent), source(src)
{
	weakSource = OBSGetWeakRef(src);
	setWindowTitle(QTStr("Alerts.EditorTitle"));
	resize(1040, 720);

	types = new QListWidget(this);
	types->setMaximumWidth(220);
	for (int i = 0; i < kStreamEventTypeCount; i++) {
		QListWidgetItem *it = new QListWidgetItem(StreamEventName(StreamEventType(i)), types);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
	}

	preview = new AlertPreview(this, this);
	preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	time = new QSlider(Qt::Horizontal, this);
	time->setRange(0, 10000);
	timeLabel = new QLabel(this);
	timeLabel->setMinimumWidth(50);
	QPushButton *appear = new QPushButton(QTStr("Alerts.NameAppears"), this);
	QPushButton *hide = new QPushButton(QTStr("Alerts.NameHides"), this);
	QPushButton *test = new QPushButton(QTStr("Alerts.TestLive"), this);
	timingLabel = new QLabel(this);
	timingLabel->setStyleSheet("color: palette(placeholder-text);");

	QHBoxLayout *timeRow = new QHBoxLayout();
	timeRow->addWidget(time, 1);
	timeRow->addWidget(timeLabel);
	QHBoxLayout *timeButtons = new QHBoxLayout();
	timeButtons->addWidget(appear);
	timeButtons->addWidget(hide);
	timeButtons->addWidget(timingLabel, 1);
	timeButtons->addWidget(test);

	auto browseRow = [this](QLineEdit *&edit, const QString &filter) {
		QHBoxLayout *row = new QHBoxLayout();
		edit = new QLineEdit(this);
		QPushButton *b = new QPushButton(QTStr("Browse"), this);
		QPushButton *clear = new QPushButton(QStringLiteral("×"), this);
		clear->setMaximumWidth(28);
		row->addWidget(edit, 1);
		row->addWidget(b);
		row->addWidget(clear);
		QLineEdit *e = edit;
		connect(b, &QPushButton::clicked, this, [this, e, filter] {
			QString start = e->text().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
							    : QFileInfo(e->text()).absolutePath();
			QString f = QFileDialog::getOpenFileName(this, QString(), start, filter);
			if (!f.isEmpty()) {
				e->setText(f);
			}
		});
		connect(clear, &QPushButton::clicked, e, &QLineEdit::clear);
		return row;
	};

	QFormLayout *form = new QFormLayout();
	form->addRow(QTStr("Alerts.Animation"),
		     browseRow(media, QTStr("Alerts.MediaFilter") +
					      QStringLiteral(" (*.webm *.mp4 *.mov *.mkv *.gif *.png *.jpg *.jpeg *.webp)")));
	form->addRow(QTStr("Alerts.Sound"),
		     browseRow(sound, QTStr("Alerts.SoundFilter") + QStringLiteral(" (*.mp3 *.ogg *.wav *.flac *.m4a *.opus)")));
	volume = new QSlider(Qt::Horizontal, this);
	volume->setRange(0, 100);
	form->addRow(QTStr("Alerts.Volume"), volume);
	textTemplate = new QLineEdit(this);
	textTemplate->setToolTip(QTStr("Alerts.TemplateHint"));
	form->addRow(QTStr("Alerts.Text"), textTemplate);
	QLabel *hint = new QLabel(QTStr("Alerts.TemplateHint"), this);
	hint->setStyleSheet("color: palette(placeholder-text);");
	hint->setWordWrap(true);
	form->addRow(QString(), hint);
	showMessage = new QCheckBox(QTStr("Alerts.ShowMessage"), this);
	form->addRow(QString(), showMessage);
	QHBoxLayout *look = new QHBoxLayout();
	font = new QPushButton(this);
	color = new QPushButton(QTStr("Alerts.Colour"), this);
	outline = new QCheckBox(QTStr("Alerts.Outline"), this);
	look->addWidget(font, 1);
	look->addWidget(color);
	look->addWidget(outline);
	form->addRow(QTStr("Alerts.Font"), look);
	duration = new QSpinBox(this);
	duration->setRange(0, 60000);
	duration->setSingleStep(500);
	duration->setSuffix(QStringLiteral(" ms"));
	duration->setSpecialValueText(QTStr("Alerts.DurationAuto"));
	form->addRow(QTStr("Alerts.Duration"), duration);
	minAmount = new QDoubleSpinBox(this);
	minAmount->setRange(0, 1000000);
	minAmount->setDecimals(2);
	minAmount->setSpecialValueText(QTStr("Alerts.AnyAmount"));
	minAmountLabel = new QLabel(QTStr("Alerts.MinAmount"), this);
	form->addRow(minAmountLabel, minAmount);
	blocked = new QLineEdit(Alerts::BlockedWords().join(QStringLiteral(", ")), this);
	blocked->setPlaceholderText(QTStr("Alerts.BlockedHint"));
	form->addRow(QTStr("Alerts.Blocked"), blocked);

	QWidget *right = new QWidget(this);
	QVBoxLayout *rl = new QVBoxLayout(right);
	rl->setContentsMargins(0, 0, 0, 0);
	rl->addWidget(preview, 1);
	rl->addLayout(timeRow);
	rl->addLayout(timeButtons);
	rl->addLayout(form);

	QHBoxLayout *main = new QHBoxLayout();
	main->addWidget(types);
	main->addWidget(right, 1);
	QPushButton *close = new QPushButton(QTStr("Close"), this);
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addLayout(main, 1);
	layout->addWidget(close, 0, Qt::AlignRight);
	connect(close, &QPushButton::clicked, this, &QDialog::close);

	/* Wiring */
	connect(types, &QListWidget::currentRowChanged, this, &AlertsEditor::SelectType);
	connect(types, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
		if (loading) {
			return;
		}
		int row = types->row(it);
		obs_source_t *src = source;
		OBSDataAutoRelease settings = obs_source_get_settings(src);
		OBSDataAutoRelease alerts = obs_data_get_obj(settings, "alerts");
		if (!alerts) {
			alerts = obs_data_create();
		}
		OBSDataAutoRelease d = obs_data_get_obj(alerts, StreamEventKey(StreamEventType(row)));
		AlertStyle st = AlertStyle::FromData(d, StreamEventType(row));
		st.enabled = it->checkState() == Qt::Checked;
		OBSDataAutoRelease nd = obs_data_create();
		st.ToData(nd);
		obs_data_set_obj(alerts, StreamEventKey(StreamEventType(row)), nd);
		obs_data_set_obj(settings, "alerts", alerts);
		obs_source_update(src, settings);
		if (StreamEventType(row) == type) {
			style.enabled = st.enabled;
		}
	});
	connect(time, &QSlider::valueChanged, this, [this] { UpdatePreview(); });
	connect(appear, &QPushButton::clicked, this, [this] {
		style.showAtMs = time->value();
		if (style.hideAtMs > 0 && style.hideAtMs <= style.showAtMs) {
			style.hideAtMs = 0;
		}
		SaveStyle();
	});
	connect(hide, &QPushButton::clicked, this, [this] {
		style.hideAtMs = std::max(time->value(), style.showAtMs + 300);
		SaveStyle();
	});
	connect(test, &QPushButton::clicked, this, [this] {
		Alerts::Preview(source, nullptr, 0);
		if (ChatHub *hub = OBSBasic::Get()->GetChatHub()) {
			hub->Test(type);
		}
	});
	connect(preview, &AlertPreview::BoxChanged, this, [this](const QRectF &b) {
		style.textBox = b;
		SaveStyle();
	});
	auto changed = [this] {
		if (!loading) {
			SaveStyle();
		}
	};
	connect(media, &QLineEdit::textChanged, this, [this, changed] {
		changed();
		UpdateTimeline();
	});
	connect(sound, &QLineEdit::textChanged, this, changed);
	connect(volume, &QSlider::valueChanged, this, changed);
	connect(textTemplate, &QLineEdit::textChanged, this, changed);
	connect(showMessage, &QCheckBox::toggled, this, changed);
	connect(outline, &QCheckBox::toggled, this, changed);
	connect(duration, &QSpinBox::valueChanged, this, [this, changed] {
		changed();
		UpdateTimeline();
	});
	connect(minAmount, &QDoubleSpinBox::valueChanged, this, changed);
	connect(font, &QPushButton::clicked, this, [this] {
		bool ok = false;
		QFont start(style.fontFace);
		start.setPixelSize(style.fontSize);
		start.setBold(style.bold);
		QFont f = QFontDialog::getFont(&ok, start, this);
		if (ok) {
			style.fontFace = f.family();
			style.fontSize = f.pixelSize() > 0 ? f.pixelSize() : int(f.pointSizeF() * 1.33);
			style.bold = f.bold();
			SaveStyle();
			LoadStyle();
		}
	});
	connect(color, &QPushButton::clicked, this, [this] {
		QColor c = QColorDialog::getColor(FromObs(style.color), this);
		if (c.isValid()) {
			style.color = ToObs(c) | 0xFF000000u;
			SaveStyle();
			LoadStyle();
		}
	});
	connect(blocked, &QLineEdit::editingFinished, this, [this] {
		QStringList words;
		for (const QString &w : blocked->text().split(',', Qt::SkipEmptyParts)) {
			words << w.trimmed();
		}
		Alerts::SetBlockedWords(words);
	});

	/* The animation's length becomes known once it has been opened */
	durationPoll.setInterval(300);
	connect(&durationPoll, &QTimer::timeout, this, [this] { UpdateTimeline(); });
	durationPoll.start();

	types->setCurrentRow(0);
}

AlertsEditor::~AlertsEditor()
{
	Alerts::Preview(source, nullptr, 0);
	std::lock_guard l(g_boxMutex);
	g_boxSource = QRectF();
}

QRectF AlertsEditor::MediaArea(int w, int h) const
{
	QSize ms = Alerts::MediaSize(style.media);
	if (ms.isEmpty()) {
		return QRectF(0, 0, w, h);
	}
	double s = std::min(double(w) / ms.width(), double(h) / ms.height());
	double mw = ms.width() * s, mh = ms.height() * s;
	return QRectF((w - mw) / 2.0, (h - mh) / 2.0, mw, mh);
}

StreamEvent AlertsEditor::SampleEvent() const
{
	StreamEvent e;
	e.type = type;
	e.test = true;
	e.user.displayName = QStringLiteral("TestViewer");
	e.user.login = QStringLiteral("testviewer");
	e.amount = (type == StreamEventType::Bits) ? 500 : 5;
	e.currency = (type == StreamEventType::Bits) ? QString() : QStringLiteral("USD");
	e.count = 5;
	e.months = 12;
	return e;
}

void AlertsEditor::SelectType(int row)
{
	if (row < 0) {
		return;
	}
	type = StreamEventType(row);
	LoadStyle();
	UpdateTimeline();
	/* Start where the name is fully on screen */
	int len = time->maximum();
	int hideAt = style.hideAtMs > 0 ? style.hideAtMs : len - 300;
	time->setValue(std::clamp((style.showAtMs + hideAt) / 2, 0, len));
	UpdatePreview();
}

void AlertsEditor::LoadStyle()
{
	loading = true;
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataAutoRelease alerts = obs_data_get_obj(settings, "alerts");
	for (int i = 0; i < types->count(); i++) {
		OBSDataAutoRelease d = alerts ? obs_data_get_obj(alerts, StreamEventKey(StreamEventType(i))) : nullptr;
		AlertStyle st = AlertStyle::FromData(d, StreamEventType(i));
		types->item(i)->setCheckState(st.enabled ? Qt::Checked : Qt::Unchecked);
		types->item(i)->setToolTip(st.media.isEmpty() ? QTStr("Alerts.NoAnimation") : st.media);
	}
	OBSDataAutoRelease d = alerts ? obs_data_get_obj(alerts, StreamEventKey(type)) : nullptr;
	style = AlertStyle::FromData(d, type);

	media->setText(style.media);
	sound->setText(style.sound);
	volume->setValue(style.volume);
	textTemplate->setText(style.textTemplate);
	showMessage->setChecked(style.showMessage);
	outline->setChecked((style.outline >> 24) != 0);
	font->setText(QStringLiteral("%1, %2 px%3").arg(style.fontFace).arg(style.fontSize).arg(style.bold ? ", bold" : ""));
	color->setStyleSheet(QStringLiteral("QPushButton { color: %1; font-weight: bold; }").arg(FromObs(style.color).name()));
	duration->setValue(style.durationMs);
	minAmount->setValue(style.minAmount);
	bool money = type == StreamEventType::Bits || type == StreamEventType::Donation ||
		     type == StreamEventType::SuperChat || type == StreamEventType::Rant;
	minAmount->setVisible(money);
	minAmountLabel->setVisible(money);
	loading = false;
}

void AlertsEditor::SaveStyle()
{
	if (loading) {
		return;
	}
	style.media = media->text().trimmed();
	style.sound = sound->text().trimmed();
	style.volume = volume->value();
	style.textTemplate = textTemplate->text();
	style.showMessage = showMessage->isChecked();
	style.outline = outline->isChecked() ? 0xFF000000u : 0x00000000u;
	style.durationMs = duration->value();
	style.minAmount = minAmount->value();

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataAutoRelease alerts = obs_data_get_obj(settings, "alerts");
	if (!alerts) {
		alerts = obs_data_create();
	}
	OBSDataAutoRelease d = obs_data_create();
	style.ToData(d);
	obs_data_set_obj(alerts, StreamEventKey(type), d);
	obs_data_set_obj(settings, "alerts", alerts);
	obs_source_update(source, settings);
	UpdatePreview();
}

void AlertsEditor::UpdateTimeline()
{
	int len = style.durationMs > 0 ? style.durationMs : Alerts::MediaDurationMs(style.media);
	if (len <= 0) {
		len = 8000;
	}
	if (time->maximum() != len) {
		time->setMaximum(len);
	}
	int hideAt = style.hideAtMs > 0 ? style.hideAtMs : len - 300;
	timingLabel->setText(QTStr("Alerts.Timing").arg(Seconds(style.showAtMs), Seconds(hideAt), Seconds(len)));
}

void AlertsEditor::UpdatePreview()
{
	timeLabel->setText(Seconds(time->value()));
	uint32_t sw = obs_source_get_width(source), sh = obs_source_get_height(source);
	QRectF area = MediaArea(int(sw), int(sh));
	{
		std::lock_guard l(g_boxMutex);
		g_boxSource = QRectF(area.x() + style.textBox.x() * area.width(), area.y() + style.textBox.y() * area.height(),
				     style.textBox.width() * area.width(), style.textBox.height() * area.height());
	}
	StreamEvent e = SampleEvent();
	Alerts::Preview(source, &e, time->value());
}
