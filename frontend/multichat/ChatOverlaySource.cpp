/******************************************************************************
    Siegfried: "Chat (all platforms)" source. See ChatOverlaySource.hpp.

    Threading: OBS may create/update/destroy sources from any thread and
    renders on the graphics thread. Painting (Qt) happens on the UI thread
    into a QImage; the graphics thread only uploads the finished image.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatOverlaySource.hpp"
#include "ChatHub.hpp"
#include "MultichatDock.hpp"

#include <OBSApp.hpp>

#include <obs-module.h>
#include <obs.hpp>

#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char *kSourceId = "siegfried_chat";
constexpr int kMaxMessages = 200;
constexpr qint64 kFadeMs = 1000;

#if defined(_WIN32)
constexpr const char *kDefaultFace = "Segoe UI";
#elif defined(__APPLE__)
constexpr const char *kDefaultFace = "Helvetica Neue";
#else
constexpr const char *kDefaultFace = "Sans Serif";
#endif

struct Style {
	int width = 420;
	int height = 640;
	QString face = QString::fromLatin1(kDefaultFace);
	int fontSize = 24;
	bool bold = false;
	int lifetimeSec = 0; /* 0 = keep */
	uint32_t bgColor = 0x00000000;   /* OBS: 0xAABBGGRR */
	uint32_t textColor = 0xFFFFFFFF; /* OBS: 0xAABBGGRR */
	bool icons = true;
	bool names = true;
	bool shadow = true;
	bool samples = true;
};

struct Instance {
	obs_source_t *source = nullptr;
	std::mutex lock;
	Style style;
	QImage pending;
	bool dirty = false;
	gs_texture_t *tex = nullptr;
	uint32_t texW = 0, texH = 0;
};

struct Entry {
	ChatMessage msg;
	qint64 arrived = 0;
};

QColor FromObs(uint32_t abgr)
{
	return QColor(int(abgr & 0xFF), int((abgr >> 8) & 0xFF), int((abgr >> 16) & 0xFF), int((abgr >> 24) & 0xFF));
}

Style ReadStyle(obs_data_t *s)
{
	Style st;
	st.width = std::max(50, int(obs_data_get_int(s, "width")));
	st.height = std::max(50, int(obs_data_get_int(s, "height")));
	OBSDataAutoRelease font = obs_data_get_obj(s, "font");
	if (font) {
		const char *face = obs_data_get_string(font, "face");
		if (face && *face) {
			st.face = QString::fromUtf8(face);
		}
		int size = int(obs_data_get_int(font, "size"));
		if (size > 0) {
			st.fontSize = size;
		}
		st.bold = obs_data_get_int(font, "flags") & OBS_FONT_BOLD;
	}
	st.lifetimeSec = int(obs_data_get_int(s, "lifetime"));
	st.bgColor = uint32_t(obs_data_get_int(s, "bg_color"));
	st.textColor = uint32_t(obs_data_get_int(s, "text_color")) | 0xFF000000u;
	st.icons = obs_data_get_bool(s, "show_icons");
	st.names = obs_data_get_bool(s, "show_names");
	st.shadow = obs_data_get_bool(s, "shadow");
	st.samples = obs_data_get_bool(s, "samples");
	return st;
}

/* Names without a colour from the platform get a stable one */
QString NameColor(const ChatMessage &m)
{
	QColor c(m.user.color);
	if (c.isValid()) {
		/* Very dark names vanish on stream; lift them */
		if (c.lightnessF() < 0.35) {
			c = c.lighter(180);
		}
		return c.name();
	}
	static const char *palette[] = {"#ff7a7a", "#ffb86b", "#ffe36b", "#8cf27e", "#6be4ff",
					"#7aa2ff", "#c08bff", "#ff8bd8", "#5ef0c0", "#f5a3a3"};
	uint h = qHash(m.user.Key());
	return QString::fromLatin1(palette[h % (sizeof(palette) / sizeof(palette[0]))]);
}

QString Html(const ChatMessage &m, const Style &st, bool shadow)
{
	QString text = QColor(FromObs(st.textColor)).name();
	QString shadowColor = QStringLiteral("#000000");
	QString out;
	if (!m.highlight.isEmpty()) {
		out += QStringLiteral("<span style=\"color:%1; font-weight:700\">[%2]</span> ")
			       .arg(shadow ? shadowColor : QStringLiteral("#ffd24a"), m.highlight.toHtmlEscaped());
	}
	if (st.names) {
		QString name = m.user.displayName.isEmpty() ? m.user.login : m.user.displayName;
		out += QStringLiteral("<span style=\"color:%1; font-weight:700\">%2</span>")
			       .arg(shadow ? shadowColor : NameColor(m), name.toHtmlEscaped());
		out += QStringLiteral("<span style=\"color:%1\">: </span>").arg(shadow ? shadowColor : text);
	}
	out += QStringLiteral("<span style=\"color:%1\">%2</span>").arg(shadow ? shadowColor : text, m.text.toHtmlEscaped());
	return out;
}

QList<Entry> Samples()
{
	struct S {
		ChatPlatform p;
		const char *name;
		const char *color;
		const char *text;
	};
	static const S samples[] = {
		{ChatPlatform::Twitch, "Viewer_on_Twitch", "#9b8cff", "hello from Twitch!"},
		{ChatPlatform::Kick, "KickViewer", "#53fc18", "Kick chat says hi"},
		{ChatPlatform::YouTube, "YouTube Viewer", "", "watching on my phone, the vertical layout looks great"},
		{ChatPlatform::Rumble, "RumbleFan", "#85c742", "gg"},
	};
	QList<Entry> out;
	qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (const S &s : samples) {
		Entry e;
		e.msg.platform = s.p;
		e.msg.user.platform = s.p;
		e.msg.user.id = QString::fromLatin1(s.name);
		e.msg.user.displayName = QString::fromLatin1(s.name);
		e.msg.user.color = QString::fromLatin1(s.color);
		e.msg.text = QString::fromUtf8(s.text);
		e.arrived = now;
		out.push_back(e);
	}
	return out;
}

QImage Paint(const Style &st, const QList<Entry> &entries, qint64 now)
{
	QImage img(st.width, st.height, QImage::Format_RGBA8888);
	img.fill(FromObs(st.bgColor));

	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);

	QFont font(st.face);
	font.setPixelSize(st.fontSize);
	font.setBold(st.bold);

	const qreal pad = std::max(4.0, st.fontSize * 0.4);
	const qreal icon = st.icons ? st.fontSize * 1.05 : 0.0;
	const qreal gap = st.icons ? st.fontSize * 0.35 : 0.0;
	const qreal textX = pad + icon + gap;
	const qreal textW = std::max(20.0, st.width - textX - pad);
	const qreal shadowOff = std::max(1.0, st.fontSize / 14.0);
	const qreal lineH = QFontMetricsF(font).height();
	const qint64 lifetime = qint64(st.lifetimeSec) * 1000;

	qreal bottom = st.height - pad;
	for (auto it = entries.crbegin(); it != entries.crend(); ++it) {
		const Entry &e = *it;
		qint64 age = now - e.arrived;
		qreal opacity = 1.0;
		if (lifetime > 0) {
			if (age >= lifetime) {
				break; /* older ones are gone too */
			}
			if (age > lifetime - kFadeMs) {
				opacity = qreal(lifetime - age) / qreal(kFadeMs);
			}
		}

		QTextDocument doc;
		doc.setDefaultFont(font);
		doc.setDocumentMargin(0);
		doc.setTextWidth(textW);
		doc.setHtml(Html(e.msg, st, false));
		qreal h = std::max(doc.size().height(), icon);
		qreal top = bottom - h;
		if (top < 0 && bottom < st.height - pad) {
			break; /* no room; keep whole messages only */
		}

		p.setOpacity(opacity);
		if (st.icons) {
			QPixmap badge = ChatBadge(e.msg.platform, int(icon));
			p.drawPixmap(QRectF(pad, top + (lineH - icon) / 2.0, icon, icon), badge, QRectF(badge.rect()));
		}
		if (st.shadow) {
			QTextDocument sd;
			sd.setDefaultFont(font);
			sd.setDocumentMargin(0);
			sd.setTextWidth(textW);
			sd.setHtml(Html(e.msg, st, true));
			p.save();
			p.translate(textX + shadowOff, top + shadowOff);
			p.setOpacity(opacity * 0.85);
			sd.drawContents(&p);
			p.restore();
		}
		p.save();
		p.translate(textX, top);
		doc.drawContents(&p);
		p.restore();

		bottom = top - pad * 0.6;
		if (bottom <= 0) {
			break;
		}
	}
	p.end();
	return img;
}

/* ------------------------------------------------------------------------ */

class Feed : public QObject {
public:
	Feed()
	{
		tick.setInterval(200);
		connect(&tick, &QTimer::timeout, this, [this] { Tick(); });
		tick.start();
	}

	void Attach(ChatHub *h)
	{
		hub = h;
		connect(h, &ChatHub::MessageReceived, this, [this](const ChatMessage &m) {
			Entry e{m, QDateTime::currentMSecsSinceEpoch()};
			messages.push_back(e);
			while (messages.size() > kMaxMessages) {
				messages.pop_front();
			}
			QueueRepaint();
		});
	}

	void Add(Instance *inst)
	{
		std::lock_guard l(mtx);
		instances.push_back(inst);
	}

	void Remove(Instance *inst)
	{
		std::lock_guard l(mtx); /* waits for a running repaint */
		instances.erase(std::remove(instances.begin(), instances.end(), inst), instances.end());
	}

	/* Safe from any thread */
	void QueueRepaint()
	{
		if (queued.exchange(true)) {
			return;
		}
		QMetaObject::invokeMethod(
			this,
			[this] {
				queued = false;
				Repaint();
			},
			Qt::QueuedConnection);
	}

private:
	void Repaint()
	{
		qint64 now = QDateTime::currentMSecsSinceEpoch();
		std::lock_guard l(mtx);
		for (Instance *inst : instances) {
			Style st;
			{
				std::lock_guard il(inst->lock);
				st = inst->style;
			}
			const QList<Entry> &list = (messages.isEmpty() && st.samples) ? sampleList() : messages;
			QImage img = Paint(st, list, now);
			std::lock_guard il(inst->lock);
			inst->pending = std::move(img);
			inst->dirty = true;
		}
	}

	/* Fading messages need a few repaints; nothing else does */
	void Tick()
	{
		if (messages.isEmpty()) {
			return;
		}
		qint64 now = QDateTime::currentMSecsSinceEpoch();
		bool need = false;
		{
			std::lock_guard l(mtx);
			for (Instance *inst : instances) {
				qint64 life;
				{
					std::lock_guard il(inst->lock);
					life = qint64(inst->style.lifetimeSec) * 1000;
				}
				if (life <= 0) {
					continue;
				}
				for (const Entry &e : messages) {
					qint64 age = now - e.arrived;
					if (age > life - kFadeMs && age < life + 400) {
						need = true;
						break;
					}
				}
			}
		}
		if (need) {
			QueueRepaint();
		}
	}

	const QList<Entry> &sampleList()
	{
		if (samples.isEmpty()) {
			samples = Samples();
		}
		return samples;
	}

	QPointer<ChatHub> hub;
	std::mutex mtx;
	std::vector<Instance *> instances;
	QList<Entry> messages;
	QList<Entry> samples;
	QTimer tick;
	std::atomic<bool> queued{false};
};

Feed *g_feed = nullptr;
std::string g_name;

/* ------------------------------------------------------------------------ */
/* obs_source_info callbacks                                                */

const char *GetName(void *)
{
	return g_name.c_str();
}

void *Create(obs_data_t *settings, obs_source_t *source)
{
	auto *inst = new Instance();
	inst->source = source;
	inst->style = ReadStyle(settings);
	if (g_feed) {
		g_feed->Add(inst);
		g_feed->QueueRepaint();
	}
	return inst;
}

void Destroy(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	if (g_feed) {
		g_feed->Remove(inst);
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
	auto *inst = static_cast<Instance *>(data);
	{
		std::lock_guard l(inst->lock);
		inst->style = ReadStyle(settings);
	}
	if (g_feed) {
		g_feed->QueueRepaint();
	}
}

uint32_t Width(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	std::lock_guard l(inst->lock);
	return uint32_t(inst->style.width);
}

uint32_t Height(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	std::lock_guard l(inst->lock);
	return uint32_t(inst->style.height);
}

void Render(void *data, gs_effect_t *)
{
	auto *inst = static_cast<Instance *>(data);
	{
		std::lock_guard l(inst->lock);
		if (inst->dirty && !inst->pending.isNull()) {
			const QImage &img = inst->pending;
			uint32_t w = uint32_t(img.width()), h = uint32_t(img.height());
			if (!inst->tex || inst->texW != w || inst->texH != h) {
				if (inst->tex) {
					gs_texture_destroy(inst->tex);
				}
				const uint8_t *bits = img.constBits();
				inst->tex = gs_texture_create(w, h, GS_RGBA, 1, &bits, GS_DYNAMIC);
				inst->texW = w;
				inst->texH = h;
			} else {
				gs_texture_set_image(inst->tex, img.constBits(), uint32_t(img.bytesPerLine()), false);
			}
			inst->dirty = false;
		}
	}
	if (inst->tex) {
		obs_source_draw(inst->tex, 0, 0, 0, 0, false);
	}
}

void Defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "width", 420);
	obs_data_set_default_int(s, "height", 640);
	OBSDataAutoRelease font = obs_data_create();
	obs_data_set_string(font, "face", kDefaultFace);
	obs_data_set_int(font, "size", 24);
	obs_data_set_int(font, "flags", 0);
	obs_data_set_default_obj(s, "font", font);
	obs_data_set_default_int(s, "lifetime", 0);
	obs_data_set_default_int(s, "bg_color", 0x00000000);
	obs_data_set_default_int(s, "text_color", 0xFFFFFFFF);
	obs_data_set_default_bool(s, "show_icons", true);
	obs_data_set_default_bool(s, "show_names", true);
	obs_data_set_default_bool(s, "shadow", true);
	obs_data_set_default_bool(s, "samples", true);
}

obs_properties_t *Properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "width", Str("ChatSource.Width"), 50, 4096, 1);
	obs_properties_add_int(props, "height", Str("ChatSource.Height"), 50, 4096, 1);
	obs_properties_add_font(props, "font", Str("ChatSource.Font"));
	obs_property_t *life = obs_properties_add_int(props, "lifetime", Str("ChatSource.Lifetime"), 0, 3600, 5);
	obs_property_int_set_suffix(life, " s");
	obs_property_set_long_description(life, Str("ChatSource.LifetimeHint"));
	obs_properties_add_color(props, "text_color", Str("ChatSource.TextColor"));
	obs_properties_add_color_alpha(props, "bg_color", Str("ChatSource.Background"));
	obs_properties_add_bool(props, "show_icons", Str("ChatSource.Icons"));
	obs_properties_add_bool(props, "show_names", Str("ChatSource.Names"));
	obs_properties_add_bool(props, "shadow", Str("ChatSource.Shadow"));
	obs_property_t *samples = obs_properties_add_bool(props, "samples", Str("ChatSource.Samples"));
	obs_property_set_long_description(samples, Str("ChatSource.SamplesHint"));
	return props;
}

} // namespace

void ChatOverlay::Register()
{
	if (g_feed) {
		return;
	}
	g_feed = new Feed();
	g_name = QTStr("ChatSource.Name").toStdString();

	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = GetName;
	info.create = Create;
	info.destroy = Destroy;
	info.update = Update;
	info.get_width = Width;
	info.get_height = Height;
	info.video_render = Render;
	info.get_defaults = Defaults;
	info.get_properties = Properties;
	obs_register_source(&info);
}

void ChatOverlay::AttachHub(ChatHub *hub)
{
	if (g_feed && hub) {
		g_feed->Attach(hub);
	}
}
