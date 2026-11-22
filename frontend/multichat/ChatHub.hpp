/******************************************************************************
    Multichat hub: owns every platform connector, merges their chat into
    one stream, keeps per-user history for the user card, and fans your
    outgoing messages out to every platform you have toggled on.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnector.hpp"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <array>

class ChatHub : public QObject {
	Q_OBJECT

public:
	explicit ChatHub(QObject *parent);
	~ChatHub() override;

	ChatConnector *Connector(ChatPlatform p) const { return connectors[size_t(p)]; }
	const std::array<ChatConnector *, kChatPlatformCount> &Connectors() const { return connectors; }

	void StartAll();
	void StopAll();

	/* Sends to every enabled platform with "send" toggled on that can send.
	 * Returns the platforms it went to. */
	QList<ChatPlatform> Send(const QString &text);
	QList<ChatPlatform> SendTargets() const;

	const QList<ChatMessage> &History(const QString &userKey) const;
	int MessageCount(const QString &userKey) const { return int(History(userKey).size()); }
	ChatUser KnownUser(const QString &userKey) const { return users.value(userKey); }

	void Save();

	/* Events (follows, subs, raids...): newest last */
	const QList<StreamEvent> &Events() const { return events; }
	/* Fake event, as if it happened live (test buttons) */
	void Test(StreamEventType type, ChatPlatform platform = ChatPlatform::Twitch);
	/* Play an event's alert again */
	void Replay(const StreamEvent &event) { emit AlertRequested(event); }

	/* Silence alerts (events still show in Activity) */
	bool AlertsPaused() const { return alertsPaused; }
	void SetAlertsPaused(bool paused);
	void SkipAlert() { emit SkipAlertRequested(); }

signals:
	void MessageReceived(const ChatMessage &msg);
	void EventReceived(const StreamEvent &event);
	void AlertRequested(const StreamEvent &event);
	void SkipAlertRequested();
	void AlertsPausedChanged(bool paused);
	void Notice(ChatPlatform platform, const QString &text, bool error);
	void StateChanged(ChatPlatform platform);

private:
	void Load();
	void OnMessage(const ChatMessage &msg);
	void OnEvent(const StreamEvent &event);
	void LoadEvents();
	void SaveEvents();

	std::array<ChatConnector *, kChatPlatformCount> connectors{};
	QHash<QString, QList<ChatMessage>> history;
	QHash<QString, ChatUser> users;
	QTimer saveTimer;
	QString configPath;
	QString eventsPath;
	QList<StreamEvent> events;
	QSet<QString> seenEventIds;
	QTimer eventsSaveTimer;
	bool alertsPaused = false;
};
