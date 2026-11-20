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
		connect(c, &ChatConnector::Notice, this, &ChatHub::Notice);
		connect(c, &ChatConnector::StateChanged, this, &ChatHub::StateChanged);
		connect(c, &ChatConnector::ConfigChanged, &saveTimer, qOverload<>(&QTimer::start));
	}

	char *path = os_get_config_path_ptr("obs-studio/multichat.json");
	configPath = QString::fromUtf8(path);
	bfree(path);
	Load();
}

ChatHub::~ChatHub()
{
	if (saveTimer.isActive()) {
		Save();
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
