/******************************************************************************
    Multichat: one chat feed for every platform you stream to.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QWidget;

enum class ChatPlatform {
	Twitch,
	Kick,
	YouTube,
	Rumble,
};

constexpr int kChatPlatformCount = 4;

const char *ChatPlatformKey(ChatPlatform p);   /* "twitch", stable, used in config */
QString ChatPlatformName(ChatPlatform p);      /* "Twitch" */
const char *ChatPlatformColor(ChatPlatform p); /* badge colour */

struct ChatUser {
	ChatPlatform platform = ChatPlatform::Twitch;
	QString id;    /* platform user id (YouTube: channel id) */
	QString login; /* handle / slug, may equal displayName */
	QString displayName;
	QString color; /* #rrggbb or empty */

	QString Key() const { return QString::fromUtf8(ChatPlatformKey(platform)) + ':' + id; }
};

struct ChatMessage {
	ChatPlatform platform = ChatPlatform::Twitch;
	QString id;
	ChatUser user;
	QString text;
	QDateTime time = QDateTime::currentDateTime();
	bool isSelf = false;
	bool isModerator = false;
	bool isBroadcaster = false;
	QString highlight; /* e.g. "$5.00 Rant" */
};

enum class ChatState {
	Disabled,
	Connecting,
	Connected,
	Error,
};

/* One platform. Reading usually works without logging in; sending and
 * moderating always need the streamer's own login on that platform. */
class ChatConnector : public QObject {
	Q_OBJECT

public:
	ChatConnector(ChatPlatform platform, QObject *parent);

	ChatPlatform Platform() const { return platform; }

	/* Persisted settings + tokens for this platform */
	QJsonObject &Config() { return config; }
	bool Enabled() const { return config.value("enabled").toBool(false); }
	bool SendTarget() const { return config.value("send").toBool(true); }
	void SetSendTarget(bool on);

	ChatState State() const { return state; }
	QString StateText() const { return stateText; }

	virtual void Start() = 0;
	virtual void Stop() = 0;
	void Restart();

	virtual bool LoggedIn() const = 0;
	virtual bool CanSend() const { return LoggedIn() && state == ChatState::Connected; }
	virtual bool CanModerate() const { return LoggedIn(); }
	virtual QString LoginRequirement() const { return {}; } /* why login can't start, if anything */

	virtual void Login(QWidget *parent) = 0;
	virtual void Logout();

	virtual void SendChat(const QString &text) = 0;
	virtual void Ban(const ChatUser &user, const QString &reason) = 0;
	virtual void Timeout(const ChatUser &user, int seconds, const QString &reason) = 0;
	virtual void Unban(const ChatUser &user) = 0;

signals:
	void MessageReceived(const ChatMessage &msg);
	void Notice(ChatPlatform platform, const QString &text, bool error);
	void StateChanged(ChatPlatform platform);
	void ConfigChanged();

protected:
	void SetState(ChatState s, const QString &text = {});
	void EmitNotice(const QString &text, bool error = false) { emit Notice(platform, text, error); }
	QString Str(const char *key) const { return config.value(key).toString(); }
	/* The app registration to use: the user's own (Advanced) if set,
	 * otherwise the one built into this build. */
	QString ClientId() const;
	QString ClientSecret() const;

public:
	bool HasBuiltinApp() const;

protected:
	/* Small helper for JSON REST calls. The callback gets the HTTP status
	 * (0 on network failure) and the parsed body. */
	using JsonCallback = std::function<void(int status, const QJsonObject &body, const QByteArray &raw)>;
	void Request(const QByteArray &verb, const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers,
		     const QByteArray &body, const QByteArray &contentType, JsonCallback cb);

	static QString EnvOr(const char *env, const char *fallback);

	ChatPlatform platform;
	QJsonObject config;
	ChatState state = ChatState::Disabled;
	QString stateText;
	QNetworkAccessManager *net;
};

/* Opens the system browser for an OAuth 2 authorization-code + PKCE login
 * and catches the redirect on a localhost port. */
class LoopbackOAuth : public QObject {
	Q_OBJECT

public:
	LoopbackOAuth(QObject *parent);
	~LoopbackOAuth() override;

	/* host: "localhost" or "127.0.0.1"; port 0 = any free port */
	bool Listen(const QString &host, quint16 port, const QString &path);
	QString RedirectUri() const { return redirectUri; }
	QString State() const { return state; }
	QString CodeVerifier() const { return verifier; }
	QString CodeChallenge() const;

	void Open(const QUrl &authorizeUrl);

signals:
	void CodeReceived(const QString &code);
	void Failed(const QString &error);

private:
	class QTcpServer *server = nullptr;
	QString redirectUri;
	QString path;
	QString state;
	QString verifier;
};
