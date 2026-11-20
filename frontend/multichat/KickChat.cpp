/******************************************************************************
    Multichat: Kick. See KickChat.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "KickChat.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrlQuery>
#include "ChatSocket.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "moc_KickChat.cpp"

namespace {
constexpr const char *kScopes = "user:read channel:read chat:write moderation:ban";
/* Kick's public API takes timeout durations in minutes */
constexpr int kKickDurationDivisor = 60;

QString PusherUrl()
{
	return qEnvironmentVariableIsSet("MULTICHAT_KICK_PUSHER")
		       ? qEnvironmentVariable("MULTICHAT_KICK_PUSHER")
		       : QStringLiteral(
				 "wss://ws-us2.pusher.com/app/32cbd69e4b950bf97679?protocol=7&client=js&version=8.4.0&flash=false");
}
QString ApiBase()
{
	return qEnvironmentVariableIsSet("MULTICHAT_KICK_API") ? qEnvironmentVariable("MULTICHAT_KICK_API")
							       : QStringLiteral("https://api.kick.com/public/v1");
}
QString IdBase()
{
	return qEnvironmentVariableIsSet("MULTICHAT_KICK_ID") ? qEnvironmentVariable("MULTICHAT_KICK_ID")
							      : QStringLiteral("https://id.kick.com");
}
QString SiteBase()
{
	return qEnvironmentVariableIsSet("MULTICHAT_KICK_SITE") ? qEnvironmentVariable("MULTICHAT_KICK_SITE")
								: QStringLiteral("https://kick.com");
}

QByteArray Form(const QList<QPair<QString, QString>> &items)
{
	QUrlQuery q;
	for (const auto &i : items) {
		q.addQueryItem(i.first, QString::fromLatin1(QUrl::toPercentEncoding(i.second)));
	}
	return q.query(QUrl::FullyEncoded).toUtf8();
}

QString StripEmotes(QString text)
{
	/* [emote:37226:KEKW] -> KEKW */
	static const QRegularExpression re(QStringLiteral("\\[emote:\\d+:([^\\]]+)\\]"));
	return text.replace(re, QStringLiteral("\\1"));
}
} // namespace

KickChat::KickChat(QObject *parent) : ChatConnector(ChatPlatform::Kick, parent)
{
	reconnectTimer.setSingleShot(true);
	connect(&reconnectTimer, &QTimer::timeout, this, &KickChat::Start);

	pingTimer.setInterval(60000);
	connect(&pingTimer, &QTimer::timeout, this, [this] {
		if (ws) {
			ws->sendTextMessage(QStringLiteral("{\"event\":\"pusher:ping\",\"data\":{}}"));
		}
	});
}

QString KickChat::LoginRequirement() const
{
	if (ClientId().isEmpty()) {
		return tr("This build has no Kick app built in. Add a Client ID under Advanced.");
	}
	return {};
}

/* ------------------------------------------------------------------------ */
/* Reading                                                                  */

void KickChat::Start()
{
	Stop();
	if (!Enabled() || Slug().isEmpty()) {
		SetState(ChatState::Disabled, Enabled() ? tr("Set a channel name") : QString());
		return;
	}
	running = true;
	SetState(ChatState::Connecting, tr("Connecting…"));

	if (LoggedIn()) {
		ResolveBroadcaster();
	}

	QString manual = Str("chatroom_id").trimmed();
	if (manual.isEmpty()) {
		manual = Str("chatroom_id_cached");
	}
	if (!manual.isEmpty()) {
		ConnectPusher(manual);
	} else {
		LookupChatroom();
	}
}

void KickChat::Stop()
{
	running = false;
	reconnectTimer.stop();
	pingTimer.stop();
	if (ws) {
		ws->disconnect(this);
		ws->close();
		ws->deleteLater();
		ws = nullptr;
	}
	if (state != ChatState::Disabled) {
		SetState(ChatState::Disabled);
	}
}

void KickChat::ScheduleReconnect()
{
	if (running && !reconnectTimer.isActive()) {
		reconnectTimer.start(5000);
	}
}

void KickChat::LookupChatroom()
{
	/* Kick's website API sits behind Cloudflare and may refuse non-browser
	 * clients. If it does, the user can paste the chatroom ID once. */
	QList<QPair<QByteArray, QByteArray>> headers{
		{"Accept", "application/json"},
		{"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
			       "Chrome/130.0 Safari/537.36"},
	};
	Request("GET", QUrl(SiteBase() + "/api/v2/channels/" + Slug()), headers, {}, {},
		[this](int status, const QJsonObject &obj, const QByteArray &) {
			if (!running) {
				return;
			}
			QString id =
				QString::number(obj.value("chatroom").toObject().value("id").toVariant().toLongLong());
			if (status != 200 || id == "0") {
				SetState(ChatState::Error,
					 tr("Couldn't find the chatroom automatically. Paste the Chatroom ID in "
					    "Chat Accounts."));
				return;
			}
			config["chatroom_id_cached"] = id;
			if (config.value("broadcaster_user_id").toString().isEmpty()) {
				qint64 uid = obj.value("user_id").toVariant().toLongLong();
				if (uid) {
					config["broadcaster_user_id"] = QString::number(uid);
				}
			}
			emit ConfigChanged();
			ConnectPusher(id);
		});
}

void KickChat::ConnectPusher(const QString &id)
{
	chatroomId = id;
	ws = new ChatSocket(this);
	connect(ws, &ChatSocket::textMessageReceived, this, &KickChat::OnPusher);
	connect(ws, &ChatSocket::disconnected, this, [this] {
		pingTimer.stop();
		if (!running) {
			return;
		}
		SetState(ChatState::Connecting, tr("Disconnected, reconnecting…"));
		ScheduleReconnect();
	});
	ws->open(QUrl(PusherUrl()));
}

void KickChat::OnPusher(const QString &frame)
{
	QJsonObject env = QJsonDocument::fromJson(frame.toUtf8()).object();
	QString event = env.value("event").toString();
	/* Pusher double-encodes: data is a JSON string */
	QJsonValue rawData = env.value("data");
	QJsonObject data = rawData.isString() ? QJsonDocument::fromJson(rawData.toString().toUtf8()).object()
					      : rawData.toObject();

	if (event == "pusher:connection_established") {
		QJsonObject sub{{"event", "pusher:subscribe"},
				{"data", QJsonObject{{"auth", ""},
						     {"channel", QStringLiteral("chatrooms.%1.v2").arg(chatroomId)}}}};
		ws->sendTextMessage(QString::fromUtf8(QJsonDocument(sub).toJson(QJsonDocument::Compact)));
		pingTimer.start();
	} else if (event == "pusher_internal:subscription_succeeded") {
		SetState(ChatState::Connected, LoggedIn() ? tr("Connected to %1").arg(Slug())
							  : tr("Reading %1 (log in to send)").arg(Slug()));
	} else if (event == "pusher:ping") {
		ws->sendTextMessage(QStringLiteral("{\"event\":\"pusher:pong\",\"data\":{}}"));
	} else if (event == "pusher:error") {
		SetState(ChatState::Error, data.value("message").toString(tr("Chat server error")));
	} else if (event == "App\\Events\\ChatMessageEvent") {
		QJsonObject sender = data.value("sender").toObject();
		QJsonObject identity = sender.value("identity").toObject();

		ChatMessage msg;
		msg.platform = ChatPlatform::Kick;
		msg.id = data.value("id").toString();
		msg.user.platform = ChatPlatform::Kick;
		msg.user.id = QString::number(sender.value("id").toVariant().toLongLong());
		msg.user.login = sender.value("slug").toString();
		msg.user.displayName = sender.value("username").toString();
		msg.user.color = identity.value("color").toString();
		for (const QJsonValue &b : identity.value("badges").toArray()) {
			QString type = b.toObject().value("type").toString();
			msg.isBroadcaster |= type == "broadcaster";
			msg.isModerator |= type == "moderator";
		}
		msg.text = StripEmotes(data.value("content").toString());
		QDateTime t = QDateTime::fromString(data.value("created_at").toString(), Qt::ISODate);
		if (t.isValid()) {
			msg.time = t.toLocalTime();
		}
		msg.isSelf = !Str("user_id").isEmpty() && msg.user.id == Str("user_id");
		emit MessageReceived(msg);
	} else if (event == "App\\Events\\UserBannedEvent") {
		QString who = data.value("user").toObject().value("username").toString();
		bool permanent = data.value("permanent").toBool();
		EmitNotice(permanent ? tr("%1 was banned").arg(who) : tr("%1 was timed out").arg(who));
	}
}

/* ------------------------------------------------------------------------ */
/* Official API                                                             */

void KickChat::Api(const QByteArray &verb, const QString &path, const QJsonObject &body,
		   std::function<void(int, const QJsonObject &)> cb, bool retried)
{
	QList<QPair<QByteArray, QByteArray>> headers{
		{"Authorization", "Bearer " + Str("access_token").toUtf8()},
		{"Accept", "application/json"},
	};
	QByteArray data = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
	Request(verb, QUrl(ApiBase() + path), headers, data, body.isEmpty() ? QByteArray() : "application/json",
		[this, verb, path, body, cb, retried](int status, const QJsonObject &obj, const QByteArray &) {
			if (status == 401 && !retried) {
				RefreshToken([this, verb, path, body, cb](bool ok) {
					if (ok) {
						Api(verb, path, body, cb, true);
					} else {
						cb(401, {});
					}
				});
				return;
			}
			cb(status, obj);
		});
}

void KickChat::ResolveBroadcaster()
{
	/* Whose chat are we posting into / moderating? */
	if (Slug() == Str("user_login").toLower() && !Str("user_id").isEmpty()) {
		config["broadcaster_user_id"] = Str("user_id");
		emit ConfigChanged();
		return;
	}
	Api("GET", "/channels?slug=" + QString::fromLatin1(QUrl::toPercentEncoding(Slug())), {},
	    [this](int status, const QJsonObject &obj) {
		    QJsonObject ch = obj.value("data").toArray().first().toObject();
		    qint64 id = ch.value("broadcaster_user_id").toVariant().toLongLong();
		    if (status == 200 && id) {
			    config["broadcaster_user_id"] = QString::number(id);
			    emit ConfigChanged();
			    emit StateChanged(platform);
		    }
	    });
}

void KickChat::SendChat(const QString &text)
{
	if (!CanSend()) {
		return;
	}
	QJsonObject body{{"broadcaster_user_id", Str("broadcaster_user_id").toLongLong()},
			 {"content", text.left(500)},
			 {"type", "user"}};
	/* No local echo: Kick broadcasts our own message back over Pusher */
	Api("POST", "/chat", body, [this](int status, const QJsonObject &obj) {
		if (status != 200) {
			EmitNotice(tr("Kick didn't accept the message: %1")
					   .arg(obj.value("message").toString(tr("HTTP %1").arg(status))),
				   true);
		}
	});
}

void KickChat::Ban(const ChatUser &user, const QString &reason)
{
	Timeout(user, 0, reason);
}

void KickChat::Timeout(const ChatUser &user, int seconds, const QString &reason)
{
	if (!LoggedIn() || Str("broadcaster_user_id").isEmpty()) {
		EmitNotice(tr("Log in to Kick to moderate."), true);
		return;
	}
	QJsonObject body{{"broadcaster_user_id", Str("broadcaster_user_id").toLongLong()},
			 {"user_id", user.id.toLongLong()}};
	if (seconds > 0) {
		body["duration"] = std::max(1, seconds / kKickDurationDivisor);
	}
	if (!reason.isEmpty()) {
		body["reason"] = reason.left(100);
	}

	Api("POST", "/moderation/bans", body, [this, user, seconds](int status, const QJsonObject &obj) {
		if (status == 200 || status == 201 || status == 204) {
			EmitNotice(seconds > 0 ? tr("Timed out %1 for %2 min").arg(user.displayName).arg(seconds / 60)
					       : tr("Banned %1").arg(user.displayName));
		} else {
			EmitNotice(tr("Kick refused: %1").arg(obj.value("message").toString(tr("HTTP %1").arg(status))),
				   true);
		}
	});
}

void KickChat::Unban(const ChatUser &user)
{
	if (!LoggedIn()) {
		return;
	}
	QJsonObject body{{"broadcaster_user_id", Str("broadcaster_user_id").toLongLong()},
			 {"user_id", user.id.toLongLong()}};
	Api("DELETE", "/moderation/bans", body, [this, user](int status, const QJsonObject &obj) {
		if (status == 200 || status == 204) {
			EmitNotice(tr("Unbanned %1").arg(user.displayName));
		} else {
			EmitNotice(tr("Kick refused: %1").arg(obj.value("message").toString(tr("HTTP %1").arg(status))),
				   true);
		}
	});
}

/* ------------------------------------------------------------------------ */
/* Login: OAuth 2.1 authorization code + PKCE, loopback redirect            */

void KickChat::Login(QWidget *)
{
	if (!LoginRequirement().isEmpty()) {
		EmitNotice(LoginRequirement(), true);
		return;
	}

	LoopbackOAuth *oauth = new LoopbackOAuth(this);
	if (!oauth->Listen("localhost", kRedirectPort, "/callback")) {
		EmitNotice(tr("Port %1 is busy; close whatever is using it and try again.").arg(kRedirectPort), true);
		oauth->deleteLater();
		return;
	}

	QUrl url(IdBase() + "/oauth/authorize");
	QUrlQuery q;
	q.addQueryItem("response_type", "code");
	q.addQueryItem("client_id", ClientId());
	q.addQueryItem("redirect_uri", oauth->RedirectUri());
	q.addQueryItem("scope", kScopes);
	q.addQueryItem("state", oauth->State());
	q.addQueryItem("code_challenge", oauth->CodeChallenge());
	q.addQueryItem("code_challenge_method", "S256");
	url.setQuery(q);

	connect(oauth, &LoopbackOAuth::Failed, this, [this, oauth](const QString &err) {
		EmitNotice(tr("Kick login failed: %1").arg(err), true);
		oauth->deleteLater();
	});
	connect(oauth, &LoopbackOAuth::CodeReceived, this, [this, oauth](const QString &code) {
		QList<QPair<QString, QString>> fields{{"grant_type", "authorization_code"},
						      {"client_id", ClientId()},
						      {"redirect_uri", oauth->RedirectUri()},
						      {"code_verifier", oauth->CodeVerifier()},
						      {"code", code}};
		if (!ClientSecret().isEmpty()) {
			fields.append({"client_secret", ClientSecret()});
		}
		QByteArray body = Form(fields);
		oauth->deleteLater();
		Request("POST", QUrl(IdBase() + "/oauth/token"), {}, body, "application/x-www-form-urlencoded",
			[this](int status, const QJsonObject &obj, const QByteArray &raw) {
				if (status != 200) {
					EmitNotice(tr("Kick login failed: %1").arg(QString::fromUtf8(raw.left(200))),
						   true);
					return;
				}
				config["access_token"] = obj.value("access_token").toString();
				config["refresh_token"] = obj.value("refresh_token").toString();
				config["expires_at"] =
					QDateTime::currentSecsSinceEpoch() + obj.value("expires_in").toInt();

				Api("GET", "/users", {}, [this](int st, const QJsonObject &me) {
					QJsonObject u = me.value("data").toArray().first().toObject();
					if (st == 200) {
						config["user_id"] =
							QString::number(u.value("user_id").toVariant().toLongLong());
						config["user_name"] = u.value("name").toString();
						config["user_login"] = u.value("name").toString().toLower();
						if (Slug().isEmpty()) {
							config["channel"] = config["user_login"];
						}
						config["enabled"] = true;
					}
					emit ConfigChanged();
					EmitNotice(tr("Logged in to Kick as %1").arg(Str("user_name")));
					Start();
				});
			});
	});

	oauth->Open(url);
}

void KickChat::RefreshToken(std::function<void(bool)> done)
{
	QString refresh = Str("refresh_token");
	if (refresh.isEmpty()) {
		done(false);
		return;
	}
	QList<QPair<QString, QString>> fields{{"grant_type", "refresh_token"},
					      {"client_id", ClientId()},
					      {"refresh_token", refresh}};
	if (!ClientSecret().isEmpty()) {
		fields.append({"client_secret", ClientSecret()});
	}
	QByteArray body = Form(fields);
	Request("POST", QUrl(IdBase() + "/oauth/token"), {}, body, "application/x-www-form-urlencoded",
		[this, done](int status, const QJsonObject &obj, const QByteArray &) {
			if (status != 200) {
				done(false);
				return;
			}
			config["access_token"] = obj.value("access_token").toString();
			/* Kick rotates refresh tokens: always keep the newest */
			config["refresh_token"] = obj.value("refresh_token").toString();
			config["expires_at"] = QDateTime::currentSecsSinceEpoch() + obj.value("expires_in").toInt();
			emit ConfigChanged();
			done(true);
		});
}
