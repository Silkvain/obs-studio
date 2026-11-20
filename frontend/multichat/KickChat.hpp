/******************************************************************************
    Multichat: Kick.
    Reads chat from the Pusher WebSocket kick.com's own web client uses
    (unofficial, no login). Sending and moderation use Kick's official
    public API with an OAuth 2.1 + PKCE login.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnector.hpp"

#include <QPointer>
#include <QTimer>

class ChatSocket;

class KickChat : public ChatConnector {
	Q_OBJECT

public:
	explicit KickChat(QObject *parent);

	static constexpr quint16 kRedirectPort = 47811;

	void Start() override;
	void Stop() override;
	bool LoggedIn() const override { return !Str("access_token").isEmpty(); }
	bool CanSend() const override { return LoggedIn() && !Str("broadcaster_user_id").isEmpty(); }
	QString LoginRequirement() const override;
	void Login(QWidget *parent) override;

	void SendChat(const QString &text) override;
	void Ban(const ChatUser &user, const QString &reason) override;
	void Timeout(const ChatUser &user, int seconds, const QString &reason) override;
	void Unban(const ChatUser &user) override;

private:
	QString Slug() const { return Str("channel").trimmed().toLower(); }
	void LookupChatroom();
	void ConnectPusher(const QString &chatroomId);
	void OnPusher(const QString &frame);
	void ResolveBroadcaster();
	void Api(const QByteArray &verb, const QString &path, const QJsonObject &body,
		 std::function<void(int, const QJsonObject &)> cb, bool retried = false);
	void RefreshToken(std::function<void(bool)> done);
	void ScheduleReconnect();

	QPointer<ChatSocket> ws;
	QTimer reconnectTimer;
	QTimer pingTimer;
	bool running = false;
	QString chatroomId;
};
