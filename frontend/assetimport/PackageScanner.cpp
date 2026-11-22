/******************************************************************************
    Siegfried: reads overlay bundles into AssetPackage.

    Order of trust:
      1. Streamlabs .overlay (zip with config.json): relative paths,
         scenes, transition. What Streamlabs itself installs from.
      2. OBS scene collection (.json): paths usually point at the
         creator's PC, so every path is repaired from the bundle.
      3. Loose files: scenes guessed from file and folder names.
    Alerts are matched by name in every case: bundles never carry the
    alert setup itself (that lives on the seller's/Streamlabs' website).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "AssetPackage.hpp"

#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QRegularExpression>

#include <algorithm>

using Pkg = AssetPackage;

namespace {

constexpr quint64 kMaxNestedArchive = 2ull << 30; /* 2 GiB in memory */
constexpr quint64 kMaxConfig = 64ull << 20;
constexpr int kMaxNested = 8;

const QStringList kVideoExt = {"webm", "mp4", "mov", "mkv", "gif", "apng"};
const QStringList kImageExt = {"png", "jpg", "jpeg", "bmp", "tga", "webp"};
const QStringList kFontExt = {"ttf", "otf"};

QString Ext(const QString &name)
{
	return QFileInfo(name).suffix().toLower();
}

QString BaseName(const QString &path)
{
	QString p = path;
	p.replace('\\', '/');
	return p.section('/', -1);
}

bool IsJunk(const QString &entry)
{
	QString base = BaseName(entry);
	return entry.contains(QLatin1String("__MACOSX/")) || base.startsWith(QLatin1String("._")) ||
	       base == QLatin1String(".DS_Store") || base.compare(QLatin1String("Thumbs.db"), Qt::CaseInsensitive) == 0 ||
	       base.compare(QLatin1String("desktop.ini"), Qt::CaseInsensitive) == 0;
}

bool IsMedia(const QString &name)
{
	QString e = Ext(name);
	return kVideoExt.contains(e) || kImageExt.contains(e);
}

/* "Pirate_Hunter_Stream_Package.zip" -> "Pirate Hunter Stream Package" */
QString CleanTitle(const QString &path)
{
	QString t = QFileInfo(path).completeBaseName();
	t.replace('_', ' ');
	t.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
	return t.trimmed();
}

/* ------------------------------------------------------------------------ */
/* Alerts by name                                                           */

QString AlertEvent(const QString &entry)
{
	QString n = QFileInfo(entry).completeBaseName().toLower();
	QString path = entry.toLower();
	bool alertish = path.contains(QLatin1String("alert")) || n.startsWith(QLatin1String("new")) ||
			n.contains(QLatin1String("alert"));
	if (!alertish) {
		return {};
	}
	struct Rule {
		const char *pattern;
		const char *event;
	};
	static const Rule rules[] = {
		{"gift", "gift_sub"},          {"resub", "resubscribe"},      {"sub", "subscribe"},
		{"member", "membership"},      {"sponsor", "membership"},     {"follow", "follow"},
		{"bit", "bits"},               {"cheer", "bits"},             {"donat", "donation"},
		{"tip", "donation"},           {"raid", "raid"},              {"host", "host"},
		{"super ?chat", "superchat"},  {"super ?sticker", "superchat"}, {"star", "stars"},
		{"supporter", "fb_support"},   {"like", "fb_like"},           {"share", "fb_share"},
		{"merch", "merch"},            {"charity", "charity"},        {"rant", "rant"},
	};
	for (const Rule &r : rules) {
		if (n.contains(QRegularExpression(QString::fromLatin1(r.pattern)))) {
			return QString::fromLatin1(r.event);
		}
	}
	return {};
}

/* ------------------------------------------------------------------------ */
/* Scanner state                                                            */

struct Scanner {
	Pkg &pkg;
	QHash<QString, int> fileByKey;              /* "archive|entry" -> file */
	QMultiHash<QString, int> fileByBase;        /* lower-case base name -> file */
	std::vector<int> overlayArchives;           /* readers that are .overlay files */
	std::vector<std::pair<int, QString>> obsCollections; /* archive, entry */
	QSet<QString> warned;

	explicit Scanner(Pkg &p) : pkg(p) {}

	int AddFile(int archive, const ZipReader::Entry &e)
	{
		QString key = QString::number(archive) + '|' + e.name;
		auto it = fileByKey.find(key);
		if (it != fileByKey.end()) {
			return it.value();
		}
		Pkg::File f;
		f.archive = archive;
		f.entry = e.name;
		f.fileName = BaseName(e.name);
		f.size = e.size;
		pkg.files.push_back(f);
		int index = int(pkg.files.size()) - 1;
		fileByKey.insert(key, index);
		fileByBase.insert(f.fileName.toLower(), index);
		return index;
	}

	int FileInArchive(int archive, const QString &entry) const
	{
		return fileByKey.value(QString::number(archive) + '|' + entry, -1);
	}

	void Once(Pkg::Severity s, const QString &text)
	{
		if (!warned.contains(text)) {
			warned.insert(text);
			pkg.Add(s, text);
		}
	}

	/* Index one archive, opening bundles inside it */
	void Index(int archive, int depth)
	{
		ZipReader *zip = pkg.archives->readers[size_t(archive)].get();
		for (const ZipReader::Entry &e : zip->Entries()) {
			if (e.isDir) {
				continue;
			}
			if (IsJunk(e.name)) {
				pkg.junkFiles++;
				continue;
			}
			QString ext = Ext(e.name);
			bool nested = (ext == QLatin1String("overlay") || ext == QLatin1String("zip"));
			if (nested && depth < 2 && int(pkg.archives->readers.size()) <= kMaxNested) {
				QByteArray bytes;
				QString err;
				if (!zip->Read(e, bytes, kMaxNestedArchive, err)) {
					pkg.Add(Pkg::Severity::Warning, QStringLiteral("Couldn't open %1: %2").arg(BaseName(e.name), err));
					continue;
				}
				std::unique_ptr<ZipReader> inner = ZipReader::OpenBytes(std::move(bytes), err);
				if (!inner) {
					continue; /* not actually a zip: ignore */
				}
				pkg.archives->readers.push_back(std::move(inner));
				pkg.archives->names.push_back(BaseName(e.name));
				int idx = int(pkg.archives->readers.size()) - 1;
				if (ext == QLatin1String("overlay")) {
					overlayArchives.push_back(idx);
				}
				Index(idx, depth + 1);
				continue;
			}
			AddFile(archive, e);
			if (ext == QLatin1String("json") && depth == 0) {
				obsCollections.emplace_back(archive, e.name);
			}
		}
	}

	bool ReadJson(int archive, const QString &entry, QJsonObject &out)
	{
		ZipReader *zip = pkg.archives->readers[size_t(archive)].get();
		const ZipReader::Entry *e = zip->Find(entry);
		QByteArray bytes;
		QString err;
		if (!e || !zip->Read(*e, bytes, kMaxConfig, err)) {
			return false;
		}
		QJsonParseError perr;
		QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
		if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
			return false;
		}
		out = doc.object();
		return true;
	}

	/* A path from someone else's PC -> a file in this bundle */
	int Repair(const QString &original, const QString &usedBy)
	{
		if (original.isEmpty()) {
			return -1;
		}
		QString base = BaseName(original).toLower();
		QList<int> candidates = fileByBase.values(base);
		if (candidates.isEmpty()) {
			Once(Pkg::Severity::Problem,
			     QStringLiteral("Missing file \"%1\" (used by %2)").arg(BaseName(original), usedBy));
			return -1;
		}
		pkg.repairedPaths++;
		if (candidates.size() == 1) {
			return candidates.first();
		}
		/* Same name in several places: prefer the folder the path came
		 * from (e.g. "...\Pirate Hunter Installation\9.webm" is inside
		 * "Pirate Hunter Installation.overlay"), then identical sizes */
		QString p = original;
		p.replace('\\', '/');
		QString parent = p.section('/', -2, -2).toLower();
		for (int c : candidates) {
			const Pkg::File &f = pkg.files[size_t(c)];
			QString arch = pkg.archives->names.value(f.archive).toLower();
			if (!parent.isEmpty() && (QFileInfo(arch).completeBaseName() == parent ||
						  f.entry.toLower().contains(parent + '/'))) {
				return c;
			}
		}
		quint64 size = pkg.files[size_t(candidates.first())].size;
		bool allSame = std::all_of(candidates.begin(), candidates.end(),
					   [&](int c) { return pkg.files[size_t(c)].size == size; });
		if (!allSame) {
			Once(Pkg::Severity::Warning,
			     QStringLiteral("Several files are called \"%1\"; picked one, check %2").arg(BaseName(original),
												   usedBy));
		}
		return candidates.first();
	}

	void NoteFont(const QJsonObject &settings)
	{
		QString face = settings.value(QStringLiteral("font")).toObject().value(QStringLiteral("face")).toString();
		if (!face.isEmpty() && !pkg.fontsUsed.contains(face)) {
			pkg.fontsUsed.push_back(face);
		}
	}

	bool ScanStreamlabs(int archive);
	bool ScanObs(int archive, const QString &entry, const QJsonObject &root);
	void ScanLoose();
	void ScanAlerts();
};

/* ------------------------------------------------------------------------ */
/* 1. Streamlabs .overlay                                                   */

bool Scanner::ScanStreamlabs(int archive)
{
	QJsonObject root;
	if (!ReadJson(archive, QStringLiteral("config.json"), root)) {
		return false;
	}
	const QJsonArray scenes = root.value(QStringLiteral("scenes")).toObject().value(QStringLiteral("items")).toArray();
	if (scenes.isEmpty()) {
		return false;
	}
	ZipReader *zip = pkg.archives->readers[size_t(archive)].get();
	const double W = pkg.canvasW, H = pkg.canvasH;

	auto fileFor = [&](const QString &name, const QString &usedBy) {
		int f = FileInArchive(archive, name);
		if (f < 0) {
			Once(Pkg::Severity::Problem, QStringLiteral("Missing file \"%1\" (used by %2)").arg(name, usedBy));
		}
		return f;
	};

	int widgets = 0, verticalItems = 0;

	for (const QJsonValue &sv : scenes) {
		QJsonObject so = sv.toObject();
		Pkg::Scene scene;
		scene.key = so.value(QStringLiteral("sceneId")).toString();
		scene.name = so.value(QStringLiteral("name")).toString();

		const QJsonArray slotItems =
			so.value(QStringLiteral("slots")).toObject().value(QStringLiteral("items")).toArray();
		for (const QJsonValue &iv : slotItems) {
			QJsonObject io = iv.toObject();
			if (io.value(QStringLiteral("sceneNodeType")).toString() != QLatin1String("item")) {
				continue; /* folders only group items; they don't draw */
			}
			if (io.value(QStringLiteral("display")).toString() == QLatin1String("vertical")) {
				verticalItems++;
				continue;
			}
			QJsonObject c = io.value(QStringLiteral("content")).toObject();
			QString type = c.value(QStringLiteral("nodeType")).toString();

			Pkg::Item it;
			it.name = io.value(QStringLiteral("name")).toString();
			it.x = io.value(QStringLiteral("x")).toDouble() * W;
			it.y = io.value(QStringLiteral("y")).toDouble() * H;
			/* Streamlabs stores scale divided by the canvas size */
			it.scaleX = io.value(QStringLiteral("scaleX")).toDouble() * W;
			it.scaleY = io.value(QStringLiteral("scaleY")).toDouble() * H;
			it.rot = io.value(QStringLiteral("rotation")).toDouble();
			it.visible = io.value(QStringLiteral("visible")).toBool(true);
			QJsonObject crop = io.value(QStringLiteral("crop")).toObject();
			it.cropL = crop.value(QStringLiteral("left")).toInt();
			it.cropT = crop.value(QStringLiteral("top")).toInt();
			it.cropR = crop.value(QStringLiteral("right")).toInt();
			it.cropB = crop.value(QStringLiteral("bottom")).toInt();
			it.filters = io.value(QStringLiteral("filters")).toArray();

			if (type == QLatin1String("VideoNode")) {
				it.kind = Pkg::Kind::Video;
				it.settings = c.value(QStringLiteral("settings")).toObject();
				it.file = fileFor(c.value(QStringLiteral("filename")).toString(), it.name);
			} else if (type == QLatin1String("ImageNode")) {
				it.kind = Pkg::Kind::Image;
				it.settings = c.value(QStringLiteral("settings")).toObject();
				it.file = fileFor(c.value(QStringLiteral("filename")).toString(), it.name);
			} else if (type == QLatin1String("IconLibraryNode")) {
				/* A folder of icons the streamer picks from; start with
				 * the first one, like Streamlabs does */
				it.kind = Pkg::Kind::Image;
				QString folder = c.value(QStringLiteral("folder")).toString() + '/';
				QString active = c.value(QStringLiteral("activeIcon")).toString();
				QStringList icons;
				for (const ZipReader::Entry &e : zip->Entries()) {
					if (!e.isDir && e.name.startsWith(folder) && kImageExt.contains(Ext(e.name))) {
						icons.push_back(e.name);
					}
				}
				std::sort(icons.begin(), icons.end(), [](const QString &a, const QString &b) {
					return a.compare(b, Qt::CaseInsensitive) < 0;
				});
				QString pick = icons.isEmpty() ? QString() : icons.first();
				for (const QString &i : icons) {
					if (!active.isEmpty() && BaseName(i) == BaseName(active)) {
						pick = i;
					}
				}
				it.file = pick.isEmpty() ? -1 : FileInArchive(archive, pick);
				if (it.file < 0) {
					Once(Pkg::Severity::Warning, QStringLiteral("Icon folder for \"%1\" is empty").arg(it.name));
				}
			} else if (type == QLatin1String("TextNode")) {
				it.kind = Pkg::Kind::Text;
				it.settings = c.value(QStringLiteral("settings")).toObject();
				NoteFont(it.settings);
			} else if (type == QLatin1String("StreamlabelNode")) {
				it.kind = Pkg::Kind::Label;
				it.labelType = c.value(QStringLiteral("labelType")).toString();
				it.settings = c.value(QStringLiteral("textSource"))
						      .toObject()
						      .value(QStringLiteral("settings"))
						      .toObject();
				NoteFont(it.settings);
			} else if (type == QLatin1String("GameCaptureNode")) {
				it.kind = Pkg::Kind::GameCapture;
				it.settings.insert(QStringLiteral("width"), c.value(QStringLiteral("width")));
				it.settings.insert(QStringLiteral("height"), c.value(QStringLiteral("height")));
			} else if (type == QLatin1String("WebcamNode")) {
				it.kind = Pkg::Kind::Webcam;
			} else if (type == QLatin1String("SceneSourceNode")) {
				it.kind = Pkg::Kind::SceneRef;
				it.sceneRef = c.value(QStringLiteral("sceneId")).toString();
			} else if (type == QLatin1String("WidgetNode")) {
				widgets++; /* alert box / chat box / goals: Streamlabs web widgets */
				continue;
			} else {
				Once(Pkg::Severity::Warning, QStringLiteral("Skipped \"%1\" (%2 isn't supported yet)").arg(it.name, type));
				continue;
			}
			scene.items.push_back(std::move(it));
		}
		pkg.scenes.push_back(std::move(scene));
	}

	if (widgets > 0) {
		pkg.Add(Pkg::Severity::Info,
			QStringLiteral("Skipped the creator's Streamlabs widgets (alert box, chat box): they're tied to their "
				       "account. Alerts use the bundle's alert videos instead."));
	}
	if (verticalItems > 0) {
		pkg.Add(Pkg::Severity::Warning,
			QStringLiteral("%1 items made for Streamlabs' vertical layout were skipped").arg(verticalItems));
	}

	QJsonObject tr = root.value(QStringLiteral("transition")).toObject();
	if (!tr.isEmpty()) {
		pkg.transition.id = tr.value(QStringLiteral("type")).toString();
		pkg.transition.settings = tr.value(QStringLiteral("settings")).toObject();
		pkg.transition.durationMs = tr.value(QStringLiteral("duration")).toInt(300);
		pkg.transition.name = QStringLiteral("%1 Transition").arg(pkg.title);
		QString path = pkg.transition.settings.value(QStringLiteral("path")).toString();
		if (pkg.transition.id == QLatin1String("obs_stinger_transition")) {
			pkg.transition.file = fileFor(BaseName(path), QStringLiteral("the transition"));
			pkg.transition.present = pkg.transition.file >= 0;
		} else if (!pkg.transition.id.isEmpty()) {
			pkg.transition.present = true; /* fade, swipe...: no file */
		}
	}

	pkg.format = QStringLiteral("Streamlabs overlay (%1)").arg(pkg.archives->names.value(archive));
	return true;
}

/* ------------------------------------------------------------------------ */
/* 2. OBS scene collection                                                  */

bool Scanner::ScanObs(int archive, const QString &entry, const QJsonObject &root)
{
	const QJsonArray sources = root.value(QStringLiteral("sources")).toArray();
	QHash<QString, QJsonObject> byName, byUuid;
	for (const QJsonValue &v : sources) {
		QJsonObject s = v.toObject();
		byName.insert(s.value(QStringLiteral("name")).toString(), s);
		QString uuid = s.value(QStringLiteral("uuid")).toString();
		if (!uuid.isEmpty()) {
			byUuid.insert(uuid, s);
		}
	}

	QStringList order;
	for (const QJsonValue &v : root.value(QStringLiteral("scene_order")).toArray()) {
		order.push_back(v.toObject().value(QStringLiteral("name")).toString());
	}
	for (const QJsonValue &v : sources) {
		QJsonObject s = v.toObject();
		if (s.value(QStringLiteral("id")).toString() == QLatin1String("scene") &&
		    !order.contains(s.value(QStringLiteral("name")).toString())) {
			order.push_back(s.value(QStringLiteral("name")).toString());
		}
	}

	auto isCapture = [](const QString &id) {
		static const QStringList ids = {"game_capture", "window_capture", "monitor_capture", "xcomposite_input",
						"xshm_input", "pipewire-screen-capture-source", "pipewire-window-capture-source",
						"pipewire-desktop-capture-source", "screen_capture", "display_capture"};
		return ids.contains(id);
	};
	auto isCamera = [](const QString &id) {
		static const QStringList ids = {"dshow_input", "v4l2_input", "av_capture_input", "macos-avcapture",
						"pipewire-camera-source"};
		return ids.contains(id);
	};

	bool sawBrowser = false;
	bool sawWidgetToken = false;
	bool scaleRefSet = false;

	for (const QString &sceneName : order) {
		QJsonObject src = byName.value(sceneName);
		if (src.isEmpty()) {
			continue;
		}
		Pkg::Scene scene;
		scene.key = sceneName;
		scene.name = sceneName;
		const QJsonArray items =
			src.value(QStringLiteral("settings")).toObject().value(QStringLiteral("items")).toArray();
		for (const QJsonValue &iv : items) {
			QJsonObject io = iv.toObject();
			QJsonObject s = byUuid.value(io.value(QStringLiteral("source_uuid")).toString());
			if (s.isEmpty()) {
				s = byName.value(io.value(QStringLiteral("name")).toString());
			}
			if (s.isEmpty()) {
				continue;
			}
			QString id = s.value(QStringLiteral("id")).toString();
			QJsonObject settings = s.value(QStringLiteral("settings")).toObject();

			Pkg::Item it;
			it.name = s.value(QStringLiteral("name")).toString();
			it.filters = s.value(QStringLiteral("filters")).toArray();
			QJsonObject pos = io.value(QStringLiteral("pos")).toObject();
			QJsonObject scale = io.value(QStringLiteral("scale")).toObject();
			QJsonObject bounds = io.value(QStringLiteral("bounds")).toObject();
			it.x = pos.value(QStringLiteral("x")).toDouble();
			it.y = pos.value(QStringLiteral("y")).toDouble();
			it.scaleX = scale.value(QStringLiteral("x")).toDouble(1.0);
			it.scaleY = scale.value(QStringLiteral("y")).toDouble(1.0);
			it.rot = io.value(QStringLiteral("rot")).toDouble();
			it.align = io.value(QStringLiteral("align")).toInt(5);
			it.boundsType = io.value(QStringLiteral("bounds_type")).toInt();
			it.boundsAlign = io.value(QStringLiteral("bounds_align")).toInt();
			it.boundsX = bounds.value(QStringLiteral("x")).toDouble();
			it.boundsY = bounds.value(QStringLiteral("y")).toDouble();
			it.cropL = io.value(QStringLiteral("crop_left")).toInt();
			it.cropT = io.value(QStringLiteral("crop_top")).toInt();
			it.cropR = io.value(QStringLiteral("crop_right")).toInt();
			it.cropB = io.value(QStringLiteral("crop_bottom")).toInt();
			it.visible = io.value(QStringLiteral("visible")).toBool(true);
			QJsonObject ref = io.value(QStringLiteral("scale_ref")).toObject();
			if (!scaleRefSet && ref.value(QStringLiteral("y")).toDouble() > 0) {
				pkg.canvasW = int(ref.value(QStringLiteral("x")).toDouble());
				pkg.canvasH = int(ref.value(QStringLiteral("y")).toDouble());
				scaleRefSet = true;
			}

			if (id == QLatin1String("ffmpeg_source")) {
				it.kind = Pkg::Kind::Video;
				it.settings = settings;
				if (settings.value(QStringLiteral("is_local_file")).toBool(true)) {
					it.file = Repair(settings.value(QStringLiteral("local_file")).toString(), it.name);
				} else {
					it.kind = Pkg::Kind::Other;
					it.otherId = id;
				}
			} else if (id == QLatin1String("image_source")) {
				it.kind = Pkg::Kind::Image;
				it.settings = settings;
				it.file = Repair(settings.value(QStringLiteral("file")).toString(), it.name);
			} else if (id.startsWith(QLatin1String("text_gdiplus")) || id.startsWith(QLatin1String("text_ft2"))) {
				it.kind = Pkg::Kind::Text;
				it.settings = settings;
				if (id.startsWith(QLatin1String("text_ft2"))) {
					/* Normalise to GDI+ names; the installer converts back */
					it.settings.insert(QStringLiteral("color"),
							   settings.value(QStringLiteral("color1")).toInteger(0xFFFFFFFF));
				}
				NoteFont(it.settings);
			} else if (isCapture(id)) {
				it.kind = Pkg::Kind::GameCapture;
			} else if (isCamera(id)) {
				it.kind = Pkg::Kind::Webcam;
			} else if (id == QLatin1String("scene")) {
				it.kind = Pkg::Kind::SceneRef;
				it.sceneRef = it.name;
			} else if (id == QLatin1String("browser_source")) {
				sawBrowser = true;
				if (settings.value(QStringLiteral("url")).toString().contains(QLatin1String("streamlabs.com"))) {
					sawWidgetToken = true;
				}
				continue;
			} else if (id == QLatin1String("group")) {
				Once(Pkg::Severity::Warning, QStringLiteral("Groups aren't imported yet (skipped \"%1\")").arg(it.name));
				continue;
			} else {
				it.kind = Pkg::Kind::Other;
				it.otherId = id;
				it.settings = settings;
			}
			scene.items.push_back(std::move(it));
		}
		pkg.scenes.push_back(std::move(scene));
	}

	if (sawWidgetToken) {
		pkg.Add(Pkg::Severity::Info,
			QStringLiteral("Removed the creator's Streamlabs widget links (alert box, chat box): they would have "
				       "shown the creator's alerts and chat, not yours."));
	} else if (sawBrowser) {
		pkg.Add(Pkg::Severity::Warning, QStringLiteral("Browser sources aren't imported yet"));
	}

	/* Transition: the one the collection uses */
	QString current = root.value(QStringLiteral("current_transition")).toString();
	for (const QJsonValue &tv : root.value(QStringLiteral("transitions")).toArray()) {
		QJsonObject t = tv.toObject();
		if (!current.isEmpty() && t.value(QStringLiteral("name")).toString() != current) {
			continue;
		}
		pkg.transition.id = t.value(QStringLiteral("id")).toString();
		pkg.transition.settings = t.value(QStringLiteral("settings")).toObject();
		pkg.transition.name = QStringLiteral("%1 Transition").arg(pkg.title);
		pkg.transition.durationMs = root.value(QStringLiteral("transition_duration")).toInt(300);
		if (pkg.transition.id == QLatin1String("obs_stinger_transition")) {
			pkg.transition.file =
				Repair(pkg.transition.settings.value(QStringLiteral("path")).toString(), QStringLiteral("the transition"));
			pkg.transition.present = pkg.transition.file >= 0;
		} else {
			pkg.transition.present = !pkg.transition.id.isEmpty();
		}
		break;
	}

	pkg.format = QStringLiteral("OBS scene collection (%1)").arg(BaseName(entry));
	(void)archive;
	return !pkg.scenes.empty();
}

/* ------------------------------------------------------------------------ */
/* 3. Loose files                                                           */

void Scanner::ScanLoose()
{
	struct Guess {
		const char *pattern;
		const char *scene;
	};
	/* In the order a stream uses them */
	static const Guess guesses[] = {
		{"start", "Starting Soon"},
		{"brb|be right back|right back", "Be Right Back"},
		{"intermission|pause|break", "Intermission"},
		{"ending|end screen|outro|stream end", "Stream Ending"},
		{"offline", "Offline"},
	};
	constexpr int kGuesses = int(sizeof(guesses) / sizeof(guesses[0]));

	std::vector<std::vector<int>> picked(kGuesses); /* guess -> files */
	int facecam = -1, stinger = -1;
	for (size_t i = 0; i < pkg.files.size(); i++) {
		const Pkg::File &f = pkg.files[i];
		if (f.archive != 0 || !IsMedia(f.fileName) || !AlertEvent(f.entry).isEmpty()) {
			continue; /* outer bundle only: nested copies are duplicates */
		}
		QString n = QFileInfo(f.fileName).completeBaseName().toLower();
		if (n.contains(QRegularExpression(QStringLiteral("transition|stinger")))) {
			stinger = int(i);
			continue;
		}
		if (n.contains(QRegularExpression(QStringLiteral("face ?cam|webcam|cam ?frame|camera")))) {
			facecam = int(i);
			continue;
		}
		for (int g = 0; g < kGuesses; g++) {
			if (n.contains(QRegularExpression(QString::fromLatin1(guesses[g].pattern)))) {
				picked[size_t(g)].push_back(int(i));
				break;
			}
		}
	}

	auto fullScreen = [&](int file, const QString &name) {
		Pkg::Item it;
		it.name = name;
		it.kind = kVideoExt.contains(Ext(pkg.files[size_t(file)].fileName)) ? Pkg::Kind::Video : Pkg::Kind::Image;
		it.file = file;
		it.settings.insert(QStringLiteral("looping"), true);
		/* Fit whatever size it is to the canvas */
		it.boundsType = 2; /* OBS_BOUNDS_SCALE_INNER */
		it.boundsAlign = 0;
		it.boundsX = pkg.canvasW;
		it.boundsY = pkg.canvasH;
		return it;
	};

	/* Live scene: game + webcam + the bundle's cam frame */
	{
		Pkg::Scene live;
		live.key = live.name = QStringLiteral("Live Scene");
		Pkg::Item game;
		game.name = QStringLiteral("Game Capture");
		game.kind = Pkg::Kind::GameCapture;
		live.items.push_back(game);
		Pkg::Item cam;
		cam.name = QStringLiteral("Webcam");
		cam.kind = Pkg::Kind::Webcam;
		live.items.push_back(cam);
		if (facecam >= 0) {
			live.items.push_back(fullScreen(facecam, QFileInfo(pkg.files[size_t(facecam)].fileName).completeBaseName()));
			pkg.Add(Pkg::Severity::Info, QStringLiteral("Line your webcam up with the cam frame in \"Live Scene\""));
		}
		pkg.scenes.push_back(std::move(live));
	}

	for (int g = 0; g < kGuesses; g++) {
		const QString sceneName = QString::fromLatin1(guesses[g].scene);
		std::vector<int> &files = picked[size_t(g)];
		std::sort(files.begin(), files.end(), [&](int a, int b) {
			return pkg.files[size_t(a)].fileName.compare(pkg.files[size_t(b)].fileName, Qt::CaseInsensitive) < 0;
		});
		for (size_t k = 0; k < files.size(); k++) {
			Pkg::Scene sc;
			sc.name = files.size() > 1 ? QFileInfo(pkg.files[size_t(files[k])].fileName).completeBaseName() : sceneName;
			sc.key = sc.name;
			if (sceneName == QLatin1String("Intermission") || sceneName == QLatin1String("Be Right Back")) {
				/* These usually frame the game */
				Pkg::Item game;
				game.name = QStringLiteral("Game Capture");
				game.kind = Pkg::Kind::GameCapture;
				game.visible = false;
				sc.items.push_back(game);
			}
			sc.items.push_back(fullScreen(files[k], sc.name));
			pkg.scenes.push_back(std::move(sc));
		}
	}

	if (stinger >= 0) {
		pkg.transition.present = true;
		pkg.transition.id = QStringLiteral("obs_stinger_transition");
		pkg.transition.name = QStringLiteral("%1 Transition").arg(pkg.title);
		pkg.transition.file = stinger;
		pkg.transition.durationMs = 300;
		/* No config: cut halfway through a typical 2 s stinger */
		pkg.transition.settings.insert(QStringLiteral("tp_type"), 0);
		pkg.transition.settings.insert(QStringLiteral("transition_point"), 1000);
		pkg.Add(Pkg::Severity::Warning,
			QStringLiteral("The bundle doesn't say when the transition switches scenes; set to 1 second"));
	}

	pkg.format = QStringLiteral("file and folder names (no setup file in the bundle)");
	pkg.Add(Pkg::Severity::Warning,
		QStringLiteral("No setup file found, so scenes were built from file names. Check the layout."));
}

/* ------------------------------------------------------------------------ */
/* Alerts and fonts (all formats)                                           */

void Scanner::ScanAlerts()
{
	QSet<QString> seen;
	for (size_t i = 0; i < pkg.files.size(); i++) {
		const Pkg::File &f = pkg.files[i];
		if (!IsMedia(f.fileName)) {
			if (kFontExt.contains(Ext(f.fileName))) {
				pkg.fontFiles.push_back(int(i));
			}
			continue;
		}
		QString ev = AlertEvent(f.entry);
		if (ev.isEmpty() || seen.contains(f.fileName.toLower())) {
			continue;
		}
		seen.insert(f.fileName.toLower());
		pkg.alerts.push_back({ev, int(i)});
	}
	std::stable_sort(pkg.alerts.begin(), pkg.alerts.end(),
			 [](const Pkg::Alert &a, const Pkg::Alert &b) { return a.event < b.event; });
}

} // namespace

/* ------------------------------------------------------------------------ */

QString AssetImport::AlertEventTitle(const QString &event)
{
	static const QHash<QString, QString> titles = {
		{"follow", "Follow"},
		{"subscribe", "Subscription"},
		{"resubscribe", "Resubscription"},
		{"gift_sub", "Gifted sub"},
		{"membership", "Membership (YouTube)"},
		{"bits", "Bits / cheer"},
		{"donation", "Donation / tip"},
		{"raid", "Raid"},
		{"host", "Host"},
		{"superchat", "Super Chat (YouTube)"},
		{"stars", "Stars (Facebook)"},
		{"fb_support", "Supporter (Facebook)"},
		{"fb_like", "Like (Facebook)"},
		{"fb_share", "Share (Facebook)"},
		{"merch", "Merch"},
		{"charity", "Charity"},
		{"rant", "Rant (Rumble)"},
	};
	return titles.value(event, event);
}

QString AssetImport::LabelText(const QString &labelType)
{
	static const QHash<QString, QString> texts = {
		{"most_recent_follower", "Latest follower"},
		{"most_recent_subscriber", "Latest subscriber"},
		{"most_recent_resubscriber", "Latest resub"},
		{"most_recent_donator", "Latest donation"},
		{"most_recent_cheerer", "Latest cheer"},
		{"most_recent_youtube_subscriber", "Latest subscriber"},
		{"most_recent_youtube_sponsor", "Latest member"},
	};
	QString t = texts.value(labelType);
	if (t.isEmpty()) {
		t = labelType;
		t.replace('_', ' ');
	}
	return t;
}

bool AssetImport::Scan(const QString &zipPath, AssetPackage &pkg, QString &error)
{
	pkg = AssetPackage();
	pkg.archives = std::make_shared<AssetPackage::Archives>();
	pkg.archives->path = zipPath;
	pkg.title = CleanTitle(zipPath);

	std::unique_ptr<ZipReader> outer = ZipReader::OpenFile(zipPath, error);
	if (!outer) {
		return false;
	}
	pkg.archives->readers.push_back(std::move(outer));
	pkg.archives->names.push_back(QFileInfo(zipPath).fileName());

	Scanner s(pkg);
	s.Index(0, 0);

	/* Most bundles have one top folder named after the product: nicer
	 * than a download name like "Pirate_Hunter_Stream_Package (1).zip" */
	QSet<QString> roots;
	for (const ZipReader::Entry &e : pkg.archives->readers[0]->Entries()) {
		if (!IsJunk(e.name)) {
			roots.insert(e.name.section('/', 0, 0));
		}
	}
	if (roots.size() == 1) {
		QString root = *roots.begin();
		bool isFolder = false;
		for (const ZipReader::Entry &e : pkg.archives->readers[0]->Entries()) {
			isFolder = isFolder || e.name.startsWith(root + '/');
		}
		if (isFolder && !root.trimmed().isEmpty()) {
			pkg.title = root.trimmed();
		}
	}

	bool done = false;
	for (int a : s.overlayArchives) {
		if (s.ScanStreamlabs(a)) {
			done = true;
			break;
		}
	}
	if (!done) {
		for (auto &[archive, entry] : s.obsCollections) {
			QJsonObject root;
			if (s.ReadJson(archive, entry, root) && root.contains(QStringLiteral("sources")) &&
			    root.contains(QStringLiteral("scene_order")) && s.ScanObs(archive, entry, root)) {
				done = true;
				break;
			}
		}
	}
	if (!done) {
		s.ScanLoose();
	}
	s.ScanAlerts();

	if (pkg.alerts.empty()) {
		pkg.Add(AssetPackage::Severity::Info, QStringLiteral("No alert animations found in this bundle"));
	}
	bool labels = false;
	for (const AssetPackage::Scene &sc : pkg.scenes) {
		for (const AssetPackage::Item &it : sc.items) {
			labels = labels || it.kind == AssetPackage::Kind::Label;
		}
	}
	if (labels) {
		pkg.Add(AssetPackage::Severity::Info,
			QStringLiteral("\"Latest follower/subscriber\" texts show a placeholder for now"));
	}
	if (!pkg.transition.present) {
		pkg.Add(AssetPackage::Severity::Info, QStringLiteral("No scene transition in this bundle"));
	}
	return true;
}
