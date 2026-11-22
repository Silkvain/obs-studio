/******************************************************************************
    Siegfried: minimal, defensive ZIP reader (stored + deflate) on zlib.

    Overlay bundles are untrusted downloads, so this reader never writes
    anywhere by itself, rejects odd entries, and caps sizes.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>

#include <memory>
#include <vector>

class ZipReader {
public:
	struct Entry {
		QString name;        /* path inside the archive, '/' separated */
		quint16 method = 0;  /* 0 = stored, 8 = deflate */
		quint32 crc = 0;
		quint64 compressedSize = 0;
		quint64 size = 0;    /* uncompressed */
		quint64 headerOffset = 0;
		bool isDir = false;
	};

	/* Reader takes ownership of the device */
	static std::unique_ptr<ZipReader> Open(std::unique_ptr<QIODevice> device, QString &error);
	static std::unique_ptr<ZipReader> OpenFile(const QString &path, QString &error);
	static std::unique_ptr<ZipReader> OpenBytes(QByteArray bytes, QString &error);

	const std::vector<Entry> &Entries() const { return entries; }
	const Entry *Find(const QString &name) const;

	/* Whole entry in memory, refused above maxSize */
	bool Read(const Entry &e, QByteArray &out, quint64 maxSize, QString &error);
	/* Streams an entry into a file */
	bool ExtractTo(const Entry &e, const QString &path, QString &error);

	/* Hard limits for untrusted archives */
	static constexpr quint64 kMaxEntrySize = 4ull << 30; /* 4 GiB */
	static constexpr int kMaxEntries = 50000;

private:
	explicit ZipReader(std::unique_ptr<QIODevice> d) : dev(std::move(d)) {}
	bool ReadDirectory(QString &error);
	bool DataOffset(const Entry &e, quint64 &offset, QString &error);
	template<typename Sink> bool Inflate(const Entry &e, Sink &&sink, QString &error);

	std::unique_ptr<QIODevice> dev;
	std::vector<Entry> entries;
};
