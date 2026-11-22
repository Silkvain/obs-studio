/******************************************************************************
    Multichat hub. See ChatHub.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatHub.hpp"
#include "KickChat.hpp"
#include "RumbleChat.hpp"
#include "TwitchChat.hpp"
#include "YouTubeChat.hpp"

#include <util/bmem.h>
#include <util/platform.h>

#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>

#include "moc_ChatHub.cpp"

namespace {
constexpr int kHistoryPerUser = 300;
} // namespace

ChatHub::ChatHub(QObject *parent) : QObject(parent)
{
	connectors[size_t(ChatPlatform::Twitch)] = new TwitchChat(this);
	connectors[size_t(ChatPlatform::Kick)] = new KickChat(this);
	connectors[size_t(ChatPlatform::YouTube)] = new YouTubeChat(this);
	connectors[size_t(ChatPlatform::Rumble)] = new RumbleChat(this);

	/* Tokens refresh in the background; coalesce writes */
	saveTimer.setSingleShot(true);
	saveTimer.setInterval(500);
	connect(&saveTimer, &QTimer::timeout, this, &ChatHub::Save);

	for (ChatConnector *c : connectors) {
		connect(c, &ChatConnector::MessageReceived, this, &ChatHub::OnMessage);
		connect(c, &ChatConnector::EventReceived, this, &ChatHub::OnEvent);
		connect(c, &ChatConnector::Notice, this, &ChatHub::Notice);
		connect(c, &ChatConnector::StateChanged, this, &ChatHub::StateChanged);
		connect(c, &ChatConnector::ConfigChanged, &saveTimer, qOverload<>(&QTimer::start));
	}

	char *path = os_get_config_path_ptr("obs-studio/multichat.json");
	configPath = QString::fromUtf8(path);
	bfree(path);
	Load();

	char *epath = os_get_config_path_ptr("obs-studio/siegfried/activity.json");
	eventsPath = QString::fromUtf8(epath);
	bfree(epath);
	eventsSaveTimer.setSingleShot(true);
	eventsSaveTimer.setInterval(2000);
	connect(&eventsSaveTimer, &QTimer::timeout, this, &ChatHub::SaveEvents);
	LoadEvents();
}

ChatHub::~ChatHub()
{
	if (saveTimer.isActive()) {
		Save();
	}
	if (eventsSaveTimer.isActive()) {
		SaveEvents();
	}
	StopAll();
}

void ChatHub::Load()
{
	QFile f(configPath);
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	for (ChatConnector *c : connectors) {
		c->Config() = root.value(QString::fromUtf8(ChatPlatformKey(c->Platform()))).toObject();
	}
}

void ChatHub::Save()
{
	QJsonObject root;
	root["version"] = 1;
	for (ChatConnector *c : connectors) {
		root[QString::fromUtf8(ChatPlatformKey(c->Platform()))] = c->Config();
	}

	QSaveFile f(configPath);
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
		f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); /* holds tokens */
		f.commit();
	}
}

void ChatHub::StartAll()
{
	for (ChatConnector *c : connectors) {
		c->Start();
	}
}

void ChatHub::StopAll()
{
	for (ChatConnector *c : connectors) {
		c->Stop();
	}
}

QList<ChatPlatform> ChatHub::SendTargets() const
{
	QList<ChatPlatform> out;
	for (ChatConnector *c : connectors) {
		if (c->Enabled() && c->SendTarget() && c->CanSend()) {
			out.append(c->Platform());
		}
	}
	return out;
}

QList<ChatPlatform> ChatHub::Send(const QString &text)
{
	QString t = text.trimmed();
	if (t.isEmpty()) {
		return {};
	}
	QList<ChatPlatform> targets = SendTargets();
	for (ChatPlatform p : targets) {
		Connector(p)->SendChat(t);
	}
	return targets;
}

void ChatHub::OnMessage(const ChatMessage &msg)
{
	QString key = msg.user.Key();
	QList<ChatMessage> &list = history[key];
	list.append(msg);
	if (list.size() > kHistoryPerUser) {
		list.removeFirst();
	}
	users.insert(key, msg.user);
	emit MessageReceived(msg);
}

const QList<ChatMessage> &ChatHub::History(const QString &userKey) const
{
	static const QList<ChatMessage> empty;
	auto it = history.constFind(userKey);
	return it == history.constEnd() ? empty : *it;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                   */

namespace {
constexpr int kMaxEvents = 500;

QJsonObject EventToJson(const StreamEvent &e)
{
	return QJsonObject{
		{"platform", QString::fromUtf8(ChatPlatformKey(e.platform))},
		{"type", QString::fromUtf8(StreamEventKey(e.type))},
		{"id", e.id},
		{"user_id", e.user.id},
		{"login", e.user.login},
		{"name", e.user.displayName},
		{"color", e.user.color},
		{"amount", e.amount},
		{"currency", e.currency},
		{"count", e.count},
		{"months", e.months},
		{"tier", e.tier},
		{"message", e.message},
		{"time", e.time.toMSecsSinceEpoch()},
	};
}

bool EventFromJson(const QJsonObject &o, StreamEvent &e)
{
	bool found = false;
	for (int i = 0; i < kChatPlatformCount; i++) {
		if (o.value("platform").toString() == QLatin1String(ChatPlatformKey(ChatPlatform(i)))) {
			e.platform = ChatPlatform(i);
			found = true;
		}
	}
	if (!found || !StreamEventFromKey(o.value("type").toString(), e.type)) {
		return false;
	}
	e.id = o.value("id").toString();
	e.user.platform = e.platform;
	e.user.id = o.value("user_id").toString();
	e.user.login = o.value("login").toString();
	e.user.displayName = o.value("name").toString();
	e.user.color = o.value("color").toString();
	e.amount = o.value("amount").toDouble();
	e.currency = o.value("currency").toString();
	e.count = o.value("count").toInt();
	e.months = o.value("months").toInt();
	e.tier = o.value("tier").toString();
	e.message = o.value("message").toString();
	e.time = QDateTime::fromMSecsSinceEpoch(qint64(o.value("time").toDouble()));
	return true;
}
} // namespace

void ChatHub::OnEvent(const StreamEvent &event)
{
	/* Platforms re-send on reconnect; never alert twice */
	if (!event.id.isEmpty()) {
		QString key = QString::fromUtf8(ChatPlatformKey(event.platform)) + ':' + event.id;
		if (seenEventIds.contains(key)) {
			return;
		}
		seenEventIds.insert(key);
	}
	StreamEvent e = event;
	if (e.id.isEmpty()) {
		/* Some platforms give no id; make one so it can be replayed */
		static quint64 counter = 0;
		e.id = QStringLiteral("local-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++counter);
	}
	events.append(e);
	while (events.size() > kMaxEvents) {
		events.removeFirst();
	}
	if (!event.test) {
		eventsSaveTimer.start();
	}
	emit EventReceived(e);
	if (!alertsPaused || e.test) {
		emit AlertRequested(e);
	}
}

void ChatHub::Test(StreamEventType type, ChatPlatform platform)
{
	static int n = 0;
	StreamEvent e;
	e.platform = platform;
	e.type = type;
	e.test = true;
	e.id = QStringLiteral("test-%1").arg(++n);
	e.user.platform = platform;
	e.user.id = QStringLiteral("test");
	e.user.login = QStringLiteral("test_viewer");
	e.user.displayName = QStringLiteral("TestViewer");
	switch (type) {
	case StreamEventType::Bits:
		e.amount = 500;
		e.message = QStringLiteral("Cheer500 great stream!");
		break;
	case StreamEventType::Donation:
	case StreamEventType::SuperChat:
	case StreamEventType::Rant:
		e.amount = 5;
		e.currency = QStringLiteral("USD");
		e.message = QStringLiteral("Keep it up!");
		break;
	case StreamEventType::GiftSub:
		e.count = 5;
		break;
	case StreamEventType::Raid:
	case StreamEventType::Host:
		e.count = 42;
		break;
	case StreamEventType::Resubscribe:
		e.months = 12;
		e.message = QStringLiteral("A whole year!");
		break;
	default:
		break;
	}
	OnEvent(e);
}

void ChatHub::SetAlertsPaused(bool paused)
{
	if (alertsPaused == paused) {
		return;
	}
	alertsPaused = paused;
	if (paused) {
		emit SkipAlertRequested(); /* silence now, not after this one */
	}
	emit AlertsPausedChanged(paused);
}

void ChatHub::LoadEvents()
{
	QFile f(eventsPath);
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}
	for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object().value("events").toArray()) {
		StreamEvent e;
		if (EventFromJson(v.toObject(), e)) {
			events.append(e);
			if (!e.id.isEmpty()) {
				seenEventIds.insert(QString::fromUtf8(ChatPlatformKey(e.platform)) + ':' + e.id);
			}
		}
	}
}

void ChatHub::SaveEvents()
{
	QJsonArray arr;
	for (const StreamEvent &e : events) {
		if (!e.test) {
			arr.append(EventToJson(e));
		}
	}
	QDir().mkpath(QFileInfo(eventsPath).absolutePath());
	QSaveFile f(eventsPath);
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(QJsonObject{{"version", 1}, {"events", arr}}).toJson(QJsonDocument::Compact));
		f.commit();
	}
}
