/******************************************************************************
    Multichat: Rumble. See RumbleChat.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "RumbleChat.hpp"

#include <QJsonArray>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "moc_RumbleChat.cpp"

namespace {
constexpr int kPollMs = 3000;
constexpr int kMaxSeen = 4000;
constexpr int kBacklogOnConnect = 15;
} // namespace

RumbleChat::RumbleChat(QObject *parent) : ChatConnector(ChatPlatform::Rumble, parent)
{
	pollTimer.setSingleShot(true);
	connect(&pollTimer, &QTimer::timeout, this, &RumbleChat::Poll);
}

void RumbleChat::Start()
{
	Stop();
	if (!Enabled() || Str("api_url").trimmed().isEmpty()) {
		SetState(ChatState::Disabled, Enabled() ? tr("Paste your Live Stream API URL") : QString());
		return;
	}
	running = true;
	primed = false;
	seen.clear();
	SetState(ChatState::Connecting, tr("Connecting…"));
	Poll();
}

void RumbleChat::Stop()
{
	running = false;
	pollTimer.stop();
	if (state != ChatState::Disabled) {
		SetState(ChatState::Disabled);
	}
}

void RumbleChat::Poll()
{
	if (!running) {
		return;
	}
	Request("GET", QUrl(Str("api_url").trimmed()), {{"Accept", "application/json"}}, {}, {},
		[this](int status, const QJsonObject &obj, const QByteArray &) {
			if (!running) {
				return;
			}
			pollTimer.start(kPollMs);
			if (status != 200) {
				SetState(ChatState::Error, status == 0 ? tr("Can't reach Rumble")
								       : tr("Rumble API error (HTTP %1)").arg(status));
				return;
			}

			QJsonArray streams = obj.value("livestreams").toArray();
			QJsonObject live;
			for (const QJsonValue &v : streams) {
				if (v.toObject().value("is_live").toBool()) {
					live = v.toObject();
					break;
				}
			}
			if (live.isEmpty()) {
				SetState(ChatState::Connected, tr("Connected, waiting for you to go live"));
				return;
			}
			SetState(ChatState::Connected, tr("Reading your live chat (read-only)"));

			QJsonObject chat = live.value("chat").toObject();
			struct Item {
				QJsonObject o;
				bool rant;
			};
			QList<Item> items;
			for (const QJsonValue &v : chat.value("recent_messages").toArray()) {
				items.append({v.toObject(), false});
			}
			for (const QJsonValue &v : chat.value("recent_rants").toArray()) {
				items.append({v.toObject(), true});
			}

			QList<ChatMessage> fresh;
			for (const Item &it : items) {
				QString user = it.o.value("username").toString();
				QString text = it.o.value("text").toString();
				QString when = it.o.value("created_on").toString();
				QString key = user + '\x1f' + when + '\x1f' + text;
				if (seen.contains(key)) {
					continue;
				}
				seen.insert(key);

				ChatMessage msg;
				msg.platform = ChatPlatform::Rumble;
				msg.user.platform = ChatPlatform::Rumble;
				msg.user.id = user; /* Rumble's API exposes usernames, not ids */
				msg.user.login = user;
				msg.user.displayName = user;
				msg.text = text;
				QDateTime t = QDateTime::fromString(when, Qt::ISODate);
				if (t.isValid()) {
					msg.time = t.toLocalTime();
				}
				if (it.rant) {
					msg.highlight =
						tr("$%1 Rant").arg(it.o.value("amount_dollars").toVariant().toString());
				}
				fresh.append(msg);
			}

			std::sort(fresh.begin(), fresh.end(),
				  [](const ChatMessage &a, const ChatMessage &b) { return a.time < b.time; });
			/* First poll: only show a short backlog, not the whole recent history */
			int start = primed ? 0 : std::max(0, int(fresh.size()) - kBacklogOnConnect);
			for (int i = start; i < fresh.size(); i++) {
				emit MessageReceived(fresh[i]);
			}
			primed = true;

			if (seen.size() > kMaxSeen) {
				/* Forget old keys, but keep everything the API still returns */
				QSet<QString> keep;
				for (const Item &it : items) {
					keep.insert(it.o.value("username").toString() + '\x1f' +
						    it.o.value("created_on").toString() + '\x1f' +
						    it.o.value("text").toString());
				}
				seen = keep;
			}
		});
}
