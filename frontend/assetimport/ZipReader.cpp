/******************************************************************************
    Siegfried: minimal, defensive ZIP reader. See ZipReader.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ZipReader.hpp"

#include <QBuffer>
#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace {

quint16 U16(const char *p)
{
	return quint16(quint8(p[0]) | (quint8(p[1]) << 8));
}

quint32 U32(const char *p)
{
	return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) | (quint32(quint8(p[2])) << 16) |
	       (quint32(quint8(p[3])) << 24);
}

quint64 U64(const char *p)
{
	return quint64(U32(p)) | (quint64(U32(p + 4)) << 32);
}

constexpr quint32 kEocdSig = 0x06054b50;
constexpr quint32 kZip64LocatorSig = 0x07064b50;
constexpr quint32 kZip64EocdSig = 0x06064b50;
constexpr quint32 kCentralSig = 0x02014b50;
constexpr quint32 kLocalSig = 0x04034b50;

/* Only plain relative paths: no "..", no absolute paths, no drive letters */
bool SafeName(const QString &name)
{
	if (name.isEmpty() || name.startsWith('/') || (name.size() > 1 && name[1] == ':')) {
		return false;
	}
	for (const QString &part : name.split('/')) {
		if (part == QLatin1String("..")) {
			return false;
		}
	}
	return !name.contains(QChar(0));
}

} // namespace

std::unique_ptr<ZipReader> ZipReader::Open(std::unique_ptr<QIODevice> device, QString &error)
{
	if (!device || !device->isOpen()) {
		error = QStringLiteral("Could not open the archive");
		return nullptr;
	}
	std::unique_ptr<ZipReader> zip(new ZipReader(std::move(device)));
	if (!zip->ReadDirectory(error)) {
		return nullptr;
	}
	return zip;
}

std::unique_ptr<ZipReader> ZipReader::OpenFile(const QString &path, QString &error)
{
	auto file = std::make_unique<QFile>(path);
	if (!file->open(QIODevice::ReadOnly)) {
		error = QStringLiteral("Could not open %1").arg(path);
		return nullptr;
	}
	return Open(std::move(file), error);
}

std::unique_ptr<ZipReader> ZipReader::OpenBytes(QByteArray bytes, QString &error)
{
	auto buffer = std::make_unique<QBuffer>();
	buffer->setData(std::move(bytes));
	buffer->open(QIODevice::ReadOnly);
	return Open(std::move(buffer), error);
}

const ZipReader::Entry *ZipReader::Find(const QString &name) const
{
	for (const Entry &e : entries) {
		if (e.name == name) {
			return &e;
		}
	}
	return nullptr;
}

bool ZipReader::ReadDirectory(QString &error)
{
	const qint64 fileSize = dev->size();
	if (fileSize < 22) {
		error = QStringLiteral("Not a ZIP file");
		return false;
	}

	/* End of central directory: last 22 bytes + up to 64 KiB comment */
	const qint64 tailSize = std::min<qint64>(fileSize, 22 + 65535);
	dev->seek(fileSize - tailSize);
	QByteArray tail = dev->read(tailSize);
	qint64 eocd = -1;
	for (qint64 i = tail.size() - 22; i >= 0; i--) {
		if (U32(tail.constData() + i) == kEocdSig) {
			eocd = i;
			break;
		}
	}
	if (eocd < 0) {
		error = QStringLiteral("Not a ZIP file (no directory)");
		return false;
	}

	const char *e = tail.constData() + eocd;
	quint64 count = U16(e + 10);
	quint64 cdSize = U32(e + 12);
	quint64 cdOffset = U32(e + 16);

	/* ZIP64: locator sits right before the EOCD record */
	if (eocd >= 20 && U32(tail.constData() + eocd - 20) == kZip64LocatorSig) {
		quint64 z64Offset = U64(tail.constData() + eocd - 20 + 8);
		dev->seek(qint64(z64Offset));
		QByteArray z = dev->read(56);
		if (z.size() == 56 && U32(z.constData()) == kZip64EocdSig) {
			count = U64(z.constData() + 32);
			cdSize = U64(z.constData() + 40);
			cdOffset = U64(z.constData() + 48);
		}
	}

	if (count > quint64(kMaxEntries) || cdOffset + cdSize > quint64(fileSize)) {
		error = QStringLiteral("The archive's directory is damaged or too large");
		return false;
	}

	dev->seek(qint64(cdOffset));
	QByteArray cd = dev->read(qint64(cdSize));
	if (quint64(cd.size()) != cdSize) {
		error = QStringLiteral("Could not read the archive's directory");
		return false;
	}

	QStringDecoder latin1(QStringDecoder::Latin1);
	qint64 pos = 0;
	for (quint64 i = 0; i < count; i++) {
		if (pos + 46 > cd.size() || U32(cd.constData() + pos) != kCentralSig) {
			error = QStringLiteral("The archive's directory is damaged");
			return false;
		}
		const char *h = cd.constData() + pos;
		quint16 flags = U16(h + 8);
		Entry entry;
		entry.method = U16(h + 10);
		entry.crc = U32(h + 16);
		entry.compressedSize = U32(h + 20);
		entry.size = U32(h + 24);
		quint16 nameLen = U16(h + 28);
		quint16 extraLen = U16(h + 30);
		quint16 commentLen = U16(h + 32);
		entry.headerOffset = U32(h + 42);

		if (pos + 46 + nameLen + extraLen + commentLen > cd.size()) {
			error = QStringLiteral("The archive's directory is damaged");
			return false;
		}
		QByteArray rawName(h + 46, nameLen);
		/* Flag 11 = UTF-8. Without it most tools still write UTF-8;
		 * fall back to Latin-1 only if it isn't valid UTF-8. */
		QString name = QString::fromUtf8(rawName);
		if (!(flags & 0x800) && name.contains(QChar::ReplacementCharacter)) {
			name = latin1(rawName);
		}
		name.replace('\\', '/');

		/* ZIP64 extra field carries the real sizes/offset */
		const char *x = h + 46 + nameLen;
		const char *xEnd = x + extraLen;
		while (x + 4 <= xEnd) {
			quint16 id = U16(x), len = U16(x + 2);
			const char *v = x + 4;
			if (v + len > xEnd) {
				break;
			}
			if (id == 0x0001) {
				const char *p = v;
				if (entry.size == 0xFFFFFFFFu && p + 8 <= v + len) {
					entry.size = U64(p);
					p += 8;
				}
				if (entry.compressedSize == 0xFFFFFFFFu && p + 8 <= v + len) {
					entry.compressedSize = U64(p);
					p += 8;
				}
				if (entry.headerOffset == 0xFFFFFFFFu && p + 8 <= v + len) {
					entry.headerOffset = U64(p);
				}
			}
			x = v + len;
		}

		pos += 46 + nameLen + extraLen + commentLen;

		entry.isDir = name.endsWith('/');
		entry.name = entry.isDir ? name.chopped(1) : name;
		bool encrypted = flags & 0x1;
		if (!SafeName(entry.name) || encrypted) {
			continue; /* skipped, never extracted */
		}
		entries.push_back(std::move(entry));
	}
	return true;
}

bool ZipReader::DataOffset(const Entry &e, quint64 &offset, QString &error)
{
	if (!dev->seek(qint64(e.headerOffset))) {
		error = QStringLiteral("Damaged entry: %1").arg(e.name);
		return false;
	}
	QByteArray h = dev->read(30);
	if (h.size() != 30 || U32(h.constData()) != kLocalSig) {
		error = QStringLiteral("Damaged entry: %1").arg(e.name);
		return false;
	}
	offset = e.headerOffset + 30 + U16(h.constData() + 26) + U16(h.constData() + 28);
	if (offset + e.compressedSize > quint64(dev->size())) {
		error = QStringLiteral("Damaged entry: %1").arg(e.name);
		return false;
	}
	return true;
}

template<typename Sink> bool ZipReader::Inflate(const Entry &e, Sink &&sink, QString &error)
{
	if (e.isDir) {
		return true;
	}
	if (e.size > kMaxEntrySize) {
		error = QStringLiteral("%1 is too large").arg(e.name);
		return false;
	}
	if (e.method != 0 && e.method != 8) {
		error = QStringLiteral("%1 uses an unsupported compression method").arg(e.name);
		return false;
	}
	quint64 offset;
	if (!DataOffset(e, offset, error)) {
		return false;
	}
	dev->seek(qint64(offset));

	constexpr qint64 kChunk = 256 * 1024;
	quint64 remaining = e.compressedSize;
	quint64 produced = 0;
	uLong crc = crc32(0L, Z_NULL, 0);

	if (e.method == 0) {
		while (remaining > 0) {
			QByteArray chunk = dev->read(qint64(std::min<quint64>(remaining, kChunk)));
			if (chunk.isEmpty()) {
				error = QStringLiteral("Damaged entry: %1").arg(e.name);
				return false;
			}
			remaining -= quint64(chunk.size());
			crc = crc32(crc, reinterpret_cast<const Bytef *>(chunk.constData()), uInt(chunk.size()));
			produced += quint64(chunk.size());
			if (!sink(chunk.constData(), qint64(chunk.size()))) {
				return false;
			}
		}
	} else {
		z_stream zs = {};
		if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
			error = QStringLiteral("Could not start decompression");
			return false;
		}
		std::vector<char> in(kChunk), out(kChunk);
		int ret = Z_OK;
		do {
			if (zs.avail_in == 0 && remaining > 0) {
				qint64 got = dev->read(in.data(), qint64(std::min<quint64>(remaining, kChunk)));
				if (got <= 0) {
					break;
				}
				remaining -= quint64(got);
				zs.next_in = reinterpret_cast<Bytef *>(in.data());
				zs.avail_in = uInt(got);
			}
			zs.next_out = reinterpret_cast<Bytef *>(out.data());
			zs.avail_out = uInt(out.size());
			ret = inflate(&zs, Z_NO_FLUSH);
			if (ret == Z_BUF_ERROR && zs.avail_in == 0 && remaining == 0) {
				break; /* truncated: caught by the size check below */
			}
			if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
				inflateEnd(&zs);
				error = QStringLiteral("Damaged entry: %1").arg(e.name);
				return false;
			}
			qint64 n = qint64(out.size() - zs.avail_out);
			produced += quint64(n);
			/* Never trust the declared size: stops zip bombs */
			if (produced > e.size) {
				inflateEnd(&zs);
				error = QStringLiteral("%1 is larger than it claims").arg(e.name);
				return false;
			}
			if (n > 0) {
				crc = crc32(crc, reinterpret_cast<const Bytef *>(out.data()), uInt(n));
				if (!sink(out.data(), n)) {
					inflateEnd(&zs);
					return false;
				}
			}
		} while (ret != Z_STREAM_END);
		inflateEnd(&zs);
	}

	if (produced != e.size || crc != e.crc) {
		error = QStringLiteral("%1 is damaged (checksum mismatch)").arg(e.name);
		return false;
	}
	return true;
}

bool ZipReader::Read(const Entry &e, QByteArray &out, quint64 maxSize, QString &error)
{
	if (e.size > maxSize) {
		error = QStringLiteral("%1 is too large").arg(e.name);
		return false;
	}
	out.clear();
	out.reserve(qsizetype(e.size));
	return Inflate(
		e,
		[&](const char *data, qint64 n) {
			out.append(data, qsizetype(n));
			return true;
		},
		error);
}

bool ZipReader::ExtractTo(const Entry &e, const QString &path, QString &error)
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		error = QStringLiteral("Could not write %1").arg(path);
		return false;
	}
	bool ok = Inflate(
		e,
		[&](const char *data, qint64 n) {
			if (file.write(data, n) != n) {
				error = QStringLiteral("Could not write %1").arg(path);
				return false;
			}
			return true;
		},
		error);
	if (!ok) {
		file.cancelWriting();
		return false;
	}
	if (!file.commit()) {
		error = QStringLiteral("Could not write %1").arg(path);
		return false;
	}
	return true;
}
