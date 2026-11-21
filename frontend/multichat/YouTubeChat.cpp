/******************************************************************************
    Multichat: YouTube. See YouTubeChat.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "YouTubeChat.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "moc_YouTubeChat.cpp"

namespace {
constexpr const char *kScope = "https://www.googleapis.com/auth/youtube.force-ssl";
constexpr int kMinPollMs = 3000;

QString ApiBase()
{
	return qEnvironmentVariableIsSet("MULTICHAT_YOUTUBE_API")
		       ? qEnvironmentVariable("MULTICHAT_YOUTUBE_API")
		       : QStringLiteral("https://www.googleapis.com/youtube/v3");
}

QByteArray Form(const QList<std::pair<QString, QString>> &items)
{
	QUrlQuery q;
	for (const auto &i : items) {
		q.addQueryItem(i.first, QString::fromLatin1(QUrl::toPercentEncoding(i.second)));
	}
	return q.query(QUrl::FullyEncoded).toUtf8();
}
} // namespace

YouTubeChat::YouTubeChat(QObject *parent) : ChatConnector(ChatPlatform::YouTube, parent)
{
	pollTimer.setSingleShot(true);
	connect(&pollTimer, &QTimer::timeout, this, &YouTubeChat::Poll);
	findTimer.setSingleShot(true);
	connect(&findTimer, &QTimer::timeout, this, &YouTubeChat::FindBroadcast);
}

QString YouTubeChat::LoginRequirement() const
{
	if (ClientId().isEmpty() || ClientSecret().isEmpty()) {
		return tr("YouTube login isn't built into this build yet. Add a Google OAuth client under Advanced.");
	}
	return {};
}

void YouTubeChat::Start()
{
	Stop();
	if (!Enabled()) {
		SetState(ChatState::Disabled);
		return;
	}
	if (!LoggedIn()) {
		SetState(ChatState::Error, tr("Log in to YouTube to read chat"));
		return;
	}
	running = true;
	FindBroadcast();
}

void YouTubeChat::Stop()
{
	running = false;
	pollTimer.stop();
	findTimer.stop();
	liveChatId.clear();
	pageToken.clear();
	if (state != ChatState::Disabled) {
		SetState(ChatState::Disabled);
	}
}

void YouTubeChat::FindBroadcast()
{
	if (!running) {
		return;
	}
	SetState(ChatState::Connecting, tr("Looking for your live broadcast…"));
	Api("GET", "/liveBroadcasts?part=snippet&broadcastStatus=active&broadcastType=all", {},
	    [this](int status, const QJsonObject &obj) {
		    if (!running) {
			    return;
		    }
		    QString chatId;
		    for (const QJsonValue &v : obj.value("items").toArray()) {
			    chatId = v.toObject().value("snippet").toObject().value("liveChatId").toString();
			    if (!chatId.isEmpty()) {
				    break;
			    }
		    }
		    if (status != 200 || chatId.isEmpty()) {
			    SetState(ChatState::Connecting,
				     status == 200 ? tr("Waiting for you to go live on YouTube…")
						   : tr("YouTube error (HTTP %1), retrying…").arg(status));
			    findTimer.start(30000);
			    return;
		    }
		    liveChatId = chatId;
		    pageToken.clear();
		    SetState(ChatState::Connected, tr("Connected to your live chat"));
		    Poll();
	    });
}

void YouTubeChat::Poll()
{
	if (!running || liveChatId.isEmpty()) {
		return;
	}
	QString q = QStringLiteral("/liveChat/messages?part=snippet,authorDetails&maxResults=200&liveChatId=") +
		    QString::fromLatin1(QUrl::toPercentEncoding(liveChatId));
	if (!pageToken.isEmpty()) {
		q += "&pageToken=" + QString::fromLatin1(QUrl::toPercentEncoding(pageToken));
	}

	Api("GET", q, {}, [this](int status, const QJsonObject &obj) {
		if (!running) {
			return;
		}
		if (status == 403 || status == 404) {
			/* chat ended or broadcast over: go back to waiting */
			liveChatId.clear();
			emit StateChanged(platform);
			findTimer.start(30000);
			SetState(ChatState::Connecting, tr("Broadcast ended, waiting for the next one…"));
			return;
		}
		if (status != 200) {
			pollTimer.start(10000);
			return;
		}

		bool first = pageToken.isEmpty();
		for (const QJsonValue &v : obj.value("items").toArray()) {
			QJsonObject item = v.toObject();
			QJsonObject sn = item.value("snippet").toObject();
			QJsonObject author = item.value("authorDetails").toObject();
			QString type = sn.value("type").toString();
			if (type != "textMessageEvent" && type != "superChatEvent" && type != "superStickerEvent") {
				continue;
			}

			ChatMessage msg;
			msg.platform = ChatPlatform::YouTube;
			msg.id = item.value("id").toString();
			msg.user.platform = ChatPlatform::YouTube;
			msg.user.id = author.value("channelId").toString();
			msg.user.displayName = author.value("displayName").toString();
			msg.user.login = msg.user.displayName;
			msg.isModerator = author.value("isChatModerator").toBool();
			msg.isBroadcaster = author.value("isChatOwner").toBool();
			msg.isSelf = msg.isBroadcaster;
			msg.text = sn.value("displayMessage").toString();
			QDateTime t = QDateTime::fromString(sn.value("publishedAt").toString(), Qt::ISODate);
			if (t.isValid()) {
				msg.time = t.toLocalTime();
			}
			if (type == "superChatEvent") {
				msg.highlight =
					sn.value("superChatDetails").toObject().value("amountDisplayString").toString() +
					tr(" Super Chat");
			}
			emit MessageReceived(msg);
		}
		Q_UNUSED(first);

		pageToken = obj.value("nextPageToken").toString();
		pollTimer.start(std::max(kMinPollMs, obj.value("pollingIntervalMillis").toInt()));
	});
}

void YouTubeChat::Api(const QByteArray &verb, const QString &pathAndQuery, const QJsonObject &body,
		      std::function<void(int, const QJsonObject &)> cb, bool retried)
{
	auto send = [this, verb, pathAndQuery, body, cb, retried] {
		QList<std::pair<QByteArray, QByteArray>> headers{
			{"Authorization", "Bearer " + Str("access_token").toUtf8()}};
		QByteArray data = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
		Request(verb, QUrl(ApiBase() + pathAndQuery), headers, data,
			body.isEmpty() ? QByteArray() : "application/json",
			[this, verb, pathAndQuery, body, cb, retried](int status, const QJsonObject &obj,
								      const QByteArray &) {
				if (status == 401 && !retried) {
					RefreshToken([this, verb, pathAndQuery, body, cb](bool ok) {
						if (ok) {
							Api(verb, pathAndQuery, body, cb, true);
						} else {
							cb(401, {});
						}
					});
					return;
				}
				cb(status, obj);
			});
	};

	/* Google access tokens last an hour: refresh proactively */
	if (Str("access_token").isEmpty() ||
	    config.value("expires_at").toVariant().toLongLong() < QDateTime::currentSecsSinceEpoch() + 60) {
		RefreshToken([send, cb](bool ok) {
			if (ok) {
				send();
			} else {
				cb(401, {});
			}
		});
	} else {
		send();
	}
}

void YouTubeChat::SendChat(const QString &text)
{
	if (!CanSend()) {
		return;
	}
	QJsonObject body{
		{"snippet", QJsonObject{{"liveChatId", liveChatId},
					{"type", "textMessageEvent"},
					{"textMessageDetails", QJsonObject{{"messageText", text.left(200)}}}}}};
	/* No local echo: our message comes back on the next poll */
	Api("POST", "/liveChat/messages?part=snippet", body, [this](int status, const QJsonObject &obj) {
		if (status != 200) {
			EmitNotice(tr("YouTube didn't accept the message: %1")
					   .arg(obj.value("error").toObject().value("message").toString(
						   tr("HTTP %1").arg(status))),
				   true);
		}
	});
}

void YouTubeChat::DoBan(const ChatUser &user, int seconds)
{
	if (!CanModerate()) {
		EmitNotice(tr("YouTube moderation needs a login and an active broadcast."), true);
		return;
	}
	QJsonObject sn{{"liveChatId", liveChatId},
		       {"type", seconds > 0 ? "temporary" : "permanent"},
		       {"bannedUserDetails", QJsonObject{{"channelId", user.id}}}};
	if (seconds > 0) {
		sn["banDurationSeconds"] = seconds;
	}
	Api("POST", "/liveChat/bans?part=snippet", QJsonObject{{"snippet", sn}},
	    [this, user, seconds](int status, const QJsonObject &obj) {
		    if (status == 200) {
			    banIds.insert(user.id, obj.value("id").toString());
			    EmitNotice(seconds > 0
					       ? tr("Timed out %1 for %2 min").arg(user.displayName).arg(seconds / 60)
					       : tr("Banned %1").arg(user.displayName));
		    } else {
			    EmitNotice(tr("YouTube refused: %1")
					       .arg(obj.value("error").toObject().value("message").toString(
						       tr("HTTP %1").arg(status))),
				       true);
		    }
	    });
}

void YouTubeChat::Ban(const ChatUser &user, const QString &)
{
	DoBan(user, 0);
}

void YouTubeChat::Timeout(const ChatUser &user, int seconds, const QString &)
{
	DoBan(user, std::max(1, seconds));
}

void YouTubeChat::Unban(const ChatUser &user)
{
	QString banId = banIds.value(user.id);
	if (banId.isEmpty()) {
		EmitNotice(tr("YouTube can only undo bans made in this session. Use YouTube Studio for older ones."),
			   true);
		return;
	}
	Api("DELETE", "/liveChat/bans?id=" + QString::fromLatin1(QUrl::toPercentEncoding(banId)), {},
	    [this, user](int status, const QJsonObject &) {
		    if (status == 204 || status == 200) {
			    banIds.remove(user.id);
			    EmitNotice(tr("Unbanned %1").arg(user.displayName));
		    } else {
			    EmitNotice(tr("YouTube refused the unban (HTTP %1)").arg(status), true);
		    }
	    });
}

/* ------------------------------------------------------------------------ */
/* Login: Google installed-app flow, loopback redirect + PKCE               */

void YouTubeChat::Login(QWidget *)
{
	if (!LoginRequirement().isEmpty()) {
		EmitNotice(LoginRequirement(), true);
		return;
	}
	LoopbackOAuth *oauth = new LoopbackOAuth(this);
	if (!oauth->Listen("127.0.0.1", 0, "/")) {
		EmitNotice(tr("Couldn't open a local port for the login."), true);
		oauth->deleteLater();
		return;
	}

	QUrl url("https://accounts.google.com/o/oauth2/v2/auth");
	QUrlQuery q;
	q.addQueryItem("response_type", "code");
	q.addQueryItem("client_id", ClientId());
	q.addQueryItem("redirect_uri", oauth->RedirectUri());
	q.addQueryItem("scope", kScope);
	q.addQueryItem("state", oauth->State());
	q.addQueryItem("code_challenge", oauth->CodeChallenge());
	q.addQueryItem("code_challenge_method", "S256");
	q.addQueryItem("access_type", "offline");
	q.addQueryItem("prompt", "consent");
	url.setQuery(q);

	connect(oauth, &LoopbackOAuth::Failed, this, [this, oauth](const QString &err) {
		EmitNotice(tr("YouTube login failed: %1").arg(err), true);
		oauth->deleteLater();
	});
	connect(oauth, &LoopbackOAuth::CodeReceived, this, [this, oauth](const QString &code) {
		QByteArray body = Form({{"grant_type", "authorization_code"},
					{"client_id", ClientId()},
					{"client_secret", ClientSecret()},
					{"redirect_uri", oauth->RedirectUri()},
					{"code_verifier", oauth->CodeVerifier()},
					{"code", code}});
		oauth->deleteLater();
		Request("POST", QUrl("https://oauth2.googleapis.com/token"), {}, body,
			"application/x-www-form-urlencoded",
			[this](int status, const QJsonObject &obj, const QByteArray &raw) {
				if (status != 200) {
					EmitNotice(tr("YouTube login failed: %1").arg(QString::fromUtf8(raw.left(200))),
						   true);
					return;
				}
				config["access_token"] = obj.value("access_token").toString();
				config["refresh_token"] = obj.value("refresh_token").toString();
				config["expires_at"] =
					QDateTime::currentSecsSinceEpoch() + obj.value("expires_in").toInt();
				config["enabled"] = true;
				emit ConfigChanged();
				EmitNotice(tr("Logged in to YouTube"));
				Start();
			});
	});
	oauth->Open(url);
}

void YouTubeChat::RefreshToken(std::function<void(bool)> done)
{
	QString refresh = Str("refresh_token");
	if (refresh.isEmpty()) {
		done(!Str("access_token").isEmpty());
		return;
	}
	QByteArray body = Form({{"grant_type", "refresh_token"},
				{"client_id", ClientId()},
				{"client_secret", ClientSecret()},
				{"refresh_token", refresh}});
	Request("POST", QUrl("https://oauth2.googleapis.com/token"), {}, body, "application/x-www-form-urlencoded",
		[this, done](int status, const QJsonObject &obj, const QByteArray &) {
			if (status != 200) {
				done(false);
				return;
			}
			config["access_token"] = obj.value("access_token").toString();
			config["expires_at"] = QDateTime::currentSecsSinceEpoch() + obj.value("expires_in").toInt();
			emit ConfigChanged();
			done(true);
		});
}
