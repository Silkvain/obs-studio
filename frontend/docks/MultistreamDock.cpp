/******************************************************************************
    Destinations dock for native multistreaming.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamDock.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs-frontend-api.h>
#include <qt-wrappers.hpp>
#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "moc_MultistreamDock.cpp"

/* ------------------------------------------------------------------------ */
/* Platform presets                                                         */

namespace {
struct Preset {
	const char *name;
	const char *server;     /* empty: account/region specific, user pastes it */
	const char *hintLookup; /* where to find the URL and key */
};

const Preset kPresets[] = {
	{"Twitch", "rtmp://live.twitch.tv/app", "Multistream.Hint.Twitch"},
	{"Kick", "", "Multistream.Hint.Kick"},
	{"YouTube", "rtmp://a.rtmp.youtube.com/live2", "Multistream.Hint.YouTube"},
	{"X", "", "Multistream.Hint.X"},
	{"Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/", "Multistream.Hint.Facebook"},
	{"Custom", "", "Multistream.Hint.Custom"},
};
constexpr int kCustomPreset = int(std::size(kPresets)) - 1;

int GuessPreset(const MultistreamDestination &d)
{
	for (int i = 0; i < kCustomPreset; i++) {
		if (d.name.compare(QString::fromUtf8(kPresets[i].name), Qt::CaseInsensitive) == 0) {
			return i;
		}
		if (*kPresets[i].server && d.server == QString::fromUtf8(kPresets[i].server)) {
			return i;
		}
	}
	if (d.server.contains("kick", Qt::CaseInsensitive) || d.server.contains("live-video.net")) {
		return 1;
	}
	return kCustomPreset;
}

const char *StateColor(MultistreamState s)
{
	switch (s) {
	case MultistreamState::Live:
		return "#2ecc71";
	case MultistreamState::Starting:
	case MultistreamState::Reconnecting:
		return "#f1c40f";
	case MultistreamState::Failed:
		return "#e74c3c";
	case MultistreamState::Offline:
	default:
		return "#7f8c8d";
	}
}

QString StateText(MultistreamState s)
{
	switch (s) {
	case MultistreamState::Live:
		return QTStr("Multistream.State.Live");
	case MultistreamState::Starting:
		return QTStr("Multistream.State.Starting");
	case MultistreamState::Reconnecting:
		return QTStr("Multistream.State.Reconnecting");
	case MultistreamState::Failed:
		return QTStr("Multistream.State.Failed");
	case MultistreamState::Offline:
	default:
		return QTStr("Multistream.State.Offline");
	}
}

QString PrimaryServiceName()
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!service) {
		return QTStr("Multistream.Primary");
	}
	OBSDataAutoRelease settings = obs_service_get_settings(service);
	QString name = QString::fromUtf8(obs_data_get_string(settings, "service"));
	if (name.isEmpty()) {
		name = QTStr("Multistream.CustomServer");
	}
	return name;
}
} // namespace

/* ------------------------------------------------------------------------ */
/* Add / edit dialog                                                        */

MultistreamDestinationDialog::MultistreamDestinationDialog(QWidget *parent, const MultistreamDestination &dest_,
							   bool isNew)
	: QDialog(parent),
	  dest(dest_)
{
	setWindowTitle(isNew ? QTStr("Multistream.AddTitle") : QTStr("Multistream.EditTitle"));
	setMinimumWidth(460);

	preset = new QComboBox(this);
	for (const Preset &p : kPresets) {
		preset->addItem(QString::fromUtf8(p.name));
	}

	name = new QLineEdit(dest.name, this);
	server = new QLineEdit(dest.server, this);
	server->setPlaceholderText("rtmp://  or  rtmps://");
	key = new QLineEdit(dest.key, this);
	key->setEchoMode(QLineEdit::Password);

	QToolButton *showKey = new QToolButton(this);
	showKey->setText(QTStr("Show"));
	showKey->setCheckable(true);
	connect(showKey, &QToolButton::toggled, this, [this, showKey](bool on) {
		key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
		showKey->setText(on ? QTStr("Hide") : QTStr("Show"));
	});

	QHBoxLayout *keyRow = new QHBoxLayout();
	keyRow->setContentsMargins(0, 0, 0, 0);
	keyRow->addWidget(key, 1);
	keyRow->addWidget(showKey);

	hint = new QLabel(this);
	hint->setWordWrap(true);
	hint->setTextFormat(Qt::PlainText);
	hint->setStyleSheet("color: palette(placeholder-text);");

	QFormLayout *form = new QFormLayout();
	form->addRow(QTStr("Multistream.Platform"), preset);
	form->addRow(QTStr("Multistream.Name"), name);
	form->addRow(QTStr("Multistream.Server"), server);
	form->addRow(QTStr("Multistream.StreamKey"), keyRow);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	okButton = buttons->button(QDialogButtonBox::Ok);
	okButton->setText(isNew ? QTStr("Multistream.Add") : QTStr("Multistream.Save"));
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	/* Quality: share the main stream's encoding, or encode separately */
	quality = new QComboBox(this);
	quality->addItem(QTStr("Multistream.Quality.Same"));
	quality->addItem(QTStr("Multistream.Quality.Custom"));

	videoBitrate = new QSpinBox(this);
	videoBitrate->setRange(500, 60000);
	videoBitrate->setSingleStep(500);
	videoBitrate->setSuffix(QStringLiteral(" kbps"));
	videoBitrate->setValue(dest.videoBitrate);

	resolution = new QComboBox(this);
	resolution->addItem(QTStr("Multistream.Quality.SameSize"), QSize());
	for (QSize sz : {QSize(3840, 2160), QSize(2560, 1440), QSize(1920, 1080), QSize(1600, 900), QSize(1280, 720),
			 QSize(852, 480)}) {
		resolution->addItem(QStringLiteral("%1×%2").arg(sz.width()).arg(sz.height()), sz);
	}
	resolution->setCurrentIndex(std::max(0, resolution->findData(QSize(dest.width, dest.height))));

	audioBitrate = new QComboBox(this);
	for (int kb : {96, 128, 160, 192, 256, 320}) {
		audioBitrate->addItem(QStringLiteral("%1 kbps").arg(kb), kb);
	}
	audioBitrate->setCurrentIndex(std::max(0, audioBitrate->findData(dest.audioBitrate)));

	customBox = new QWidget(this);
	QFormLayout *customForm = new QFormLayout(customBox);
	customForm->setContentsMargins(0, 0, 0, 0);
	customForm->addRow(QTStr("Multistream.Quality.VideoBitrate"), videoBitrate);
	customForm->addRow(QTStr("Multistream.Quality.Resolution"), resolution);
	customForm->addRow(QTStr("Multistream.Quality.AudioBitrate"), audioBitrate);

	qualityHint = new QLabel(this);
	qualityHint->setWordWrap(true);
	qualityHint->setStyleSheet("color: palette(placeholder-text);");

	QFormLayout *qualityForm = new QFormLayout();
	qualityForm->addRow(QTStr("Multistream.Quality"), quality);

	quality->setCurrentIndex(dest.customQuality ? 1 : 0);
	customBox->setVisible(dest.customQuality);
	connect(quality, &QComboBox::currentIndexChanged, this, [this](int i) {
		customBox->setVisible(i == 1);
		UpdateQualityHint();
		adjustSize();
	});

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(hint);
	layout->addSpacing(8);
	layout->addLayout(qualityForm);
	layout->addWidget(customBox);
	layout->addWidget(qualityHint);
	layout->addStretch();
	layout->addWidget(buttons);

	int initial = isNew ? 0 : GuessPreset(dest);
	preset->setCurrentIndex(initial);
	PresetChanged(initial);
	if (!isNew) {
		/* Don't let the preset clobber saved values when editing */
		name->setText(dest.name);
		server->setText(dest.server);
	}

	connect(preset, &QComboBox::currentIndexChanged, this, &MultistreamDestinationDialog::PresetChanged);
	connect(preset, &QComboBox::currentIndexChanged, this, &MultistreamDestinationDialog::UpdateQualityHint);
	UpdateQualityHint();
	connect(name, &QLineEdit::textChanged, this, &MultistreamDestinationDialog::Validate);
	connect(server, &QLineEdit::textChanged, this, &MultistreamDestinationDialog::Validate);
	connect(key, &QLineEdit::textChanged, this, &MultistreamDestinationDialog::Validate);
	Validate();
}

void MultistreamDestinationDialog::PresetChanged(int index)
{
	if (index < 0 || index >= int(std::size(kPresets))) {
		return;
	}
	const Preset &p = kPresets[index];

	/* Only replace the name if the user hasn't typed a custom one */
	bool nameIsPreset = name->text().isEmpty();
	for (const Preset &other : kPresets) {
		if (name->text() == QString::fromUtf8(other.name)) {
			nameIsPreset = true;
		}
	}
	if (nameIsPreset && index != kCustomPreset) {
		name->setText(QString::fromUtf8(p.name));
	}

	if (*p.server) {
		server->setText(QString::fromUtf8(p.server));
	} else {
		bool serverIsPreset = server->text().isEmpty();
		for (const Preset &other : kPresets) {
			if (*other.server && server->text() == QString::fromUtf8(other.server)) {
				serverIsPreset = true;
			}
		}
		if (serverIsPreset) {
			server->clear();
		}
	}

	hint->setText(QTStr(p.hintLookup));
	Validate();
}

void MultistreamDestinationDialog::Validate()
{
	QString s = server->text().trimmed();
	bool serverOk = s.startsWith("rtmp://", Qt::CaseInsensitive) || s.startsWith("rtmps://", Qt::CaseInsensitive);
	bool ok = !name->text().trimmed().isEmpty() && serverOk && !key->text().trimmed().isEmpty();
	okButton->setEnabled(ok);
}

MultistreamDestination MultistreamDestinationDialog::Result() const
{
	MultistreamDestination d = dest;
	d.name = name->text().trimmed();
	d.server = server->text().trimmed();
	d.key = key->text().trimmed();
	d.customQuality = quality->currentIndex() == 1;
	d.videoBitrate = videoBitrate->value();
	d.audioBitrate = audioBitrate->currentData().toInt();
	QSize sz = resolution->currentData().toSize();
	d.width = sz.isValid() ? sz.width() : 0;
	d.height = sz.isValid() ? sz.height() : 0;
	return d;
}

void MultistreamDestinationDialog::UpdateQualityHint()
{
	if (quality->currentIndex() == 0) {
		qualityHint->setText(QTStr("Multistream.Quality.SameHint"));
		return;
	}
	QString text = QTStr("Multistream.Quality.CustomHint");
	QString platform = preset->currentText();
	if (platform == "YouTube") {
		text += QStringLiteral(" ") + QTStr("Multistream.Quality.YouTubeHint");
	} else if (platform == "Twitch") {
		text += QStringLiteral(" ") + QTStr("Multistream.Quality.TwitchHint");
	}
	qualityHint->setText(text);
}

/* ------------------------------------------------------------------------ */
/* Row                                                                      */

MultistreamRow::MultistreamRow(QWidget *parent, const QString &id_, bool primary_)
	: QWidget(parent),
	  id(id_),
	  primary(primary_)
{
	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(6, 4, 6, 4);
	layout->setSpacing(8);

	if (!primary) {
		enabled = new QCheckBox(this);
		enabled->setToolTip(QTStr("Multistream.EnabledTooltip"));
		connect(enabled, &QCheckBox::toggled, this, [this](bool on) { emit EnabledToggled(id, on); });
		layout->addWidget(enabled);
	}

	dot = new QLabel(this);
	dot->setFixedSize(10, 10);
	layout->addWidget(dot);

	QVBoxLayout *text = new QVBoxLayout();
	text->setSpacing(0);
	name = new QLabel(this);
	QFont f = name->font();
	f.setBold(true);
	name->setFont(f);
	status = new QLabel(this);
	status->setStyleSheet("color: palette(placeholder-text);");
	status->setTextFormat(Qt::PlainText);
	/* Long error messages must never push the buttons out of view;
	 * the full text stays available in the tooltip. */
	name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	text->addWidget(name);
	text->addWidget(status);
	layout->addLayout(text, 1);

	bitrate = new QLabel(this);
	bitrate->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	bitrate->setMinimumWidth(70);
	layout->addWidget(bitrate);

	if (primary) {
		QLabel *tag = new QLabel(QTStr("Multistream.PrimaryTag"), this);
		tag->setToolTip(QTStr("Multistream.PrimaryTooltip"));
		tag->setStyleSheet("color: palette(placeholder-text);");
		layout->addWidget(tag);
	} else {
		startStop = new QToolButton(this);
		startStop->setAutoRaise(true);
		connect(startStop, &QToolButton::clicked, this, [this] {
			if (state == MultistreamState::Live || state == MultistreamState::Starting ||
			    state == MultistreamState::Reconnecting) {
				emit StopClicked(id);
			} else {
				emit StartClicked(id);
			}
		});
		layout->addWidget(startStop);

		menu = new QToolButton(this);
		menu->setText(QStringLiteral("⋯"));
		menu->setAutoRaise(true);
		menu->setPopupMode(QToolButton::InstantPopup);
		QMenu *m = new QMenu(menu);
		m->addAction(QTStr("Multistream.Edit"), this, [this] { emit EditClicked(id); });
		m->addAction(QTStr("Multistream.Remove"), this, [this] { emit RemoveClicked(id); });
		menu->setMenu(m);
		layout->addWidget(menu);
	}

	SetStatus(MultistreamState::Offline, {}, 0.0);
}

void MultistreamRow::SetName(const QString &n)
{
	name->setText(n);
}

void MultistreamRow::SetEnabled(bool on)
{
	if (!enabled) {
		return;
	}
	QSignalBlocker block(enabled);
	enabled->setChecked(on);
	name->setEnabled(on);
}

void MultistreamRow::SetStatus(MultistreamState s, const QString &message, double kbps)
{
	state = s;
	dot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 5px;").arg(StateColor(s)));

	QString text = StateText(s);
	if (!message.isEmpty()) {
		text += QStringLiteral(" — ") + message;
	}
	status->setText(text);
	status->setToolTip(text);

	bitrate->setText(s == MultistreamState::Live && kbps > 0.0
				 ? QStringLiteral("%1 kb/s").arg(QString::number(kbps, 'f', 0))
				 : QString());
	UpdateStartStop();
}

void MultistreamRow::SetPrimaryLive(bool live)
{
	primaryLive = live;
	UpdateStartStop();
}

void MultistreamRow::UpdateStartStop()
{
	if (!startStop) {
		return;
	}
	bool running = state == MultistreamState::Live || state == MultistreamState::Starting ||
		       state == MultistreamState::Reconnecting;
	startStop->setVisible(primaryLive);
	startStop->setText(running ? QTStr("Multistream.StopOne") : QTStr("Multistream.StartOne"));
	startStop->setToolTip(running ? QTStr("Multistream.StopOneTooltip") : QTStr("Multistream.StartOneTooltip"));
}

/* ------------------------------------------------------------------------ */
/* Dock                                                                     */

MultistreamDock::MultistreamDock(QWidget *parent, MultistreamManager *manager_) : QWidget(parent), manager(manager_)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	summary = new QLabel(this);
	summary->setWordWrap(true);
	layout->addWidget(summary);

	QWidget *rowsWidget = new QWidget();
	rowsLayout = new QVBoxLayout(rowsWidget);
	rowsLayout->setContentsMargins(0, 0, 0, 0);
	rowsLayout->setSpacing(0);
	rowsLayout->addStretch();

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(rowsWidget);
	layout->addWidget(scroll, 1);

	QPushButton *add = new QPushButton(QTStr("Multistream.AddDestination"), this);
	connect(add, &QPushButton::clicked, this, &MultistreamDock::AddClicked);
	layout->addWidget(add);

	connect(manager, &MultistreamManager::DestinationsChanged, this, &MultistreamDock::Rebuild);
	connect(manager, &MultistreamManager::StatusChanged, this, &MultistreamDock::RefreshStatus);

	OBSBasic *main = OBSBasic::Get();
	connect(main, &OBSBasic::StreamingStarting, this, [this] {
		primaryStarting = true;
		RefreshPrimary();
	});
	connect(main, &OBSBasic::StreamingStarted, this, [this] {
		primaryStarting = false;
		primaryLive = true;
		RefreshPrimary();
	});
	connect(main, &OBSBasic::StreamingStopped, this, [this] {
		primaryStarting = false;
		primaryLive = false;
		RefreshPrimary();
	});

	/* Primary bitrate, and keep its name in sync with Settings -> Stream */
	QTimer *timer = new QTimer(this);
	timer->setInterval(1000);
	connect(timer, &QTimer::timeout, this, &MultistreamDock::RefreshPrimary);
	timer->start();

	Rebuild();
}

void MultistreamDock::Rebuild()
{
	for (auto &[id, row] : rows) {
		if (row) {
			row->deleteLater();
		}
	}
	rows.clear();

	if (!primaryRow) {
		primaryRow = new MultistreamRow(this, QString(), true);
		rowsLayout->insertWidget(0, primaryRow);
	}

	int insertAt = 1;
	for (const auto &d : manager->Destinations()) {
		MultistreamRow *row = new MultistreamRow(this, d.id, false);
		row->SetName(d.customQuality ? QStringLiteral("%1  ·  %2 kbps").arg(d.name).arg(d.videoBitrate)
					     : d.name);
		row->SetEnabled(d.enabled);
		row->SetPrimaryLive(primaryLive);
		MultistreamStatus st = manager->Status(d.id);
		row->SetStatus(st.state, st.message, st.kbps);

		connect(row, &MultistreamRow::EnabledToggled, manager, &MultistreamManager::SetEnabled);
		connect(row, &MultistreamRow::StartClicked, manager, &MultistreamManager::StartOne);
		connect(row, &MultistreamRow::StopClicked, manager, &MultistreamManager::StopOne);
		connect(row, &MultistreamRow::EditClicked, this, &MultistreamDock::EditDestination);
		connect(row, &MultistreamRow::RemoveClicked, this, &MultistreamDock::RemoveDestination);

		rowsLayout->insertWidget(insertAt++, row);
		rows[d.id] = row;
	}

	RefreshPrimary();
}

void MultistreamDock::RefreshStatus(const QString &id)
{
	auto it = rows.find(id);
	if (it == rows.end() || !it->second) {
		return;
	}
	MultistreamStatus st = manager->Status(id);
	it->second->SetStatus(st.state, st.message, st.kbps);
	for (const auto &d : manager->Destinations()) {
		if (d.id == id) {
			it->second->SetEnabled(d.enabled);
		}
	}
	UpdateSummary();
}

void MultistreamDock::RefreshPrimary()
{
	if (!primaryRow) {
		return;
	}

	primaryRow->SetName(PrimaryServiceName());

	static uint64_t lastBytes = 0, lastNs = 0;
	double kbps = 0.0;
	MultistreamState st = MultistreamState::Offline;
	OBSOutputAutoRelease out = obs_frontend_get_streaming_output();

	if (primaryStarting) {
		st = MultistreamState::Starting;
	} else if (primaryLive && out) {
		st = obs_output_reconnecting(out) ? MultistreamState::Reconnecting : MultistreamState::Live;
		uint64_t bytes = obs_output_get_total_bytes(out);
		uint64_t now = os_gettime_ns();
		if (lastNs && now > lastNs && bytes >= lastBytes) {
			kbps = double(bytes - lastBytes) * 8.0 / 1000.0 / (double(now - lastNs) / 1e9);
		}
		lastBytes = bytes;
		lastNs = now;
	} else {
		lastBytes = lastNs = 0;
	}
	primaryState = st;
	primaryRow->SetStatus(st, {}, kbps);

	for (auto &[id, row] : rows) {
		if (row) {
			row->SetPrimaryLive(primaryLive);
		}
	}

	UpdateSummary();
}

void MultistreamDock::UpdateSummary()
{
	int enabled = manager->EnabledCount();
	if (primaryLive) {
		int live = manager->LiveCount() + (primaryState == MultistreamState::Live ? 1 : 0);
		summary->setText(QTStr("Multistream.SummaryLive").arg(live).arg(enabled + 1));
	} else if (enabled == 0) {
		summary->setText(QTStr("Multistream.SummaryEmpty"));
	} else {
		summary->setText(QTStr("Multistream.SummaryReady").arg(enabled + 1));
	}
}

void MultistreamDock::AddClicked()
{
	MultistreamDestinationDialog dlg(this, MultistreamDestination::Create(), true);
	if (dlg.exec() == QDialog::Accepted) {
		manager->AddDestination(dlg.Result());
	}
}

void MultistreamDock::EditDestination(const QString &id)
{
	for (const auto &d : manager->Destinations()) {
		if (d.id != id) {
			continue;
		}
		MultistreamDestinationDialog dlg(this, d, false);
		if (dlg.exec() == QDialog::Accepted) {
			manager->UpdateDestination(dlg.Result());
		}
		return;
	}
}

void MultistreamDock::RemoveDestination(const QString &id)
{
	QString name;
	for (const auto &d : manager->Destinations()) {
		if (d.id == id) {
			name = d.name;
		}
	}

	QMessageBox::StandardButton res = OBSMessageBox::question(this, QTStr("Multistream.RemoveTitle"),
								  QTStr("Multistream.RemoveText").arg(name));
	if (res == QMessageBox::Yes) {
		manager->RemoveDestination(id);
	}
}
