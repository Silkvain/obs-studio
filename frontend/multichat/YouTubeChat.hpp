/******************************************************************************
    Multichat: YouTube.
    Official YouTube Data API v3: finds your active broadcast, polls its
    live chat, sends messages and bans/timeouts. Needs a Google login for
    everything (YouTube has no anonymous chat API). Uses API quota.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnector.hpp"

#include <QHash>
#include <QTimer>

class YouTubeChat : public ChatConnector {
	Q_OBJECT

public:
	explicit YouTubeChat(QObject *parent);

	void Start() override;
	void Stop() override;
	bool LoggedIn() const override { return !Str("refresh_token").isEmpty() || !Str("access_token").isEmpty(); }
	bool CanSend() const override { return LoggedIn() && !liveChatId.isEmpty(); }
	bool CanModerate() const override { return LoggedIn() && !liveChatId.isEmpty(); }
	QString LoginRequirement() const override;
	void Login(QWidget *parent) override;

	void SendChat(const QString &text) override;
	void Ban(const ChatUser &user, const QString &reason) override;
	void Timeout(const ChatUser &user, int seconds, const QString &reason) override;
	void Unban(const ChatUser &user) override;

private:
	void FindBroadcast();
	void Poll();
	void Api(const QByteArray &verb, const QString &pathAndQuery, const QJsonObject &body,
		 std::function<void(int, const QJsonObject &)> cb, bool retried = false);
	void RefreshToken(std::function<void(bool)> done);
	void DoBan(const ChatUser &user, int seconds);

	QTimer pollTimer;
	QTimer findTimer;
	bool running = false;
	QString liveChatId;
	QString pageToken;
	QHash<QString, QString> banIds; /* channelId -> ban resource id, for unban */
};
