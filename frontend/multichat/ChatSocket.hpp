/******************************************************************************
    Minimal RFC 6455 WebSocket client (text frames, ping/pong, close) on
    top of QSslSocket. OBS's Qt builds on Windows and macOS don't include
    the Qt WebSockets module, so Multichat brings its own.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QByteArray>
#include <QObject>
#include <QUrl>

class QSslSocket;

class ChatSocket : public QObject {
	Q_OBJECT

public:
	explicit ChatSocket(QObject *parent = nullptr);
	~ChatSocket() override;

	void open(const QUrl &url); /* ws:// or wss:// */
	void sendTextMessage(const QString &text);
	void close();
	bool isOpen() const { return state == State::Open; }

signals:
	void connected();
	void disconnected();
	void textMessageReceived(const QString &message);

private:
	enum class State { Idle, Connecting, Handshake, Open, Closing, Closed };

	void SendHandshake();
	void OnReadyRead();
	bool ParseHandshake();
	void ParseFrames();
	void SendFrame(quint8 opcode, const QByteArray &payload);
	void Finish();

	QSslSocket *socket;
	QUrl url;
	State state = State::Idle;
	QByteArray buffer;
	QByteArray key;
	QByteArray fragment;
	quint8 fragmentOpcode = 0;
};
