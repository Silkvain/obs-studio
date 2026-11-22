/******************************************************************************
    Siegfried: installs an AssetPackage.

    - Copies only the files the bundle actually uses into Siegfried's own
      asset folder, so deleting the zip breaks nothing.
    - Writes a new scene collection: scenes, sources with fixed paths, and
      the bundle's transition as a real transition.
    - Alerts go to alerts.json next to the files, for the alert system.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AssetPackage.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <obs.hpp>
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#ifdef _WIN32
#include <QSettings>
#include <windows.h>
#endif

using Pkg = AssetPackage;

namespace {

QString NewUuid()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool HasInput(const char *id)
{
	return obs_source_get_display_name(id) != nullptr;
}

QString Versioned(const QString &id)
{
	const char *v = obs_get_latest_input_type_id(id.toUtf8().constData());
	return v ? QString::fromUtf8(v) : id;
}

QString TextId()
{
#ifdef _WIN32
	return QStringLiteral("text_gdiplus");
#else
	return QStringLiteral("text_ft2_source");
#endif
}

QString CameraId()
{
#if defined(_WIN32)
	return QStringLiteral("dshow_input");
#elif defined(__APPLE__)
	return QStringLiteral("macos-avcapture");
#else
	/* V4L2 is what OBS itself defaults to and needs no portal prompt */
	if (HasInput("v4l2_input")) {
		return QStringLiteral("v4l2_input");
	}
	return QStringLiteral("pipewire-camera-source");
#endif
}

QString CaptureId(QJsonObject &settings)
{
#if defined(_WIN32)
	settings.insert(QStringLiteral("capture_mode"), QStringLiteral("any_fullscreen"));
	return QStringLiteral("game_capture");
#elif defined(__APPLE__)
	(void)settings;
	return QStringLiteral("screen_capture");
#else
	(void)settings;
	if (HasInput("pipewire-screen-capture-source")) {
		return QStringLiteral("pipewire-screen-capture-source");
	}
	return QStringLiteral("xshm_input");
#endif
}

/* GDI+ text settings (what bundles use) -> this platform's text source */
QJsonObject TextSettings(QJsonObject s)
{
	s.remove(QStringLiteral("custom_font"));
	s.insert(QStringLiteral("read_from_file"), false);
	s.remove(QStringLiteral("file"));
#ifndef _WIN32
	/* FreeType: colours are ABGR with alpha; GDI+ is BGR + opacity */
	qint64 c = s.value(QStringLiteral("color")).toInteger(0xFFFFFF) & 0xFFFFFF;
	qint64 abgr = 0xFF000000LL | c;
	s.insert(QStringLiteral("color1"), abgr);
	s.insert(QStringLiteral("color2"), abgr);
	s.insert(QStringLiteral("outline"), s.value(QStringLiteral("outline")).toBool());
	if (s.value(QStringLiteral("extents")).toBool()) {
		s.insert(QStringLiteral("custom_width"), s.value(QStringLiteral("extents_cx")).toInt());
		s.insert(QStringLiteral("word_wrap"), s.value(QStringLiteral("extents_wrap")).toBool());
	}
#endif
	return s;
}

QString SafeFileName(QString name)
{
	static const QString bad = QStringLiteral("<>:\"/\\|?*");
	for (QChar &c : name) {
		if (bad.contains(c) || c.unicode() < 32) {
			c = '_';
		}
	}
	name = name.trimmed();
	return name.isEmpty() ? QStringLiteral("file") : name;
}

/* Bundle title -> folder name */
QString Slug(const QString &title)
{
	QString s = SafeFileName(title);
	s.replace(' ', '-');
	return s.left(60);
}

QString ConfigPath(const QString &sub)
{
	std::filesystem::path base = App()->userConfigLocation / std::filesystem::u8path("obs-studio");
	return QString::fromStdString((base / std::filesystem::u8path(sub.toStdString())).u8string());
}

QJsonObject Vec2(double x, double y)
{
	return QJsonObject{{"x", x}, {"y", y}};
}

/* ------------------------------------------------------------------------ */

struct Builder {
	const Pkg &pkg;
	QString assetDir;
	QHash<int, QString> extracted; /* file index -> absolute path */
	QSet<QString> usedNames;       /* file names in assetDir */
	QJsonArray sources;
	QHash<QString, QString> sourceUuidByKey; /* share sources between scenes */
	QHash<QString, QString> sceneUuidByKey;
	QHash<QString, QString> sceneNameByKey;
	QSet<QString> sourceNames;
	int filesCopied = 0;
	QString error;

	Builder(const Pkg &p, const QString &dir) : pkg(p), assetDir(dir) {}

	QString UniqueSourceName(const QString &wanted)
	{
		QString name = wanted.trimmed().isEmpty() ? QStringLiteral("Source") : wanted.trimmed();
		QString candidate = name;
		for (int i = 2; sourceNames.contains(candidate); i++) {
			candidate = QStringLiteral("%1 (%2)").arg(name).arg(i);
		}
		sourceNames.insert(candidate);
		return candidate;
	}

	/* Extract one bundle file into the asset folder (once) */
	QString Extract(int index, const QString &subdir = QString(), const QString &niceName = QString())
	{
		if (index < 0 || size_t(index) >= pkg.files.size()) {
			return {};
		}
		auto it = extracted.find(index);
		if (it != extracted.end()) {
			return it.value();
		}
		const Pkg::File &f = pkg.files[size_t(index)];
		QString dir = subdir.isEmpty() ? assetDir : assetDir + '/' + subdir;
		QDir().mkpath(dir);

		/* Streamlabs numbers its files ("9.webm"); name them after
		 * what they are so the folder makes sense to people */
		QString name = SafeFileName(f.fileName);
		static const QRegularExpression numbered(QStringLiteral("^\\d+\\.[A-Za-z0-9]+$"));
		if (!niceName.isEmpty() && numbered.match(name).hasMatch()) {
			QString base = QFileInfo(niceName).suffix().isEmpty() ? niceName : QFileInfo(niceName).completeBaseName();
			name = SafeFileName(base) + '.' + QFileInfo(name).suffix();
		}
		QString key = (subdir + '/' + name).toLower();
		if (usedNames.contains(key)) {
			QFileInfo fi(name);
			for (int i = 2;; i++) {
				QString alt = QStringLiteral("%1 (%2).%3").arg(fi.completeBaseName()).arg(i).arg(fi.suffix());
				QString altKey = (subdir + '/' + alt).toLower();
				if (!usedNames.contains(altKey)) {
					name = alt;
					key = altKey;
					break;
				}
			}
		}
		usedNames.insert(key);

		QString path = dir + '/' + name;
		ZipReader *zip = pkg.archives->readers[size_t(f.archive)].get();
		const ZipReader::Entry *e = zip->Find(f.entry);
		QString err;
		if (!e || !zip->ExtractTo(*e, path, err)) {
			error = err.isEmpty() ? QStringLiteral("Could not copy %1").arg(f.fileName) : err;
			return {};
		}
		filesCopied++;
		extracted.insert(index, path);
		return path;
	}

	/* Source JSON for an item; shared sources come back with the same uuid */
	QString SourceFor(const Pkg::Item &it)
	{
		QString id;
		QJsonObject settings = it.settings;
		QString name = it.name;
		QString key;

		switch (it.kind) {
		case Pkg::Kind::Video: {
			QString path = Extract(it.file, QString(), it.name);
			if (path.isEmpty()) {
				return {};
			}
			id = QStringLiteral("ffmpeg_source");
			settings.insert(QStringLiteral("local_file"), path);
			settings.insert(QStringLiteral("is_local_file"), true);
			if (!settings.contains(QStringLiteral("looping"))) {
				settings.insert(QStringLiteral("looping"), true);
			}
			key = QStringLiteral("video|%1|%2").arg(it.file).arg(name);
			break;
		}
		case Pkg::Kind::Image: {
			QString path = Extract(it.file, QString(), it.name);
			if (path.isEmpty()) {
				return {};
			}
			id = QStringLiteral("image_source");
			settings.insert(QStringLiteral("file"), path);
			key = QStringLiteral("image|%1|%2").arg(it.file).arg(name);
			break;
		}
		case Pkg::Kind::Text:
			id = TextId();
			settings = TextSettings(settings);
			break;
		case Pkg::Kind::Label:
			id = TextId();
			settings = TextSettings(settings);
			settings.insert(QStringLiteral("text"), AssetImport::LabelText(it.labelType));
			break;
		case Pkg::Kind::GameCapture:
			settings = QJsonObject();
			id = CaptureId(settings);
			name = QStringLiteral("Game Capture");
			key = QStringLiteral("game");
			break;
		case Pkg::Kind::Webcam:
			settings = QJsonObject();
			id = CameraId();
			name = QStringLiteral("Webcam Capture");
			key = QStringLiteral("webcam");
			break;
		case Pkg::Kind::Other:
			id = it.otherId;
			if (!HasInput(id.toUtf8().constData())) {
				return {};
			}
			break;
		case Pkg::Kind::SceneRef:
			return sceneUuidByKey.value(it.sceneRef);
		}

		if (!key.isEmpty()) {
			auto found = sourceUuidByKey.find(key);
			if (found != sourceUuidByKey.end()) {
				return found.value();
			}
		}

		QString uuid = NewUuid();
		QJsonObject src{
			{"name", UniqueSourceName(name)},
			{"uuid", uuid},
			{"id", id},
			{"versioned_id", Versioned(id)},
			{"settings", settings},
			{"enabled", true},
			{"muted", false},
			{"volume", 1.0},
			{"mixers", 255},
			{"filters", FiltersFor(it)},
		};
		sources.push_back(src);
		if (!key.isEmpty()) {
			sourceUuidByKey.insert(key, uuid);
		}
		return uuid;
	}

	/* Keep filters this build knows; others would only error on load */
	QJsonArray FiltersFor(const Pkg::Item &it)
	{
		QJsonArray out;
		for (const QJsonValue &v : it.filters) {
			QJsonObject f = v.toObject();
			QString fid = f.value(QStringLiteral("id")).toString();
			if (fid.isEmpty()) {
				fid = f.value(QStringLiteral("type")).toString(); /* Streamlabs */
			}
			if (fid.isEmpty() || !obs_source_get_display_name(fid.toUtf8().constData())) {
				continue;
			}
			QJsonObject nf{
				{"name", f.value(QStringLiteral("name")).toString(fid)},
				{"id", fid},
				{"versioned_id", fid},
				{"uuid", NewUuid()},
				{"enabled", f.value(QStringLiteral("enabled")).toBool(true)},
				{"settings", f.value(QStringLiteral("settings")).toObject()},
			};
			out.push_back(nf);
		}
		return out;
	}

	QJsonObject ItemJson(const Pkg::Item &it, const QString &sourceUuid, const QString &sourceName, int itemId)
	{
		const double W = pkg.canvasW, H = pkg.canvasH;
		/* Relative coordinates: OBS rescales them to the user's canvas
		 * (bundles are made for 1080p, streamers may use 1440p) */
		QJsonObject o{
			{"name", sourceName},
			{"source_uuid", sourceUuid},
			{"visible", it.visible},
			{"locked", false},
			{"rot", it.rot},
			{"align", it.align},
			{"bounds_type", it.boundsType},
			{"bounds_align", it.boundsAlign},
			{"bounds_crop", false},
			{"pos_rel", Vec2((2.0 * it.x - W) / H, 2.0 * it.y / H - 1.0)},
			{"scale_rel", Vec2(it.scaleX, it.scaleY)},
			{"scale_ref", Vec2(W, H)},
			{"bounds_rel", Vec2(2.0 * it.boundsX / H, 2.0 * it.boundsY / H)},
			{"pos", Vec2(it.x, it.y)},
			{"scale", Vec2(it.scaleX, it.scaleY)},
			{"bounds", Vec2(it.boundsX, it.boundsY)},
			{"crop_left", it.cropL},
			{"crop_top", it.cropT},
			{"crop_right", it.cropR},
			{"crop_bottom", it.cropB},
			{"id", itemId},
			{"group_item_backup", false},
			{"private_settings", QJsonObject()},
		};
		return o;
	}

	QString SourceName(const QString &uuid) const
	{
		for (const QJsonValue &v : sources) {
			QJsonObject s = v.toObject();
			if (s.value(QStringLiteral("uuid")).toString() == uuid) {
				return s.value(QStringLiteral("name")).toString();
			}
		}
		return {};
	}

	bool Build(QJsonObject &collection, const QString &collectionName)
	{
		/* Scenes first so nested scenes can be referenced */
		for (const Pkg::Scene &sc : pkg.scenes) {
			sceneUuidByKey.insert(sc.key, NewUuid());
			sceneNameByKey.insert(sc.key, UniqueSourceName(sc.name));
		}

		QJsonArray order;
		for (const Pkg::Scene &sc : pkg.scenes) {
			QJsonArray items;
			int itemId = 0;
			for (const Pkg::Item &it : sc.items) {
				QString uuid = SourceFor(it);
				if (!error.isEmpty()) {
					return false;
				}
				if (uuid.isEmpty()) {
					continue;
				}
				QString name = it.kind == Pkg::Kind::SceneRef ? sceneNameByKey.value(it.sceneRef)
									     : SourceName(uuid);
				items.push_back(ItemJson(it, uuid, name, ++itemId));
			}
			QJsonObject scene{
				{"name", sceneNameByKey.value(sc.key)},
				{"uuid", sceneUuidByKey.value(sc.key)},
				{"id", "scene"},
				{"versioned_id", "scene"},
				{"settings", QJsonObject{{"items", items}, {"id_counter", itemId}, {"custom_size", false}}},
				{"enabled", true},
				{"mixers", 0},
			};
			sources.push_back(scene);
			order.push_back(QJsonObject{{"name", sceneNameByKey.value(sc.key)}});
		}

		QJsonArray transitions;
		QString currentTransition = QStringLiteral("Fade");
		int duration = 300;
		if (pkg.transition.present) {
			QJsonObject settings = pkg.transition.settings;
			bool ok = true;
			if (pkg.transition.id == QLatin1String("obs_stinger_transition")) {
				QString path = Extract(pkg.transition.file, QStringLiteral("transition"),
						       QStringLiteral("Transition"));
				ok = !path.isEmpty();
				settings.insert(QStringLiteral("path"), path);
				settings.insert(QStringLiteral("track_matte_path"), QString());
			}
			if (!error.isEmpty()) {
				return false;
			}
			if (ok) {
				transitions.push_back(QJsonObject{
					{"name", pkg.transition.name},
					{"id", pkg.transition.id},
					{"settings", settings},
				});
				currentTransition = pkg.transition.name;
				duration = pkg.transition.durationMs;
			}
		}

		QString first = order.isEmpty() ? QString()
						: order.first().toObject().value(QStringLiteral("name")).toString();
		collection = QJsonObject{
			{"name", collectionName},
			{"current_scene", first},
			{"current_program_scene", first},
			{"scene_order", order},
			{"sources", sources},
			{"groups", QJsonArray()},
			{"transitions", transitions},
			{"current_transition", currentTransition},
			{"transition_duration", duration},
			{"quick_transitions", QJsonArray()},
			{"saved_projectors", QJsonArray()},
			{"modules", QJsonObject()},
			{"preview_locked", false},
			{"scaling_enabled", false},
		};
		return true;
	}
};

int InstallFonts(const Pkg &pkg, Builder &b)
{
	int installed = 0;
	for (int f : pkg.fontFiles) {
		QString path = b.Extract(f, QStringLiteral("fonts"));
		if (path.isEmpty()) {
			continue;
		}
		QString name = QFileInfo(path).fileName();
#ifdef _WIN32
		/* Per-user font install (no admin): copy + HKCU registration */
		QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
			      QStringLiteral("/Microsoft/Windows/Fonts");
		QDir().mkpath(dir);
		QString dest = dir + '/' + name;
		if (!QFile::exists(dest) && !QFile::copy(path, dest)) {
			continue;
		}
		QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"),
			      QSettings::NativeFormat);
		reg.setValue(QFileInfo(name).completeBaseName() + QStringLiteral(" (TrueType)"),
			     QDir::toNativeSeparators(dest));
		AddFontResourceExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(dest).utf16()), 0, nullptr);
#else
		QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
			      QStringLiteral("/fonts/siegfried");
		QDir().mkpath(dir);
		QString dest = dir + '/' + name;
		if (!QFile::exists(dest) && !QFile::copy(path, dest)) {
			continue;
		}
#endif
		installed++;
	}
	return installed;
}

} // namespace

bool AssetImport::Install(const AssetPackage &pkg, const QString &collectionName, bool installFonts,
			  InstallResult &result, QString &error)
{
	/* Asset folder: <config>/obs-studio/siegfried/assets/<bundle> */
	QString base = ConfigPath(QStringLiteral("siegfried/assets/") + Slug(pkg.title));
	QString dir = base;
	for (int i = 2; QDir(dir).exists(); i++) {
		dir = QStringLiteral("%1-%2").arg(base).arg(i);
	}
	if (!QDir().mkpath(dir)) {
		error = QStringLiteral("Could not create %1").arg(dir);
		return false;
	}

	Builder b(pkg, dir);
	QJsonObject collection;
	if (!b.Build(collection, collectionName)) {
		error = b.error;
		QDir(dir).removeRecursively();
		return false;
	}

	/* Alerts: files + a manifest for the alert system */
	QJsonArray alerts;
	for (const Pkg::Alert &a : pkg.alerts) {
		QString path = b.Extract(a.file, QStringLiteral("alerts"));
		if (path.isEmpty()) {
			error = b.error;
			QDir(dir).removeRecursively();
			return false;
		}
		alerts.push_back(QJsonObject{{"event", a.event}, {"file", path}});
	}
	QJsonObject manifest{
		{"title", pkg.title},
		{"source", QFileInfo(pkg.archives->path).fileName()},
		{"format", pkg.format},
		{"collection", collectionName},
		{"alerts", alerts},
	};

	if (installFonts) {
		result.fontsInstalled = InstallFonts(pkg, b);
	}

	QFile mf(dir + QStringLiteral("/siegfried-package.json"));
	if (mf.open(QIODevice::WriteOnly)) {
		mf.write(QJsonDocument(manifest).toJson());
	}

	/* The scene collection file */
	std::string fileName;
	if (!GetFileSafeName(collectionName.toUtf8().constData(), fileName)) {
		fileName = "Imported";
	}
	std::filesystem::path scenesDir = App()->userScenesLocation / std::filesystem::u8path("obs-studio/basic/scenes/");
	std::string collectionFile = scenesDir.u8string() + fileName;
	if (!GetClosestUnusedFileName(collectionFile, "json")) {
		error = QStringLiteral("Could not pick a file name for the scene collection");
		QDir(dir).removeRecursively();
		return false;
	}
	QFile cf(QString::fromStdString(collectionFile));
	if (!cf.open(QIODevice::WriteOnly) || cf.write(QJsonDocument(collection).toJson()) < 0) {
		error = QStringLiteral("Could not write the scene collection");
		QDir(dir).removeRecursively();
		return false;
	}
	cf.close();

	result.collectionName = collectionName;
	result.assetDir = dir;
	result.filesCopied = b.filesCopied;
	blog(LOG_INFO, "[import-assets] Installed '%s' (%d files) as scene collection '%s'",
	     pkg.title.toUtf8().constData(), b.filesCopied, collectionName.toUtf8().constData());
	return true;
}
