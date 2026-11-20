/******************************************************************************
    Minimal WebSocket client. See ChatSocket.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ChatSocket.hpp"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSslSocket>

#include "moc_ChatSocket.cpp"

namespace {
constexpr const char *kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr qsizetype kMaxBuffer = 16 * 1024 * 1024; /* drop absurd frames */

enum Opcode : quint8 {
	OpContinuation = 0x0,
	OpText = 0x1,
	OpBinary = 0x2,
	OpClose = 0x8,
	OpPing = 0x9,
	OpPong = 0xA,
};
} // namespace

ChatSocket::ChatSocket(QObject *parent) : QObject(parent), socket(new QSslSocket(this))
{
	connect(socket, &QSslSocket::connected, this, [this] {
		if (url.scheme() == "ws") {
			SendHandshake();
		}
	});
	connect(socket, &QSslSocket::encrypted, this, &ChatSocket::SendHandshake);
	connect(socket, &QSslSocket::readyRead, this, &ChatSocket::OnReadyRead);
	connect(socket, &QSslSocket::disconnected, this, &ChatSocket::Finish);
	connect(socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { Finish(); });
}

ChatSocket::~ChatSocket()
{
	socket->disconnect(this);
	socket->abort();
}

void ChatSocket::open(const QUrl &u)
{
	url = u;
	state = State::Connecting;
	buffer.clear();
	fragment.clear();

	bool secure = url.scheme() == "wss";
	quint16 port = quint16(url.port(secure ? 443 : 80));
	if (secure) {
		socket->connectToHostEncrypted(url.host(), port);
	} else {
		socket->connectToHost(url.host(), port);
	}
}

void ChatSocket::SendHandshake()
{
	QByteArray raw(16, Qt::Uninitialized);
	for (char &c : raw) {
		c = char(QRandomGenerator::global()->bounded(256));
	}
	key = raw.toBase64();

	QByteArray path = url.path(QUrl::FullyEncoded).toUtf8();
	if (path.isEmpty()) {
		path = "/";
	}
	if (url.hasQuery()) {
		path += "?" + url.query(QUrl::FullyEncoded).toUtf8();
	}

	QByteArray host = url.host().toUtf8();
	int defaultPort = url.scheme() == "wss" ? 443 : 80;
	if (url.port(defaultPort) != defaultPort) {
		host += ":" + QByteArray::number(url.port());
	}

	QByteArray req = "GET " + path + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" +
			 "Upgrade: websocket\r\n"
			 "Connection: Upgrade\r\n"
			 "Sec-WebSocket-Key: " +
			 key + "\r\n" +
			 "Sec-WebSocket-Version: 13\r\n"
			 "User-Agent: OBS-Multichat\r\n"
			 "\r\n";
	state = State::Handshake;
	socket->write(req);
}

void ChatSocket::OnReadyRead()
{
	buffer += socket->readAll();
	if (buffer.size() > kMaxBuffer) {
		socket->abort();
		return;
	}
	if (state == State::Handshake && !ParseHandshake()) {
		return;
	}
	if (state == State::Open || state == State::Closing) {
		ParseFrames();
	}
}

bool ChatSocket::ParseHandshake()
{
	qsizetype end = buffer.indexOf("\r\n\r\n");
	if (end < 0) {
		return false;
	}

	QByteArray head = buffer.left(end);
	buffer.remove(0, end + 4);

	QList<QByteArray> lines = head.split('\n');
	bool ok = !lines.isEmpty() && lines.first().contains(" 101 ");

	QByteArray expected = QCryptographicHash::hash(key + kGuid, QCryptographicHash::Sha1).toBase64();
	bool acceptOk = false;
	for (const QByteArray &l : lines) {
		qsizetype colon = l.indexOf(':');
		if (colon < 0) {
			continue;
		}
		if (l.left(colon).trimmed().toLower() == "sec-websocket-accept") {
			acceptOk = l.mid(colon + 1).trimmed() == expected;
		}
	}

	if (!ok || !acceptOk) {
		socket->abort();
		return false;
	}
	state = State::Open;
	emit connected();
	return true;
}

void ChatSocket::ParseFrames()
{
	for (;;) {
		if (buffer.size() < 2) {
			return;
		}
		const auto *d = reinterpret_cast<const quint8 *>(buffer.constData());
		bool fin = d[0] & 0x80;
		quint8 opcode = d[0] & 0x0F;
		bool masked = d[1] & 0x80;
		quint64 len = d[1] & 0x7F;
		qsizetype pos = 2;

		if (len == 126) {
			if (buffer.size() < 4) {
				return;
			}
			len = (quint64(d[2]) << 8) | d[3];
			pos = 4;
		} else if (len == 127) {
			if (buffer.size() < 10) {
				return;
			}
			len = 0;
			for (int i = 0; i < 8; i++) {
				len = (len << 8) | d[2 + i];
			}
			pos = 10;
		}
		if (len > quint64(kMaxBuffer)) {
			socket->abort();
			return;
		}

		QByteArray mask;
		if (masked) {
			if (buffer.size() < pos + 4) {
				return;
			}
			mask = buffer.mid(pos, 4);
			pos += 4;
		}
		if (buffer.size() < pos + qsizetype(len)) {
			return;
		}

		QByteArray payload = buffer.mid(pos, qsizetype(len));
		buffer.remove(0, pos + qsizetype(len));
		if (masked) {
			for (qsizetype i = 0; i < payload.size(); i++) {
				payload[i] = char(payload[i] ^ mask[i % 4]);
			}
		}

		switch (opcode) {
		case OpText:
		case OpBinary:
			if (fin) {
				if (opcode == OpText) {
					emit textMessageReceived(QString::fromUtf8(payload));
				}
			} else {
				fragment = payload;
				fragmentOpcode = opcode;
			}
			break;
		case OpContinuation:
			fragment += payload;
			if (fin) {
				if (fragmentOpcode == OpText) {
					emit textMessageReceived(QString::fromUtf8(fragment));
				}
				fragment.clear();
			}
			break;
		case OpPing:
			SendFrame(OpPong, payload);
			break;
		case OpClose:
			if (state == State::Open) {
				SendFrame(OpClose, payload.left(2));
			}
			state = State::Closing;
			socket->disconnectFromHost();
			return;
		default:
			break;
		}
		if (state != State::Open) {
			return;
		}
	}
}

void ChatSocket::SendFrame(quint8 opcode, const QByteArray &payload)
{
	if (socket->state() != QAbstractSocket::ConnectedState) {
		return;
	}

	QByteArray frame;
	frame.append(char(0x80 | opcode));
	qsizetype n = payload.size();
	if (n < 126) {
		frame.append(char(0x80 | n));
	} else if (n <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		frame.append(char((n >> 8) & 0xFF));
		frame.append(char(n & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (int i = 7; i >= 0; i--) {
			frame.append(char((quint64(n) >> (8 * i)) & 0xFF));
		}
	}

	/* Client frames must be masked */
	quint32 m = QRandomGenerator::global()->generate();
	char mask[4] = {char(m >> 24), char(m >> 16), char(m >> 8), char(m)};
	frame.append(mask, 4);
	QByteArray body = payload;
	for (qsizetype i = 0; i < body.size(); i++) {
		body[i] = char(body[i] ^ mask[i % 4]);
	}
	frame += body;
	socket->write(frame);
}

void ChatSocket::sendTextMessage(const QString &text)
{
	if (state == State::Open) {
		SendFrame(OpText, text.toUtf8());
	}
}

void ChatSocket::close()
{
	if (state == State::Open) {
		SendFrame(OpClose, QByteArray("\x03\xe8", 2)); /* 1000 normal */
		state = State::Closing;
		socket->disconnectFromHost();
	} else if (state != State::Closed && state != State::Idle) {
		socket->abort();
	}
}

void ChatSocket::Finish()
{
	if (state == State::Closed || state == State::Idle) {
		return;
	}
	state = State::Closed;
	emit disconnected();
}
