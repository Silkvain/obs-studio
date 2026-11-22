/******************************************************************************
    Multichat: Twitch. See TwitchChat.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "TwitchChat.hpp"

#include <QDesktopServices>
#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QUrlQuery>
#include <QVBoxLayout>
#include "ChatSocket.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

#include "moc_TwitchChat.cpp"

namespace {
/* moderator:read:followers: follow alerts (EventSub). Subs, gifts, raids
 * and bits arrive over chat and need nothing extra. */
constexpr const char *kScopes = "chat:read chat:edit moderator:manage:banned_users moderator:read:followers";
constexpr const char *kFollowScope = "moderator:read:followers";

QString EventSubUrl()
{
	return qEnvironmentVariableIsSet("MULTICHAT_TWITCH_EVENTSUB") ? qEnvironmentVariable("MULTICHAT_TWITCH_EVENTSUB")
								      : QStringLiteral("wss://eventsub.wss.twitch.tv/ws");
}

QString TierFromPlan(const QString &plan)
{
	if (plan == QLatin1String("Prime")) {
		return plan;
	}
	bool ok = false;
	int n = plan.toInt(&ok);
	return ok ? QString::number(std::max(1, n / 1000)) : QString();
}

QString IrcUrl()
{
	return qEnvironmentVariableIsSet("MULTICHAT_TWITCH_IRC") ? qEnvironmentVariable("MULTICHAT_TWITCH_IRC")
								 : QStringLiteral("wss://irc-ws.chat.twitch.tv:443");
}
QString HelixBase()
{
	return qEnvironmentVariableIsSet("MULTICHAT_TWITCH_HELIX") ? qEnvironmentVariable("MULTICHAT_TWITCH_HELIX")
								   : QStringLiteral("https://api.twitch.tv/helix");
}
QString IdBase()
{
	return qEnvironmentVariableIsSet("MULTICHAT_TWITCH_ID") ? qEnvironmentVariable("MULTICHAT_TWITCH_ID")
								: QStringLiteral("https://id.twitch.tv");
}

QString UnescapeTag(const QString &v)
{
	QString out;
	out.reserve(v.size());
	for (int i = 0; i < v.size(); i++) {
		if (v[i] == '\\' && i + 1 < v.size()) {
			QChar n = v[++i];
			out += n == ':'   ? QChar(';')
			       : n == 's' ? QChar(' ')
			       : n == 'r' ? QChar('\r')
			       : n == 'n' ? QChar('\n')
					  : n;
		} else {
			out += v[i];
		}
	}
	return out;
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

TwitchChat::TwitchChat(QObject *parent) : ChatConnector(ChatPlatform::Twitch, parent)
{
	reconnectTimer.setSingleShot(true);
	connect(&reconnectTimer, &QTimer::timeout, this, &TwitchChat::Start);
}

QString TwitchChat::LoginRequirement() const
{
	return ClientId().isEmpty() ? tr("This build has no Twitch app built in. Add a Client ID under Advanced.")
				    : QString();
}

/* ------------------------------------------------------------------------ */
/* Connection                                                               */

void TwitchChat::Start()
{
	Stop();
	if (!Enabled() || Channel().isEmpty()) {
		SetState(ChatState::Disabled, Enabled() ? tr("Set a channel name") : QString());
		return;
	}

	running = true;
	SetState(ChatState::Connecting, tr("Connecting…"));

	ws = new ChatSocket(this);
	connect(ws, &ChatSocket::connected, this, &TwitchChat::OnConnected);
	connect(ws, &ChatSocket::textMessageReceived, this, &TwitchChat::OnText);
	connect(ws, &ChatSocket::disconnected, this, [this] {
		if (!running) {
			return;
		}
		SetState(ChatState::Connecting, tr("Disconnected, reconnecting…"));
		ScheduleReconnect();
	});
	ws->open(QUrl(IrcUrl()));
	StartEventSub(EventSubUrl());
}

void TwitchChat::Stop()
{
	running = false;
	reconnectTimer.stop();
	StopEventSub();
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

void TwitchChat::ScheduleReconnect()
{
	if (running && !reconnectTimer.isActive()) {
		reconnectTimer.start(5000);
	}
}

void TwitchChat::OnConnected()
{
	anonymous = !LoggedIn();
	ws->sendTextMessage(QStringLiteral("CAP REQ :twitch.tv/tags twitch.tv/commands"));
	if (anonymous) {
		ws->sendTextMessage(QStringLiteral("PASS SCHMOOPIIE"));
		ws->sendTextMessage(
			QStringLiteral("NICK justinfan%1").arg(QRandomGenerator::global()->bounded(10000, 99999)));
	} else {
		ws->sendTextMessage(QStringLiteral("PASS oauth:") + Str("access_token"));
		ws->sendTextMessage(QStringLiteral("NICK ") + Str("user_login"));
	}
	ws->sendTextMessage(QStringLiteral("JOIN #") + Channel());
}

void TwitchChat::OnText(const QString &frame)
{
	for (const QString &line : frame.split(QStringLiteral("\r\n"), Qt::SkipEmptyParts)) {
		HandleLine(line);
	}
}

void TwitchChat::HandleLine(const QString &raw)
{
	QString line = raw;
	QHash<QString, QString> tags;

	if (line.startsWith('@')) {
		qsizetype sp = line.indexOf(' ');
		for (const QString &kv : line.mid(1, sp - 1).split(';')) {
			qsizetype eq = kv.indexOf('=');
			tags.insert(kv.left(eq), eq >= 0 ? UnescapeTag(kv.mid(eq + 1)) : QString());
		}
		line = line.mid(sp + 1);
	}

	QString prefix;
	if (line.startsWith(':')) {
		qsizetype sp = line.indexOf(' ');
		prefix = line.mid(1, sp - 1);
		line = line.mid(sp + 1);
	}

	QString trailing;
	qsizetype colon = line.indexOf(QStringLiteral(" :"));
	if (colon >= 0) {
		trailing = line.mid(colon + 2);
		line = line.left(colon);
	}
	QStringList params = line.split(' ', Qt::SkipEmptyParts);
	if (params.isEmpty()) {
		return;
	}
	QString cmd = params.takeFirst();

	if (cmd == "PING") {
		ws->sendTextMessage(QStringLiteral("PONG :") + trailing);
	} else if (cmd == "001") {
		/* welcome */
	} else if (cmd == "JOIN") {
		SetState(ChatState::Connected, anonymous ? tr("Reading #%1 (log in to send)").arg(Channel())
							 : tr("Connected to #%1").arg(Channel()));
	} else if (cmd == "NOTICE" && trailing.contains("Login authentication failed")) {
		/* Expired token: refresh once, otherwise fall back to read-only */
		RefreshToken([this](bool ok) {
			if (!ok) {
				EmitNotice(tr("Twitch login expired. Log in again to send messages."), true);
				config.remove("access_token");
				emit ConfigChanged();
			}
			Start();
		});
	} else if (cmd == "USERSTATE" || cmd == "GLOBALUSERSTATE") {
		selfColor = tags.value("color");
		selfDisplayName = tags.value("display-name");
	} else if (cmd == "PRIVMSG") {
		ChatMessage msg;
		msg.platform = ChatPlatform::Twitch;
		msg.id = tags.value("id");
		msg.user.platform = ChatPlatform::Twitch;
		msg.user.id = tags.value("user-id");
		msg.user.login = prefix.section('!', 0, 0);
		msg.user.displayName = tags.value("display-name", msg.user.login);
		if (msg.user.displayName.isEmpty()) {
			msg.user.displayName = msg.user.login;
		}
		msg.user.color = tags.value("color");
		QString badges = tags.value("badges");
		msg.isBroadcaster = badges.contains("broadcaster/");
		msg.isModerator = badges.contains("moderator/");

		QString text = trailing;
		if (text.startsWith("\001ACTION ") && text.endsWith('\001')) {
			text = text.mid(8, text.size() - 9);
		}
		msg.text = text;

		bool okTs = false;
		qint64 ts = tags.value("tmi-sent-ts").toLongLong(&okTs);
		if (okTs) {
			msg.time = QDateTime::fromMSecsSinceEpoch(ts);
		}
		if (tags.contains("bits")) {
			msg.highlight = tr("%1 bits").arg(tags.value("bits"));
			StreamEvent ev;
			ev.platform = ChatPlatform::Twitch;
			ev.type = StreamEventType::Bits;
			ev.id = msg.id;
			ev.user = msg.user;
			ev.amount = tags.value("bits").toDouble();
			ev.message = msg.text;
			ev.time = msg.time;
			emit EventReceived(ev);
		}

		emit MessageReceived(msg);
	} else if (cmd == "USERNOTICE") {
		HandleUserNotice(tags, trailing);
	} else if (cmd == "CLEARCHAT" && !trailing.isEmpty()) {
		QString dur = tags.value("ban-duration");
		EmitNotice(dur.isEmpty() ? tr("%1 was banned").arg(trailing)
					 : tr("%1 was timed out for %2s").arg(trailing, dur));
	} else if (cmd == "RECONNECT") {
		Start();
	}
}

/* Subs, resubs, gifted subs and raids arrive as USERNOTICE in chat */
void TwitchChat::HandleUserNotice(const QHash<QString, QString> &tags, const QString &text)
{
	QString kind = tags.value("msg-id");
	StreamEvent ev;
	ev.platform = ChatPlatform::Twitch;
	ev.id = tags.value("id");
	ev.user.platform = ChatPlatform::Twitch;
	ev.user.id = tags.value("user-id");
	ev.user.login = tags.value("login");
	ev.user.displayName = tags.value("display-name", ev.user.login);
	ev.user.color = tags.value("color");
	ev.message = text;
	ev.tier = TierFromPlan(tags.value("msg-param-sub-plan"));
	bool okTs = false;
	qint64 ts = tags.value("tmi-sent-ts").toLongLong(&okTs);
	if (okTs) {
		ev.time = QDateTime::fromMSecsSinceEpoch(ts);
	}

	if (kind == "sub") {
		ev.type = StreamEventType::Subscribe;
	} else if (kind == "resub") {
		ev.type = StreamEventType::Resubscribe;
		ev.months = tags.value("msg-param-cumulative-months").toInt();
	} else if (kind == "submysterygift") {
		/* "X is gifting 5 subs": one alert, not five */
		ev.type = StreamEventType::GiftSub;
		ev.count = std::max(1, tags.value("msg-param-mass-gift-count").toInt());
		QString group = tags.value("msg-param-community-gift-id");
		if (!group.isEmpty()) {
			giftGroups.insert(group);
		}
	} else if (kind == "subgift") {
		QString group = tags.value("msg-param-community-gift-id");
		if (!group.isEmpty() && giftGroups.contains(group)) {
			return; /* part of a mystery gift already announced */
		}
		ev.type = StreamEventType::GiftSub;
		ev.count = 1;
		ev.message = tr("to %1").arg(tags.value("msg-param-recipient-display-name"));
	} else if (kind == "raid") {
		ev.type = StreamEventType::Raid;
		ev.count = tags.value("msg-param-viewerCount").toInt();
		QString name = tags.value("msg-param-displayName");
		if (!name.isEmpty()) {
			ev.user.displayName = name;
		}
	} else if (kind == "giftpaidupgrade" || kind == "anongiftpaidupgrade" || kind == "primepaidupgrade") {
		ev.type = StreamEventType::Subscribe;
	} else {
		return;
	}
	if (ev.user.displayName.isEmpty()) {
		ev.user.displayName = ev.user.login.isEmpty() ? tr("Anonymous") : ev.user.login;
	}
	emit EventReceived(ev);
}

/* ------------------------------------------------------------------------ */
/* Follows: EventSub over WebSocket (needs moderator:read:followers)        */

void TwitchChat::StartEventSub(const QString &url)
{
	StopEventSub();
	if (!LoggedIn()) {
		return; /* follows need the streamer's login */
	}
	eventWs = new ChatSocket(this);
	connect(eventWs, &ChatSocket::textMessageReceived, this, &TwitchChat::OnEventSub);
	connect(eventWs, &ChatSocket::disconnected, this, [this] {
		if (running) {
			QTimer::singleShot(10000, this, [this] {
				if (running && !eventWs) {
					StartEventSub(EventSubUrl());
				}
			});
		}
		if (eventWs) {
			eventWs->deleteLater();
			eventWs = nullptr;
		}
	});
	eventWs->open(QUrl(url));
}

void TwitchChat::StopEventSub()
{
	if (eventWs) {
		eventWs->disconnect(this);
		eventWs->close();
		eventWs->deleteLater();
		eventWs = nullptr;
	}
}

void TwitchChat::OnEventSub(const QString &frame)
{
	QJsonObject root = QJsonDocument::fromJson(frame.toUtf8()).object();
	QJsonObject meta = root.value("metadata").toObject();
	QJsonObject payload = root.value("payload").toObject();
	QString type = meta.value("message_type").toString();

	if (type == "session_welcome") {
		QString session = payload.value("session").toObject().value("id").toString();
		WithBroadcasterId([this, session](const QString &bid) {
			QJsonObject body{
				{"type", "channel.follow"},
				{"version", "2"},
				{"condition", QJsonObject{{"broadcaster_user_id", bid}, {"moderator_user_id", Str("user_id")}}},
				{"transport", QJsonObject{{"method", "websocket"}, {"session_id", session}}},
			};
			Helix("POST", QStringLiteral("/eventsub/subscriptions"), body, [this](int status, const QJsonObject &) {
				if (status == 401 || status == 403) {
					EmitNotice(tr("Follow alerts need one more Twitch permission (%1): click Connect for "
						      "Twitch again.")
							   .arg(QString::fromLatin1(kFollowScope)),
						   true);
				} else if (status >= 300) {
					EmitNotice(tr("Couldn't turn on Twitch follow alerts (error %1)").arg(status), true);
				}
			});
		});
	} else if (type == "session_reconnect") {
		QString next = payload.value("session").toObject().value("reconnect_url").toString();
		if (!next.isEmpty()) {
			StartEventSub(next);
		}
	} else if (type == "notification") {
		QString subType = payload.value("subscription").toObject().value("type").toString();
		QJsonObject e = payload.value("event").toObject();
		if (subType == "channel.follow") {
			StreamEvent ev;
			ev.platform = ChatPlatform::Twitch;
			ev.type = StreamEventType::Follow;
			ev.id = meta.value("message_id").toString();
			ev.user.platform = ChatPlatform::Twitch;
			ev.user.id = e.value("user_id").toString();
			ev.user.login = e.value("user_login").toString();
			ev.user.displayName = e.value("user_name").toString(ev.user.login);
			QDateTime t = QDateTime::fromString(e.value("followed_at").toString(), Qt::ISODateWithMs);
			if (t.isValid()) {
				ev.time = t.toLocalTime();
			}
			emit EventReceived(ev);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Sending                                                                  */

void TwitchChat::SendChat(const QString &text)
{
	if (!CanSend() || anonymous) {
		return;
	}
	QString clean = text;
	clean.replace('\n', ' ').replace('\r', ' ');
	ws->sendTextMessage(QStringLiteral("PRIVMSG #%1 :%2").arg(Channel(), clean));

	/* Twitch doesn't echo your own messages back over IRC */
	ChatMessage msg;
	msg.platform = ChatPlatform::Twitch;
	msg.user.platform = ChatPlatform::Twitch;
	msg.user.id = Str("user_id");
	msg.user.login = Str("user_login");
	msg.user.displayName = selfDisplayName.isEmpty() ? Str("user_name") : selfDisplayName;
	msg.user.color = selfColor;
	msg.text = clean;
	msg.isSelf = true;
	msg.isBroadcaster = msg.user.login == Channel();
	emit MessageReceived(msg);
}

/* ------------------------------------------------------------------------ */
/* Helix (moderation)                                                       */

void TwitchChat::Helix(const QByteArray &verb, const QString &path, const QJsonObject &body,
		       std::function<void(int, const QJsonObject &)> cb, bool retried)
{
	QList<std::pair<QByteArray, QByteArray>> headers{
		{"Client-Id", ClientId().toUtf8()},
		{"Authorization", "Bearer " + Str("access_token").toUtf8()},
	};
	QByteArray data = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
	Request(verb, QUrl(HelixBase() + path), headers, data, body.isEmpty() ? QByteArray() : "application/json",
		[this, verb, path, body, cb, retried](int status, const QJsonObject &obj, const QByteArray &) {
			if (status == 401 && !retried) {
				RefreshToken([this, verb, path, body, cb](bool ok) {
					if (ok) {
						Helix(verb, path, body, cb, true);
					} else {
						cb(401, {});
					}
				});
				return;
			}
			cb(status, obj);
		});
}

void TwitchChat::WithBroadcasterId(std::function<void(const QString &)> cb)
{
	if (!broadcasterId.isEmpty()) {
		cb(broadcasterId);
		return;
	}
	if (Channel() == Str("user_login") && !Str("user_id").isEmpty()) {
		broadcasterId = Str("user_id");
		cb(broadcasterId);
		return;
	}
	Helix("GET", QStringLiteral("/users?login=") + Channel(), {}, [this, cb](int status, const QJsonObject &obj) {
		QJsonArray data = obj.value("data").toArray();
		if (status != 200 || data.isEmpty()) {
			EmitNotice(tr("Couldn't look up Twitch channel #%1").arg(Channel()), true);
			return;
		}
		broadcasterId = data.first().toObject().value("id").toString();
		cb(broadcasterId);
	});
}

void TwitchChat::DoBan(const ChatUser &user, int seconds, const QString &reason)
{
	if (!LoggedIn()) {
		EmitNotice(tr("Log in to Twitch to moderate."), true);
		return;
	}
	WithBroadcasterId([this, user, seconds, reason](const QString &bid) {
		QJsonObject data{{"user_id", user.id}};
		if (seconds > 0) {
			data["duration"] = seconds;
		}
		if (!reason.isEmpty()) {
			data["reason"] = reason;
		}
		QString path =
			QStringLiteral("/moderation/bans?broadcaster_id=%1&moderator_id=%2").arg(bid, Str("user_id"));
		Helix("POST", path, QJsonObject{{"data", data}},
		      [this, user, seconds](int status, const QJsonObject &obj) {
			      if (status == 200) {
				      EmitNotice(seconds > 0 ? tr("Timed out %1 for %2")
								       .arg(user.displayName)
								       .arg(seconds >= 3600
										    ? tr("%1 h").arg(seconds / 3600)
										    : tr("%1 min").arg(seconds / 60))
							     : tr("Banned %1").arg(user.displayName));
			      } else {
				      EmitNotice(tr("Twitch refused: %1")
							 .arg(obj.value("message").toString(tr("HTTP %1").arg(status))),
						 true);
			      }
		      });
	});
}

void TwitchChat::Ban(const ChatUser &user, const QString &reason)
{
	DoBan(user, 0, reason);
}

void TwitchChat::Timeout(const ChatUser &user, int seconds, const QString &reason)
{
	DoBan(user, std::max(1, seconds), reason);
}

void TwitchChat::Unban(const ChatUser &user)
{
	if (!LoggedIn()) {
		return;
	}
	WithBroadcasterId([this, user](const QString &bid) {
		QString path = QStringLiteral("/moderation/bans?broadcaster_id=%1&moderator_id=%2&user_id=%3")
				       .arg(bid, Str("user_id"), user.id);
		Helix("DELETE", path, {}, [this, user](int status, const QJsonObject &obj) {
			if (status == 204 || status == 200) {
				EmitNotice(tr("Unbanned %1").arg(user.displayName));
			} else {
				EmitNotice(tr("Twitch refused: %1")
						   .arg(obj.value("message").toString(tr("HTTP %1").arg(status))),
					   true);
			}
		});
	});
}

/* ------------------------------------------------------------------------ */
/* Login: OAuth Device Code flow                                            */

void TwitchChat::Login(QWidget *parent)
{
	QString clientId = ClientId();
	if (clientId.isEmpty()) {
		EmitNotice(LoginRequirement(), true);
		return;
	}

	QByteArray body = Form({{"client_id", clientId}, {"scopes", kScopes}});
	Request("POST", QUrl(IdBase() + "/oauth2/device"), {}, body, "application/x-www-form-urlencoded",
		[this, parent](int status, const QJsonObject &obj, const QByteArray &) {
			if (status != 200) {
				EmitNotice(tr("Twitch login failed: %1")
						   .arg(obj.value("message").toString(tr("HTTP %1").arg(status))),
					   true);
				return;
			}
			QString userCode = obj.value("user_code").toString();
			QString verifyUrl = obj.value("verification_uri").toString();

			QDialog *dlg = new QDialog(parent);
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			dlg->setWindowTitle(tr("Connect Twitch"));
			QVBoxLayout *l = new QVBoxLayout(dlg);
			l->addWidget(new QLabel(tr(
				"Your browser is opening Twitch. Click <b>Authorize</b> there.<br>If Twitch asks for a code, enter:")));
			QLabel *code = new QLabel(userCode);
			QFont f = code->font();
			f.setPointSize(f.pointSize() * 2);
			f.setBold(true);
			f.setLetterSpacing(QFont::AbsoluteSpacing, 4);
			code->setFont(f);
			code->setAlignment(Qt::AlignCenter);
			code->setTextInteractionFlags(Qt::TextSelectableByMouse);
			l->addWidget(code);
			QPushButton *open = new QPushButton(tr("Open browser again"));
			connect(open, &QPushButton::clicked, dlg,
				[verifyUrl] { QDesktopServices::openUrl(QUrl(verifyUrl)); });
			l->addWidget(open);
			QLabel *waiting = new QLabel(tr("Waiting for you to approve…"));
			waiting->setAlignment(Qt::AlignCenter);
			l->addWidget(waiting);
			connect(dlg, &QDialog::finished, this, [this] { devicePollTimer.stop(); });
			deviceDialog = dlg;
			dlg->show();

			QDesktopServices::openUrl(QUrl(verifyUrl));
			PollDeviceToken(obj.value("device_code").toString(),
					std::max(1, obj.value("interval").toInt(5)));
		});
}

void TwitchChat::PollDeviceToken(const QString &deviceCode, int interval)
{
	devicePollTimer.disconnect();
	devicePollTimer.setInterval(interval * 1000);
	connect(&devicePollTimer, &QTimer::timeout, this, [this, deviceCode] {
		QByteArray body = Form({{"client_id", ClientId()},
					{"scopes", kScopes},
					{"device_code", deviceCode},
					{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});
		Request("POST", QUrl(IdBase() + "/oauth2/token"), {}, body, "application/x-www-form-urlencoded",
			[this](int status, const QJsonObject &obj, const QByteArray &) {
				if (status == 400 && obj.value("message").toString() == "authorization_pending") {
					return;
				}
				devicePollTimer.stop();
				if (status != 200) {
					if (deviceDialog) {
						deviceDialog->close();
					}
					EmitNotice(
						tr("Twitch login failed: %1")
							.arg(obj.value("message").toString(tr("HTTP %1").arg(status))),
						true);
					return;
				}
				config["access_token"] = obj.value("access_token").toString();
				config["refresh_token"] = obj.value("refresh_token").toString();
				config["expires_at"] =
					QDateTime::currentSecsSinceEpoch() + obj.value("expires_in").toInt();

				Helix("GET", "/users", {}, [this](int st, const QJsonObject &me) {
					QJsonObject u = me.value("data").toArray().first().toObject();
					if (st == 200) {
						config["user_id"] = u.value("id").toString();
						config["user_login"] = u.value("login").toString();
						config["user_name"] = u.value("display_name").toString();
						if (Channel().isEmpty()) {
							config["channel"] = u.value("login").toString();
						}
						config["enabled"] = true;
					}
					broadcasterId.clear();
					emit ConfigChanged();
					if (deviceDialog) {
						deviceDialog->close();
					}
					EmitNotice(tr("Logged in to Twitch as %1").arg(Str("user_name")));
					Start();
				});
			});
	});
	devicePollTimer.start();
}

void TwitchChat::RefreshToken(std::function<void(bool)> done)
{
	QString refresh = Str("refresh_token");
	if (refresh.isEmpty()) {
		done(false);
		return;
	}
	QByteArray body =
		Form({{"client_id", ClientId()}, {"grant_type", "refresh_token"}, {"refresh_token", refresh}});
	Request("POST", QUrl(IdBase() + "/oauth2/token"), {}, body, "application/x-www-form-urlencoded",
		[this, done](int status, const QJsonObject &obj, const QByteArray &) {
			if (status != 200) {
				done(false);
				return;
			}
			config["access_token"] = obj.value("access_token").toString();
			config["refresh_token"] = obj.value("refresh_token").toString();
			config["expires_at"] = QDateTime::currentSecsSinceEpoch() + obj.value("expires_in").toInt();
			emit ConfigChanged();
			done(true);
		});
}
