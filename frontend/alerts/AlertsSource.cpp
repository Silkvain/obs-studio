/******************************************************************************
    Siegfried: "Alerts" source. See AlertsSource.hpp.

    Threads: the queue and timing run on the UI thread (Manager). Render
    and audio threads only read a small shared state under a mutex.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AlertsSource.hpp"
#include "AlertsEditor.hpp"

#include <multichat/ChatHub.hpp>

#include <OBSApp.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/util_uint64.h>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char *kSourceId = "siegfried_alerts";
constexpr int kGapMs = 400;
constexpr int kFadeMs = 250;
constexpr int kImageDefaultMs = 5000;
constexpr size_t kMaxQueue = 50;

#if defined(_WIN32)
constexpr const char *kDefaultFace = "Segoe UI";
#elif defined(__APPLE__)
constexpr const char *kDefaultFace = "Helvetica Neue";
#else
constexpr const char *kDefaultFace = "Sans Serif";
#endif

QColor FromObs(uint32_t abgr)
{
	return QColor(int(abgr & 0xFF), int((abgr >> 8) & 0xFF), int((abgr >> 16) & 0xFF), int((abgr >> 24) & 0xFF));
}

bool IsImage(const QString &path)
{
	static const QStringList ext = {"png", "jpg", "jpeg", "bmp", "webp", "gif", "tga"};
	return ext.contains(QFileInfo(path).suffix().toLower());
}

} // namespace

/* ------------------------------------------------------------------------ */
/* AlertStyle                                                               */

AlertStyle AlertStyle::Default(StreamEventType type)
{
	AlertStyle s;
	s.fontFace = QString::fromLatin1(kDefaultFace);
	switch (type) {
	case StreamEventType::Follow:
		s.textTemplate = QStringLiteral("{name} just followed!");
		break;
	case StreamEventType::Subscribe:
		s.textTemplate = QStringLiteral("{name} subscribed!");
		break;
	case StreamEventType::Resubscribe:
		s.textTemplate = QStringLiteral("{name} resubscribed for {months} months!");
		break;
	case StreamEventType::GiftSub:
		s.textTemplate = QStringLiteral("{name} gifted {count} subs!");
		break;
	case StreamEventType::Bits:
		s.textTemplate = QStringLiteral("{name} cheered {amount}!");
		break;
	case StreamEventType::Donation:
		s.textTemplate = QStringLiteral("{name} donated {amount}!");
		break;
	case StreamEventType::Raid:
		s.textTemplate = QStringLiteral("{name} is raiding with {count} viewers!");
		break;
	case StreamEventType::Host:
		s.textTemplate = QStringLiteral("{name} is hosting with {count} viewers!");
		break;
	case StreamEventType::SuperChat:
		s.textTemplate = QStringLiteral("{name} sent a {amount} Super Chat!");
		break;
	case StreamEventType::Membership:
		s.textTemplate = QStringLiteral("{name} became a member!");
		break;
	case StreamEventType::Rant:
		s.textTemplate = QStringLiteral("{name} ranted {amount}!");
		break;
	}
	return s;
}

AlertStyle AlertStyle::FromData(obs_data_t *d, StreamEventType type)
{
	AlertStyle s = Default(type);
	if (!d) {
		return s;
	}
	auto str = [&](const char *k, QString &out) {
		if (obs_data_has_user_value(d, k)) {
			out = QString::fromUtf8(obs_data_get_string(d, k));
		}
	};
	auto num = [&](const char *k, int &out) {
		if (obs_data_has_user_value(d, k)) {
			out = int(obs_data_get_int(d, k));
		}
	};
	if (obs_data_has_user_value(d, "enabled")) {
		s.enabled = obs_data_get_bool(d, "enabled");
	}
	str("media", s.media);
	str("sound", s.sound);
	num("volume", s.volume);
	num("duration_ms", s.durationMs);
	str("template", s.textTemplate);
	if (obs_data_has_user_value(d, "show_message")) {
		s.showMessage = obs_data_get_bool(d, "show_message");
	}
	str("font_face", s.fontFace);
	num("font_size", s.fontSize);
	if (obs_data_has_user_value(d, "bold")) {
		s.bold = obs_data_get_bool(d, "bold");
	}
	if (obs_data_has_user_value(d, "color")) {
		s.color = uint32_t(obs_data_get_int(d, "color"));
	}
	if (obs_data_has_user_value(d, "outline")) {
		s.outline = uint32_t(obs_data_get_int(d, "outline"));
	}
	if (obs_data_has_user_value(d, "text_w")) {
		s.textBox = QRectF(obs_data_get_double(d, "text_x"), obs_data_get_double(d, "text_y"),
				   obs_data_get_double(d, "text_w"), obs_data_get_double(d, "text_h"));
	}
	num("show_at_ms", s.showAtMs);
	num("hide_at_ms", s.hideAtMs);
	if (obs_data_has_user_value(d, "min_amount")) {
		s.minAmount = obs_data_get_double(d, "min_amount");
	}
	return s;
}

void AlertStyle::ToData(obs_data_t *d) const
{
	obs_data_set_bool(d, "enabled", enabled);
	obs_data_set_string(d, "media", media.toUtf8().constData());
	obs_data_set_string(d, "sound", sound.toUtf8().constData());
	obs_data_set_int(d, "volume", volume);
	obs_data_set_int(d, "duration_ms", durationMs);
	obs_data_set_string(d, "template", textTemplate.toUtf8().constData());
	obs_data_set_bool(d, "show_message", showMessage);
	obs_data_set_string(d, "font_face", fontFace.toUtf8().constData());
	obs_data_set_int(d, "font_size", fontSize);
	obs_data_set_bool(d, "bold", bold);
	obs_data_set_int(d, "color", color);
	obs_data_set_int(d, "outline", outline);
	obs_data_set_double(d, "text_x", textBox.x());
	obs_data_set_double(d, "text_y", textBox.y());
	obs_data_set_double(d, "text_w", textBox.width());
	obs_data_set_double(d, "text_h", textBox.height());
	obs_data_set_int(d, "show_at_ms", showAtMs);
	obs_data_set_int(d, "hide_at_ms", hideAtMs);
	obs_data_set_double(d, "min_amount", minAmount);
}

QString Alerts::Expand(const QString &tmpl, const StreamEvent &e)
{
	QString name = e.user.displayName.isEmpty() ? e.user.login : e.user.displayName;
	QString amount = e.AmountText();
	if (e.type == StreamEventType::GiftSub || e.type == StreamEventType::Raid ||
	    e.type == StreamEventType::Host) {
		amount = QString::number(std::max(1, e.count));
	}
	QString out = tmpl;
	out.replace(QLatin1String("{name}"), name);
	out.replace(QLatin1String("{amount}"), amount);
	out.replace(QLatin1String("{count}"), QString::number(std::max(1, e.count)));
	out.replace(QLatin1String("{months}"), QString::number(std::max(1, e.months)));
	out.replace(QLatin1String("{tier}"), e.tier);
	out.replace(QLatin1String("{message}"), e.message);
	out.replace(QLatin1String("{platform}"), ChatPlatformName(e.platform));
	return out.trimmed();
}

QStringList Alerts::BlockedWords()
{
	const char *v = config_get_string(App()->GetUserConfig(), "Alerts", "BlockedWords");
	QStringList out;
	for (const QString &w : QString::fromUtf8(v ? v : "").split(',', Qt::SkipEmptyParts)) {
		if (!w.trimmed().isEmpty()) {
			out << w.trimmed();
		}
	}
	return out;
}

void Alerts::SetBlockedWords(const QStringList &words)
{
	config_set_string(App()->GetUserConfig(), "Alerts", "BlockedWords", words.join(',').toUtf8().constData());
}

/* ------------------------------------------------------------------------ */
/* Instances and the shared player                                          */

namespace {

struct Instance {
	obs_source_t *source = nullptr;
	std::mutex lock;
	int width = 800, height = 600;
	bool hear = true;
	OBSData alerts; /* per-type styles, copied from settings */
	QImage pending;
	bool dirty = false;
	gs_texture_t *tex = nullptr;
	uint32_t texW = 0, texH = 0;
};

struct Playing {
	StreamEvent event;
	AlertStyle style;
	OBSSource media;
	OBSSource sound;
	qint64 started = 0;
	int durationMs = 0;
	bool preview = false;
	int previewMs = 0;
	obs_source_t *previewSource = nullptr;
	/* OBS only plays/decodes media that is "on screen": link the alert's
	 * media to every Alerts source while it plays */
	std::vector<std::pair<OBSWeakSource, OBSSource>> links;
	bool sawPlaying = false; /* "ended" only counts after it really played */
	qint64 seekUntil = 0;    /* preview: keep re-seeking until the file is open */
	qint64 lastSeek = 0;
};

class Manager : public QObject {
public:
	Manager()
	{
		timer.setInterval(33);
		connect(&timer, &QTimer::timeout, this, [this] { Tick(); });
		timer.start();
	}

	void Attach(ChatHub *h)
	{
		hub = h;
		connect(h, &ChatHub::AlertRequested, this, [this](const StreamEvent &e) { Enqueue(e); });
		connect(h, &ChatHub::SkipAlertRequested, this, [this] {
			if (hub && hub->AlertsPaused()) {
				queue.clear(); /* silenced: drop what was waiting too */
			}
			Finish();
		});
	}

	void Add(Instance *i)
	{
		std::lock_guard l(instancesMutex);
		instances.push_back(i);
	}

	void Remove(Instance *i)
	{
		std::lock_guard l(instancesMutex);
		instances.erase(std::remove(instances.begin(), instances.end(), i), instances.end());
		if (audioOwner == i) {
			audioOwner = nullptr;
		}
	}

	/* Render/audio threads */
	void Current(OBSSource &media, OBSSource &sound, bool &showMedia)
	{
		std::lock_guard l(stateMutex);
		media = shownMedia;
		sound = shownSound;
		showMedia = mediaVisible;
	}
	Instance *AudioOwner() const { return audioOwner; }
	bool Previewing() const { return previewing; }

	void Preview(obs_source_t *src, const StreamEvent *event, int timeMs)
	{
		if (!event) {
			if (playing && playing->preview) {
				Finish();
			}
			return;
		}
		if (!playing || !playing->preview || playing->event.type != event->type ||
		    playing->previewSource != src) {
			if (playing) {
				Finish(false);
			}
			Start(*event, StyleFor(src, event->type), true);
			playing->previewSource = src;
			previewing = true;
		}
		/* Style may have changed in the editor: re-read every time */
		playing->style = StyleFor(src, event->type);
		playing->previewMs = timeMs;
		playing->seekUntil = QDateTime::currentMSecsSinceEpoch() + 3000;
		if (playing->media) {
			obs_source_media_play_pause(playing->media, true);
			obs_source_media_set_time(playing->media, timeMs);
		}
		lastTextKey.clear();
	}

	int Duration(const QString &path)
	{
		auto it = children.find(path);
		if (it == children.end() || IsImage(path)) {
			return IsImage(path) ? kImageDefaultMs : -1;
		}
		int64_t d = obs_source_media_get_duration(it->second);
		return d > 0 ? int(d) : -1;
	}

	QSize Size(const QString &path)
	{
		auto it = children.find(path);
		if (it == children.end()) {
			return {};
		}
		return QSize(int(obs_source_get_width(it->second)), int(obs_source_get_height(it->second)));
	}

	/* Make sure a media file has a child (so the editor can ask its length) */
	void Prepare(const QString &path)
	{
		if (!path.isEmpty()) {
			Child(path, false);
		}
	}

private:
	AlertStyle StyleFor(obs_source_t *src, StreamEventType type)
	{
		std::lock_guard l(instancesMutex);
		Instance *pick = nullptr;
		for (Instance *i : instances) {
			if (!src || i->source == src) {
				pick = i;
				break;
			}
		}
		if (!pick) {
			return AlertStyle::Default(type);
		}
		std::lock_guard il(pick->lock);
		OBSDataAutoRelease d = obs_data_get_obj(pick->alerts, StreamEventKey(type));
		return AlertStyle::FromData(d, type);
	}

	bool HasInstances()
	{
		std::lock_guard l(instancesMutex);
		return !instances.empty();
	}

	bool Blocked(const StreamEvent &e)
	{
		QString hay = (e.user.displayName + ' ' + e.user.login + ' ' + e.message).toLower();
		for (const QString &w : Alerts::BlockedWords()) {
			if (hay.contains(w.toLower())) {
				return true;
			}
		}
		return false;
	}

	void Enqueue(const StreamEvent &e)
	{
		if (!HasInstances()) {
			return; /* no Alerts source in this scene collection */
		}
		if (!e.test) {
			AlertStyle st = StyleFor(nullptr, e.type);
			if (!st.enabled || (st.minAmount > 0 && e.amount > 0 && e.amount < st.minAmount)) {
				return;
			}
		}
		if (Blocked(e)) {
			blog(LOG_INFO, "[alerts] Skipped an alert: blocked word in name or message");
			return;
		}
		if (queue.size() >= kMaxQueue) {
			queue.pop_front();
		}
		queue.push_back(e);
	}

	OBSSource Child(const QString &path, bool sound)
	{
		QString key = (sound ? QStringLiteral("s|") : QStringLiteral("m|")) + path;
		auto it = children.find(key);
		if (it != children.end()) {
			return it->second;
		}
		OBSDataAutoRelease s = obs_data_create();
		const char *id;
		if (!sound && IsImage(path)) {
			id = "image_source";
			obs_data_set_string(s, "file", path.toUtf8().constData());
		} else {
			id = "ffmpeg_source";
			obs_data_set_string(s, "local_file", path.toUtf8().constData());
			obs_data_set_bool(s, "is_local_file", true);
			obs_data_set_bool(s, "looping", false);
			obs_data_set_bool(s, "restart_on_activate", false);
			obs_data_set_bool(s, "close_when_inactive", false);
			obs_data_set_bool(s, "clear_on_media_end", true);
			obs_data_set_bool(s, "hw_decode", false); /* transparent webm: software decoder keeps alpha */
		}
		OBSSourceAutoRelease src = obs_source_create_private(id, "siegfried-alert-media", s);
		if (!src) {
			return nullptr;
		}
		if (id == std::string("ffmpeg_source")) {
			obs_source_media_stop(src);
		}
		OBSSource keep = src.Get();
		children.emplace(key, keep);
		if (!sound) {
			children.emplace(path, keep); /* for Duration() */
		}
		return keep;
	}

	void Start(const StreamEvent &e, const AlertStyle &st, bool preview)
	{
		playing.emplace();
		playing->event = e;
		playing->style = st;
		playing->preview = preview;
		playing->started = QDateTime::currentMSecsSinceEpoch();
		if (!st.media.isEmpty() && QFile::exists(st.media)) {
			playing->media = Child(st.media, false);
		}
		if (!st.sound.isEmpty() && QFile::exists(st.sound) && !preview) {
			playing->sound = Child(st.sound, true);
		}
		bool hear = true;
		{
			std::lock_guard l(instancesMutex);
			if (!instances.empty()) {
				std::lock_guard il(instances.front()->lock);
				hear = instances.front()->hear;
			}
		}
		{
			std::lock_guard l(instancesMutex);
			for (Instance *i : instances) {
				for (obs_source_t *child : {playing->media.Get(), playing->sound.Get()}) {
					if (child && obs_source_add_active_child(i->source, child)) {
						playing->links.emplace_back(OBSGetWeakRef(i->source), OBSSource(child));
					}
				}
			}
		}
		for (obs_source_t *child : {playing->media.Get(), playing->sound.Get()}) {
			if (child) {
				obs_source_inc_showing(child);
			}
		}
		if (playing->media && !IsImage(st.media)) {
			obs_source_set_volume(playing->media, 1.0f);
			obs_source_set_monitoring_enabled(playing->media, hear && !preview);
			obs_source_media_restart(playing->media);
		}
		if (playing->sound) {
			obs_source_set_volume(playing->sound, float(st.volume) / 100.0f);
			obs_source_set_monitoring_enabled(playing->sound, hear);
			obs_source_media_restart(playing->sound);
		}
		{
			std::lock_guard l(stateMutex);
			shownMedia = playing->media;
			shownSound = playing->sound;
			mediaVisible = true;
		}
		lastTextKey.clear();
		if (!preview) {
			blog(LOG_INFO, "[alerts] Playing %s alert", StreamEventKey(e.type));
		}
	}

	void Finish(bool scheduleNext = true)
	{
		if (!playing) {
			return;
		}
		if (playing->media && !IsImage(playing->style.media)) {
			obs_source_media_stop(playing->media);
		}
		if (playing->sound) {
			obs_source_media_stop(playing->sound);
		}
		for (auto &[parent, child] : playing->links) {
			OBSSourceAutoRelease p = OBSGetStrongRef(parent);
			if (p) {
				obs_source_remove_active_child(p, child);
			}
		}
		for (obs_source_t *child : {playing->media.Get(), playing->sound.Get()}) {
			if (child) {
				obs_source_dec_showing(child);
			}
		}
		{
			std::lock_guard l(stateMutex);
			shownMedia = nullptr;
			shownSound = nullptr;
			mediaVisible = false;
		}
		playing.reset();
		previewing = false;
		PaintText(QString(), QString(), AlertStyle(), 0.0);
		if (scheduleNext) {
			idleUntil = QDateTime::currentMSecsSinceEpoch() + kGapMs;
		}
	}

	int DurationOf(const Playing &p)
	{
		if (p.style.durationMs > 0) {
			return p.style.durationMs;
		}
		if (p.media && !IsImage(p.style.media)) {
			int64_t d = obs_source_media_get_duration(p.media);
			if (d > 0) {
				return int(d);
			}
			return -1; /* not known yet */
		}
		return kImageDefaultMs;
	}

	void Tick()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();
		UpdateAudioOwner();

		if (!playing) {
			if (!queue.empty() && now >= idleUntil) {
				StreamEvent e = queue.front();
				queue.pop_front();
				Start(e, StyleFor(nullptr, e.type), false);
			} else {
				return;
			}
		}

		Playing &p = *playing;
		int elapsed = p.preview ? p.previewMs : int(now - p.started);
		if (p.preview && now < p.seekUntil && now - p.lastSeek >= 250 && p.media && !IsImage(p.style.media)) {
			p.lastSeek = now;
			obs_source_media_play_pause(p.media, true);
			obs_source_media_set_time(p.media, p.previewMs);
		}
		int duration = DurationOf(p);
		if (!p.preview) {
			bool isVideo = p.media && !IsImage(p.style.media);
			obs_media_state state = isVideo ? obs_source_media_get_state(p.media) : OBS_MEDIA_STATE_NONE;
			if (state == OBS_MEDIA_STATE_PLAYING) {
				p.sawPlaying = true;
			}
			/* The player can report a stale "ended" right after a restart:
			 * only believe it once the alert has been up a while */
			bool ended = isVideo && p.sawPlaying && state == OBS_MEDIA_STATE_ENDED && elapsed > 1500;
			if ((duration > 0 && elapsed >= duration) || ended || (duration < 0 && elapsed > 30000)) {
				Finish();
				return;
			}
		}

		/* Text: fade in at showAt, out at hideAt */
		int total = duration > 0 ? duration : 8000;
		int showAt = p.style.showAtMs;
		int hideAt = p.style.hideAtMs > 0 ? p.style.hideAtMs : total - 300;
		double opacity = 0.0;
		if (elapsed >= showAt && elapsed <= hideAt) {
			opacity = std::min(1.0, double(elapsed - showAt) / kFadeMs);
			opacity = std::min(opacity, double(hideAt - elapsed) / kFadeMs);
			opacity = std::clamp(opacity, 0.0, 1.0);
		}
		QString title = Alerts::Expand(p.style.textTemplate, p.event);
		QString message = (p.style.showMessage && p.event.type != StreamEventType::GiftSub) ? p.event.message
												     : QString();
		PaintText(title, message, p.style, opacity);
	}

	void UpdateAudioOwner()
	{
		std::lock_guard l(instancesMutex);
		Instance *owner = nullptr;
		for (Instance *i : instances) {
			if (obs_source_active(i->source)) {
				owner = i;
				break;
			}
		}
		audioOwner = owner;
	}

	/* Text image per instance, only when what's shown changes */
	void PaintText(const QString &title, const QString &message, const AlertStyle &st, double opacity)
	{
		int step = int(std::round(opacity * 10));
		QSize mediaSize;
		{
			std::lock_guard l(stateMutex);
			if (shownMedia) {
				mediaSize = QSize(int(obs_source_get_width(shownMedia)), int(obs_source_get_height(shownMedia)));
			}
		}
		QString key = QStringLiteral("%1|%2|%3|%4x%5|%6|%7,%8,%9,%10|%11|%12|%13")
				      .arg(title, message)
				      .arg(step)
				      .arg(mediaSize.width())
				      .arg(mediaSize.height())
				      .arg(st.fontSize)
				      .arg(st.textBox.x())
				      .arg(st.textBox.y())
				      .arg(st.textBox.width())
				      .arg(st.textBox.height())
				      .arg(st.fontFace)
				      .arg(st.color)
				      .arg(st.outline);
		if (key == lastTextKey) {
			return;
		}
		lastTextKey = key;

		std::lock_guard l(instancesMutex);
		for (Instance *inst : instances) {
			int w, h;
			{
				std::lock_guard il(inst->lock);
				w = inst->width;
				h = inst->height;
			}
			QImage img(w, h, QImage::Format_RGBA8888);
			img.fill(Qt::transparent);
			if (step > 0 && !title.isEmpty()) {
				QPainter p(&img);
				p.setRenderHint(QPainter::Antialiasing);
				p.setOpacity(step / 10.0);
				QRectF area = FitRect(mediaSize, w, h);
				double scale = mediaSize.height() > 0 ? area.height() / mediaSize.height() : area.height() / 600.0;
				QRectF box(area.x() + st.textBox.x() * area.width(), area.y() + st.textBox.y() * area.height(),
					   st.textBox.width() * area.width(), st.textBox.height() * area.height());
				DrawText(p, box, title, message, st, scale);
			}
			std::lock_guard il(inst->lock);
			inst->pending = std::move(img);
			inst->dirty = true;
		}
	}

public:
	static QRectF FitRect(QSize media, int w, int h)
	{
		if (media.width() <= 0 || media.height() <= 0) {
			return QRectF(0, 0, w, h);
		}
		double s = std::min(double(w) / media.width(), double(h) / media.height());
		double mw = media.width() * s, mh = media.height() * s;
		return QRectF((w - mw) / 2.0, (h - mh) / 2.0, mw, mh);
	}

private:
	static void DrawText(QPainter &p, const QRectF &box, const QString &title, const QString &message,
			     const AlertStyle &st, double scale)
	{
		QFont font(st.fontFace);
		font.setBold(st.bold);
		double px = std::max(6.0, st.fontSize * scale);
		font.setPixelSize(int(px));
		/* Long names shrink to fit the box instead of overflowing */
		while (QFontMetricsF(font).horizontalAdvance(title) > box.width() && font.pixelSize() > 8) {
			font.setPixelSize(font.pixelSize() - 1);
		}
		QFontMetricsF fm(font);
		QFont small = font;
		small.setBold(false);
		small.setPixelSize(std::max(8, int(font.pixelSize() * 0.55)));
		QFontMetricsF sfm(small);
		QString msg = message.isEmpty() ? QString()
						: sfm.elidedText(message, Qt::ElideRight, box.width());

		double blockH = fm.height() + (msg.isEmpty() ? 0 : sfm.height() * 1.1);
		double y = box.center().y() - blockH / 2.0;

		auto line = [&](const QString &text, const QFont &f, double top) {
			QFontMetricsF m(f);
			double x = box.center().x() - m.horizontalAdvance(text) / 2.0;
			QPainterPath path;
			path.addText(QPointF(x, top + m.ascent()), f, text);
			QColor outline = FromObs(st.outline);
			if (outline.alpha() > 0) {
				p.strokePath(path, QPen(outline, std::max(1.0, f.pixelSize() * 0.12), Qt::SolidLine,
							Qt::RoundCap, Qt::RoundJoin));
			}
			p.fillPath(path, FromObs(st.color));
		};
		line(title, font, y);
		if (!msg.isEmpty()) {
			line(msg, small, y + fm.height() * 1.05);
		}
	}

	QPointer<ChatHub> hub;
	std::mutex instancesMutex;
	std::vector<Instance *> instances;
	std::atomic<Instance *> audioOwner{nullptr};
	std::atomic<bool> previewing{false};

	std::mutex stateMutex;
	OBSSource shownMedia;
	OBSSource shownSound;
	bool mediaVisible = false;

	std::deque<StreamEvent> queue;
	std::optional<Playing> playing;
	qint64 idleUntil = 0;
	std::map<QString, OBSSource> children;
	QString lastTextKey;
	QTimer timer;
};

Manager *g_manager = nullptr;
std::string g_name;
std::atomic<bool> g_editorRendering{false};

/* ------------------------------------------------------------------------ */
/* Pre-fill from an imported bundle                                         */

void PrefillFromPackage(obs_data_t *settings)
{
	char *col = obs_frontend_get_current_scene_collection();
	QString collection = QString::fromUtf8(col ? col : "");
	bfree(col);
	if (collection.isEmpty()) {
		return;
	}
	std::filesystem::path base = App()->userConfigLocation / std::filesystem::u8path("obs-studio/siegfried/assets");
	QDirIterator it(QString::fromStdString(base.u8string()), {QStringLiteral("siegfried-package.json")}, QDir::Files,
			QDirIterator::Subdirectories);
	while (it.hasNext()) {
		QFile f(it.next());
		if (!f.open(QIODevice::ReadOnly)) {
			continue;
		}
		QJsonObject m = QJsonDocument::fromJson(f.readAll()).object();
		if (m.value("collection").toString() != collection) {
			continue;
		}
		OBSDataAutoRelease alerts = obs_data_create();
		for (const QJsonValue &v : m.value("alerts").toArray()) {
			QJsonObject a = v.toObject();
			StreamEventType type;
			if (!StreamEventFromKey(a.value("event").toString(), type)) {
				continue;
			}
			AlertStyle st = AlertStyle::Default(type);
			st.media = a.value("file").toString();
			/* Bundle art already says "NEW FOLLOWER" etc.: just the name
			 * (and the amount where there is one) */
			switch (type) {
			case StreamEventType::Follow:
			case StreamEventType::Subscribe:
			case StreamEventType::Membership:
				st.textTemplate = QStringLiteral("{name}");
				break;
			default:
				st.textTemplate = QStringLiteral("{name} · {amount}");
				break;
			}
			st.textBox = QRectF(0.15, 0.42, 0.7, 0.14);
			st.showAtMs = 900;
			OBSDataAutoRelease d = obs_data_create();
			st.ToData(d);
			obs_data_set_obj(alerts, StreamEventKey(type), d);
		}
		obs_data_set_obj(settings, "alerts", alerts);
		blog(LOG_INFO, "[alerts] Filled alerts from imported bundle '%s'",
		     m.value("title").toString().toUtf8().constData());
		return;
	}
}

/* ------------------------------------------------------------------------ */
/* obs_source_info                                                          */

void ReadSettings(Instance *inst, obs_data_t *s)
{
	std::lock_guard l(inst->lock);
	inst->width = std::max(50, int(obs_data_get_int(s, "width")));
	inst->height = std::max(50, int(obs_data_get_int(s, "height")));
	inst->hear = obs_data_get_bool(s, "hear_alerts");
	OBSDataAutoRelease a = obs_data_get_obj(s, "alerts");
	inst->alerts = a ? a.Get() : nullptr;
	if (!inst->alerts) {
		OBSDataAutoRelease empty = obs_data_create();
		inst->alerts = empty.Get();
	}
}

const char *GetName(void *)
{
	return g_name.c_str();
}

void *Create(obs_data_t *settings, obs_source_t *source)
{
	if (!obs_data_has_user_value(settings, "alerts")) {
		PrefillFromPackage(settings);
	}
	auto *inst = new Instance();
	inst->source = source;
	ReadSettings(inst, settings);
	if (g_manager) {
		g_manager->Add(inst);
	}
	return inst;
}

void Destroy(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	if (g_manager) {
		g_manager->Remove(inst);
	}
	if (inst->tex) {
		obs_enter_graphics();
		gs_texture_destroy(inst->tex);
		obs_leave_graphics();
	}
	delete inst;
}

void Update(void *data, obs_data_t *settings)
{
	ReadSettings(static_cast<Instance *>(data), settings);
}

uint32_t Width(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	std::lock_guard l(inst->lock);
	return uint32_t(inst->width);
}

uint32_t Height(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	std::lock_guard l(inst->lock);
	return uint32_t(inst->height);
}

void Render(void *data, gs_effect_t *)
{
	auto *inst = static_cast<Instance *>(data);
	OBSSource media, sound;
	bool show = false;
	if (g_manager) {
		g_manager->Current(media, sound, show);
	}
	/* Editor preview: only inside the editor window, never on stream */
	bool hidePreview = g_manager && g_manager->Previewing() && !g_editorRendering;
	int w, h;
	{
		std::lock_guard l(inst->lock);
		w = inst->width;
		h = inst->height;
		if (inst->dirty && !inst->pending.isNull()) {
			const QImage &img = inst->pending;
			uint32_t iw = uint32_t(img.width()), ih = uint32_t(img.height());
			if (!inst->tex || inst->texW != iw || inst->texH != ih) {
				if (inst->tex) {
					gs_texture_destroy(inst->tex);
				}
				const uint8_t *bits = img.constBits();
				inst->tex = gs_texture_create(iw, ih, GS_RGBA, 1, &bits, GS_DYNAMIC);
				inst->texW = iw;
				inst->texH = ih;
			} else {
				gs_texture_set_image(inst->tex, img.constBits(), uint32_t(img.bytesPerLine()), false);
			}
			inst->dirty = false;
		}
	}

	if (hidePreview) {
		return;
	}
	if (show && media) {
		QSize ms(int(obs_source_get_width(media)), int(obs_source_get_height(media)));
		if (ms.width() > 0 && ms.height() > 0) {
			QRectF r = Manager::FitRect(ms, w, h);
			gs_matrix_push();
			gs_matrix_translate3f(float(r.x()), float(r.y()), 0.0f);
			gs_matrix_scale3f(float(r.width() / ms.width()), float(r.height() / ms.height()), 1.0f);
			obs_source_video_render(media);
			gs_matrix_pop();
		}
	}

	if (inst->tex) {
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
		gs_effect_set_texture(image, inst->tex);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(inst->tex, 0, 0, 0);
		}
		gs_blend_state_pop();
	}
}

void EnumActive(void *data, obs_source_enum_proc_t cb, void *param)
{
	(void)data;
	OBSSource media, sound;
	bool show = false;
	if (g_manager) {
		g_manager->Current(media, sound, show);
	}
	obs_source_t *self = static_cast<Instance *>(data)->source;
	if (media) {
		cb(self, media, param);
	}
	if (sound) {
		cb(self, sound, param);
	}
}

/* Mix the alert's sound into the stream (once, via one Alerts source) */
bool AudioRender(void *data, uint64_t *ts_out, obs_source_audio_mix *out, uint32_t mixers, size_t channels,
		 size_t sample_rate)
{
	auto *inst = static_cast<Instance *>(data);
	if (!g_manager || g_manager->AudioOwner() != inst) {
		return false;
	}
	OBSSource media, sound;
	bool show = false;
	g_manager->Current(media, sound, show);
	obs_source_t *kids[2] = {media, sound};

	uint64_t timestamp = 0;
	for (obs_source_t *k : kids) {
		if (k && !obs_source_audio_pending(k)) {
			uint64_t t = obs_source_get_audio_timestamp(k);
			if (t && (!timestamp || t < timestamp)) {
				timestamp = t;
			}
		}
	}
	if (!timestamp) {
		return false;
	}
	for (obs_source_t *k : kids) {
		if (!k || obs_source_audio_pending(k)) {
			continue;
		}
		uint64_t t = obs_source_get_audio_timestamp(k);
		if (!t) {
			continue;
		}
		size_t pos = size_t(util_mul_div64(t - timestamp, sample_rate, 1000000000ULL));
		if (pos >= AUDIO_OUTPUT_FRAMES) {
			continue;
		}
		size_t count = AUDIO_OUTPUT_FRAMES - pos;
		obs_source_audio_mix child = {};
		obs_source_get_audio_mix(k, &child);
		for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
			if ((mixers & (1 << mix)) == 0) {
				continue;
			}
			for (size_t ch = 0; ch < channels; ch++) {
				float *o = out->output[mix].data[ch] + pos;
				const float *in = child.output[mix].data[ch];
				for (size_t i = 0; i < count; i++) {
					o[i] += in[i];
				}
			}
		}
	}
	*ts_out = timestamp;
	(void)sample_rate;
	return true;
}

void Defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "width", 800);
	obs_data_set_default_int(s, "height", 600);
	obs_data_set_default_bool(s, "hear_alerts", true);
}

bool EditClicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *inst = static_cast<Instance *>(data);
	AlertsEditor::Open(inst->source);
	return false;
}

obs_properties_t *Properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_button2(props, "edit", Str("Alerts.Edit"), EditClicked, data);
	obs_properties_add_int(props, "width", Str("Alerts.Width"), 50, 4096, 1);
	obs_properties_add_int(props, "height", Str("Alerts.Height"), 50, 4096, 1);
	obs_property_t *hear = obs_properties_add_bool(props, "hear_alerts", Str("Alerts.Hear"));
	obs_property_set_long_description(hear, Str("Alerts.HearHint"));
	return props;
}

} // namespace

void Alerts::Register()
{
	if (g_manager) {
		return;
	}
	g_manager = new Manager();
	g_name = QTStr("Alerts.Name").toStdString();

	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = GetName;
	info.create = Create;
	info.destroy = Destroy;
	info.update = Update;
	info.get_width = Width;
	info.get_height = Height;
	info.video_render = Render;
	info.enum_active_sources = EnumActive;
	info.audio_render = AudioRender;
	info.get_defaults = Defaults;
	info.get_properties = Properties;
	obs_register_source(&info);
}

void Alerts::AttachHub(ChatHub *hub)
{
	if (g_manager && hub) {
		g_manager->Attach(hub);
	}
}

void Alerts::Preview(obs_source_t *src, const StreamEvent *event, int timeMs)
{
	if (g_manager) {
		g_manager->Preview(src, event, timeMs);
	}
}

int Alerts::MediaDurationMs(const QString &path)
{
	if (!g_manager || path.isEmpty()) {
		return -1;
	}
	g_manager->Prepare(path);
	return g_manager->Duration(path);
}

QSize Alerts::MediaSize(const QString &path)
{
	if (!g_manager || path.isEmpty()) {
		return {};
	}
	g_manager->Prepare(path);
	return g_manager->Size(path);
}

void Alerts::SetEditorRendering(bool on)
{
	g_editorRendering = on;
}
