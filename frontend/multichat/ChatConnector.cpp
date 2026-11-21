/******************************************************************************
    Multichat: shared connector plumbing and OAuth loopback login.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatConnector.hpp"

#include "multichat-config.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

#include "moc_ChatConnector.cpp"

const char *ChatPlatformKey(ChatPlatform p)
{
	switch (p) {
	case ChatPlatform::Twitch:
		return "twitch";
	case ChatPlatform::Kick:
		return "kick";
	case ChatPlatform::YouTube:
		return "youtube";
	case ChatPlatform::Rumble:
		return "rumble";
	}
	return "unknown";
}

QString ChatPlatformName(ChatPlatform p)
{
	switch (p) {
	case ChatPlatform::Twitch:
		return QStringLiteral("Twitch");
	case ChatPlatform::Kick:
		return QStringLiteral("Kick");
	case ChatPlatform::YouTube:
		return QStringLiteral("YouTube");
	case ChatPlatform::Rumble:
		return QStringLiteral("Rumble");
	}
	return {};
}

const char *ChatPlatformColor(ChatPlatform p)
{
	switch (p) {
	case ChatPlatform::Twitch:
		return "#9146ff";
	case ChatPlatform::Kick:
		return "#53fc18";
	case ChatPlatform::YouTube:
		return "#ff0033";
	case ChatPlatform::Rumble:
		return "#85c742";
	}
	return "#888888";
}

/* ------------------------------------------------------------------------ */

ChatConnector::ChatConnector(ChatPlatform platform_, QObject *parent)
	: QObject(parent),
	  platform(platform_),
	  net(new QNetworkAccessManager(this))
{
}

void ChatConnector::SetSendTarget(bool on)
{
	config["send"] = on;
	emit ConfigChanged();
	emit StateChanged(platform);
}

void ChatConnector::Restart()
{
	Stop();
	Start();
}

void ChatConnector::Logout()
{
	for (const char *k : {"access_token", "refresh_token", "expires_at", "user_id", "user_login", "user_name"}) {
		config.remove(k);
	}
	emit ConfigChanged();
	Restart();
}

static QString BuiltinClientId(ChatPlatform p)
{
	switch (p) {
	case ChatPlatform::Twitch:
		return QStringLiteral(MULTICHAT_TWITCH_CLIENT_ID);
	case ChatPlatform::Kick:
		return QStringLiteral(MULTICHAT_KICK_CLIENT_ID);
	case ChatPlatform::YouTube:
		return QStringLiteral(MULTICHAT_YOUTUBE_CLIENT_ID);
	default:
		return {};
	}
}

static QString BuiltinClientSecret(ChatPlatform p)
{
	switch (p) {
	case ChatPlatform::Kick:
		return QStringLiteral(MULTICHAT_KICK_CLIENT_SECRET);
	case ChatPlatform::YouTube:
		return QStringLiteral(MULTICHAT_YOUTUBE_CLIENT_SECRET);
	default:
		return {};
	}
}

QString ChatConnector::ClientId() const
{
	QString own = Str("client_id").trimmed();
	return own.isEmpty() ? BuiltinClientId(platform) : own;
}

QString ChatConnector::ClientSecret() const
{
	/* A custom app needs its own secret; never mix it with the built-in one */
	if (!Str("client_id").trimmed().isEmpty()) {
		return Str("client_secret").trimmed();
	}
	return BuiltinClientSecret(platform);
}

bool ChatConnector::HasBuiltinApp() const
{
	return !BuiltinClientId(platform).isEmpty();
}

void ChatConnector::SetState(ChatState s, const QString &text)
{
	state = s;
	stateText = text;
	emit StateChanged(platform);
}

QString ChatConnector::EnvOr(const char *env, const char *fallback)
{
	/* Development hook: lets the test suite point connectors at mock servers */
	QByteArray v = qgetenv(env);
	return v.isEmpty() ? QString::fromUtf8(fallback) : QString::fromUtf8(v);
}

void ChatConnector::Request(const QByteArray &verb, const QUrl &url,
			    const QList<std::pair<QByteArray, QByteArray>> &headers, const QByteArray &body,
			    const QByteArray &contentType, JsonCallback cb)
{
	QNetworkRequest req(url);
	for (const auto &h : headers) {
		req.setRawHeader(h.first, h.second);
	}
	if (!contentType.isEmpty()) {
		req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
	}
	req.setTransferTimeout(15000);

	QNetworkReply *reply = net->sendCustomRequest(req, verb, body);
	connect(reply, &QNetworkReply::finished, this, [reply, cb] {
		reply->deleteLater();
		int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		QByteArray raw = reply->readAll();
		QJsonObject obj = QJsonDocument::fromJson(raw).object();
		if (cb) {
			cb(status, obj, raw);
		}
	});
}

/* ------------------------------------------------------------------------ */
/* OAuth loopback                                                           */

static QString RandomUrlSafe(int bytes)
{
	QByteArray b(bytes, Qt::Uninitialized);
	for (int i = 0; i < bytes; i++) {
		b[i] = char(QRandomGenerator::system()->bounded(256));
	}
	return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

LoopbackOAuth::LoopbackOAuth(QObject *parent) : QObject(parent)
{
	state = RandomUrlSafe(16);
	verifier = RandomUrlSafe(48);
}

LoopbackOAuth::~LoopbackOAuth()
{
	if (server) {
		server->close();
	}
}

QString LoopbackOAuth::CodeChallenge() const
{
	QByteArray hash = QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256);
	return QString::fromLatin1(hash.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool LoopbackOAuth::Listen(const QString &host, quint16 port, const QString &path_)
{
	path = path_;
	server = new QTcpServer(this);
	if (!server->listen(QHostAddress::LocalHost, port)) {
		return false;
	}

	redirectUri = QStringLiteral("http://%1:%2%3").arg(host).arg(server->serverPort()).arg(path);

	connect(server, &QTcpServer::newConnection, this, [this] {
		while (QTcpSocket *sock = server->nextPendingConnection()) {
			connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
				QByteArray req = sock->readAll();
				QByteArray line = req.left(req.indexOf("\r\n"));
				QList<QByteArray> parts = line.split(' ');
				if (parts.size() < 2) {
					sock->close();
					return;
				}
				QUrl url(QStringLiteral("http://localhost") + QString::fromLatin1(parts[1]));
				if (url.path() != path) {
					sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
					sock->disconnectFromHost();
					return;
				}

				QUrlQuery q(url);
				QString code = q.queryItemValue("code");
				QString gotState = q.queryItemValue("state");
				QString error = q.queryItemValue("error");
				bool ok = !code.isEmpty() && gotState == state && error.isEmpty();

				QByteArray html =
					ok ? "<html><body style='font-family:sans-serif;background:#1e1e2e;color:#eee;"
					     "text-align:center;padding-top:15%'><h2>Logged in</h2>"
					     "<p>You can close this tab and go back to OBS.</p></body></html>"
					   : "<html><body style='font-family:sans-serif;text-align:center;"
					     "padding-top:15%'><h2>Login failed</h2><p>Go back to OBS and try "
					     "again.</p></body></html>";
				sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
					    "Connection: close\r\nContent-Length: " +
					    QByteArray::number(html.size()) + "\r\n\r\n" + html);
				sock->disconnectFromHost();
				server->close();

				if (ok) {
					emit CodeReceived(code);
				} else {
					emit Failed(error.isEmpty() ? QStringLiteral("state mismatch") : error);
				}
			});
			connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
		}
	});
	return true;
}

void LoopbackOAuth::Open(const QUrl &authorizeUrl)
{
	QDesktopServices::openUrl(authorizeUrl);
}
