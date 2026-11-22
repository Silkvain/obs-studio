/******************************************************************************
    Multichat: Twitch.
    Reads chat over Twitch's IRC WebSocket (anonymous until you log in).
    Login uses the OAuth Device Code flow, so no redirect URL is needed.
    Sending goes over IRC; ban/timeout/unban use the Helix API.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnector.hpp"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimer>

class ChatSocket;
class QDialog;

class TwitchChat : public ChatConnector {
	Q_OBJECT

public:
	explicit TwitchChat(QObject *parent);

	void Start() override;
	void Stop() override;
	bool LoggedIn() const override { return !Str("access_token").isEmpty(); }
	QString LoginRequirement() const override;
	void Login(QWidget *parent) override;

	void SendChat(const QString &text) override;
	void Ban(const ChatUser &user, const QString &reason) override;
	void Timeout(const ChatUser &user, int seconds, const QString &reason) override;
	void Unban(const ChatUser &user) override;

private:
	void OnConnected();
	void OnText(const QString &frame);
	void HandleLine(const QString &line);
	void HandleUserNotice(const QHash<QString, QString> &tags, const QString &text);
	void StartEventSub(const QString &url);
	void StopEventSub();
	void OnEventSub(const QString &frame);
	void ScheduleReconnect();
	void RefreshToken(std::function<void(bool)> done);
	void Helix(const QByteArray &verb, const QString &path, const QJsonObject &body,
		   std::function<void(int, const QJsonObject &)> cb, bool retried = false);
	void WithBroadcasterId(std::function<void(const QString &)> cb);
	void DoBan(const ChatUser &user, int seconds, const QString &reason);
	void PollDeviceToken(const QString &deviceCode, int interval);

	QString Channel() const { return Str("channel").trimmed().toLower().remove('#'); }

	QPointer<ChatSocket> ws;
	QPointer<ChatSocket> eventWs;
	QSet<QString> giftGroups;
	QTimer reconnectTimer;
	QTimer devicePollTimer;
	QPointer<QDialog> deviceDialog;
	bool running = false;
	bool anonymous = true;
	QString broadcasterId;
	QString selfColor;
	QString selfDisplayName;
};
