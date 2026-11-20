/******************************************************************************
    Multichat: Rumble.
    Polls the official Rumble Live Stream API URL (rumble.com/account/
    livestream-api). Read-only: Rumble's API has no send or moderation.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnector.hpp"

#include <QSet>
#include <QTimer>

class RumbleChat : public ChatConnector {
	Q_OBJECT

public:
	explicit RumbleChat(QObject *parent);

	void Start() override;
	void Stop() override;
	bool LoggedIn() const override { return false; }
	bool CanSend() const override { return false; }
	bool CanModerate() const override { return false; }
	QString LoginRequirement() const override
	{
		return tr("Rumble's API is read-only; no login needed. Paste your Live Stream API URL.");
	}
	void Login(QWidget *) override {}

	void SendChat(const QString &) override {}
	void Ban(const ChatUser &, const QString &) override { Unsupported(); }
	void Timeout(const ChatUser &, int, const QString &) override { Unsupported(); }
	void Unban(const ChatUser &) override { Unsupported(); }

private:
	void Poll();
	void Unsupported() { EmitNotice(tr("Rumble doesn't offer moderation through its API. Use rumble.com."), true); }

	QTimer pollTimer;
	bool running = false;
	bool primed = false;
	QSet<QString> seen;
};
