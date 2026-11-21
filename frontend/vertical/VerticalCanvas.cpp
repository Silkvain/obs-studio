/******************************************************************************
    Siegfried: Vertical (9:16) canvas. See VerticalCanvas.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "VerticalCanvas.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <graphics/matrix4.h>
#include <util/config-file.h>

#include <QTimer>

#include <algorithm>

#include "moc_VerticalCanvas.cpp"

namespace {
constexpr const char *kSection = "Vertical";

uint32_t Even(double v)
{
	uint32_t n = uint32_t(v + 0.5);
	return std::max(2u, n + (n & 1u));
}
} // namespace

VerticalCanvas::VerticalCanvas(OBSBasic *main_) : QObject(main_), main(main_)
{
	config_t *cfg = App()->GetUserConfig();
	config_set_default_bool(cfg, kSection, "Linked", true);
	config_set_default_bool(cfg, kSection, "SafeZones", true);
	linked = config_get_bool(cfg, kSection, "Linked");
	safeZones = config_get_bool(cfg, kSection, "SafeZones");

	saveTimer = new QTimer(this);
	saveTimer->setSingleShot(true);
	saveTimer->setInterval(1000);
	connect(saveTimer, &QTimer::timeout, this, [this] {
		if (!shuttingDown) {
			main->SaveProject();
		}
	});

	obs_frontend_add_event_callback(FrontendEvent, this);
	videoResetSig.Connect(obs_get_signal_handler(), "video_reset", VideoResetSignal, this);

	/* Twitch Dual Format (Enhanced Broadcasting "Additional canvas") encodes
	 * this canvas, so it needs the video mix while the main stream runs. */
	connect(main, &OBSBasic::StreamingPreparing, this, [this] {
		config_t *pc = main->Config();
		const char *extra = config_get_string(pc, "Stream1", "MultitrackExtraCanvas");
		bool multitrack = config_get_bool(pc, "Stream1", "EnableMultitrackVideo");
		if (!twitchHold && multitrack && extra && *extra && Uuid() == QString::fromUtf8(extra)) {
			twitchHold = AcquireVideo() != nullptr;
		}
	});
	connect(main, &OBSBasic::StreamingStopped, this, [this] {
		if (twitchHold) {
			twitchHold = false;
			ReleaseVideo();
		}
	});

	/* In case the collection is already loaded */
	EnsureCanvas();
}

VerticalCanvas::~VerticalCanvas()
{
	Shutdown();
}

void VerticalCanvas::Shutdown()
{
	if (shuttingDown) {
		return;
	}
	shuttingDown = true;
	obs_frontend_remove_event_callback(FrontendEvent, this);
	videoResetSig.Disconnect();
	sourceAddSig.Disconnect();
	sourceRemoveSig.Disconnect();
	sourceRenameSig.Disconnect();
	heldCanvas = nullptr;
	std::lock_guard lock(canvasMutex);
	weakCanvas = nullptr;
}

/* ------------------------------------------------------------------------ */
/* Canvas                                                                   */

OBSCanvasAutoRelease VerticalCanvas::GetCanvas() const
{
	std::lock_guard lock(canvasMutex);
	return weakCanvas ? obs_weak_canvas_get_canvas(weakCanvas) : nullptr;
}

bool VerticalCanvas::GetSize(uint32_t &cx, uint32_t &cy) const
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	return canvas && obs_canvas_get_base_size(canvas, &cx, &cy);
}

QString VerticalCanvas::Uuid() const
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	return canvas ? QString::fromUtf8(obs_canvas_get_uuid(canvas)) : QString();
}

bool VerticalCanvas::ComputeVideoInfo(obs_video_info &ovi) const
{
	obs_video_info m = {};
	if (!obs_get_video_info(&m) || !m.base_height || !m.output_height) {
		return false;
	}
	/* 9:16 at the main canvas's height: 1080p -> 1080x1920 */
	ovi = m;
	ovi.base_width = Even(m.base_height);
	ovi.base_height = Even(double(m.base_height) * 16.0 / 9.0);
	/* Encoded size follows the main output's downscale, if any */
	ovi.output_width = Even(m.output_height);
	ovi.output_height = Even(double(m.output_height) * 16.0 / 9.0);
	return true;
}

void VerticalCanvas::EnsureCanvas()
{
	if (shuttingDown) {
		return;
	}

	obs_canvas_t *found = nullptr;
	for (const OBS::Canvas &c : main->GetCanvases()) {
		const char *name = obs_canvas_get_name(c);
		if (name && strcmp(name, kCanvasName) == 0) {
			found = c;
			break;
		}
	}

	bool created = false;
	if (!found) {
		/* No video info yet: a canvas without a mix costs nothing */
		found = main->AddCanvas(kCanvasName, nullptr, ACTIVATE | SCENE_REF);
		created = true;
	}
	if (!found) {
		blog(LOG_WARNING, "[vertical] Could not create the vertical canvas");
		return;
	}

	{
		std::lock_guard lock(canvasMutex);
		weakCanvas = OBSGetWeakRef(found);
	}
	ConnectCanvasSignals();
	UpdateVideoInfo();

	std::vector<OBSSource> scenes = Scenes();
	if (scenes.empty()) {
		OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
		QString base = mainScene ? QString::fromUtf8(obs_source_get_name(mainScene)) : QStringLiteral("Scene");
		CreateScene(base);
		scenes = Scenes();
	}

	/* Pick the scene to show: linked one, then the last used one */
	OBSSource pick;
	if (linked) {
		OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
		if (mainScene) {
			pick = obs_canvas_get_source_by_name(found, obs_source_get_name(mainScene));
			obs_source_release(pick); /* get_source_by_name adds a ref */
		}
	}
	if (!pick) {
		const char *last = config_get_string(App()->GetUserConfig(), kSection, "LastScene");
		for (const OBSSource &s : scenes) {
			if (last && strcmp(obs_source_get_name(s), last) == 0) {
				pick = s;
			}
		}
	}
	if (!pick && !scenes.empty()) {
		pick = scenes.front();
	}
	SetChannel(pick);

	if (created) {
		blog(LOG_INFO, "[vertical] Created vertical canvas");
		RequestSave();
	}
	emit CanvasChanged();
	emit ScenesChanged();
	emit CurrentSceneChanged();
}

void VerticalCanvas::DropCanvas()
{
	sourceAddSig.Disconnect();
	sourceRemoveSig.Disconnect();
	sourceRenameSig.Disconnect();
	{
		std::lock_guard lock(canvasMutex);
		weakCanvas = nullptr;
	}
	emit CanvasChanged();
	emit ScenesChanged();
	emit CurrentSceneChanged();
}

void VerticalCanvas::ConnectCanvasSignals()
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	if (!canvas) {
		return;
	}
	signal_handler_t *sh = obs_canvas_get_signal_handler(canvas);
	sourceAddSig.Connect(sh, "source_add", CanvasSourceSignal, this);
	sourceRemoveSig.Connect(sh, "source_remove", CanvasSourceSignal, this);
	sourceRenameSig.Connect(sh, "source_rename", CanvasSourceSignal, this);
}

void VerticalCanvas::CanvasSourceSignal(void *data, calldata_t *)
{
	auto *self = static_cast<VerticalCanvas *>(data);
	QMetaObject::invokeMethod(
		self,
		[self] {
			emit self->ScenesChanged();
			self->RequestSave();
		},
		Qt::QueuedConnection);
}

void VerticalCanvas::UpdateVideoInfo()
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	obs_video_info ovi;
	if (!canvas || !ComputeVideoInfo(ovi)) {
		return;
	}
	if (obs_canvas_video_active(canvas)) {
		return; /* Settings can't change while it's encoding anyway */
	}
	if (obs_canvas_has_video(canvas)) {
		obs_canvas_reset_video(canvas, &ovi);
	} else {
		obs_canvas_set_idle_video_info(canvas, &ovi);
	}
	emit SettingsChanged();
}

void VerticalCanvas::VideoResetSignal(void *data, calldata_t *)
{
	auto *self = static_cast<VerticalCanvas *>(data);
	QMetaObject::invokeMethod(self, &VerticalCanvas::UpdateVideoInfo, Qt::QueuedConnection);
}

void VerticalCanvas::FrontendEvent(obs_frontend_event event, void *data)
{
	auto *self = static_cast<VerticalCanvas *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		self->EnsureCanvas();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		self->DropCanvas();
		break;
	case OBS_FRONTEND_EVENT_CANVAS_REMOVED: {
		OBSCanvasAutoRelease c = self->GetCanvas();
		if (!c || obs_canvas_removed(c)) {
			self->DropCanvas();
		}
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		self->FollowMainScene();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		self->Shutdown();
		break;
	default:
		break;
	}
}

void VerticalCanvas::RequestSave()
{
	if (!shuttingDown && saveTimer) {
		saveTimer->start();
	}
}

/* ------------------------------------------------------------------------ */
/* Scenes                                                                   */

std::vector<OBSSource> VerticalCanvas::Scenes() const
{
	std::vector<OBSSource> result;
	OBSCanvasAutoRelease canvas = GetCanvas();
	if (!canvas) {
		return result;
	}
	auto cb = [](void *param, obs_source_t *source) {
		if (!obs_source_is_group(source)) {
			static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
		}
		return true;
	};
	obs_canvas_enum_scenes(canvas, cb, &result);
	return result;
}

OBSSource VerticalCanvas::CurrentScene() const
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	if (!canvas) {
		return nullptr;
	}
	OBSSourceAutoRelease s = obs_canvas_get_channel(canvas, 0);
	return OBSSource(s.Get());
}

void VerticalCanvas::SetChannel(obs_source_t *scene)
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	if (!canvas) {
		return;
	}
	obs_canvas_set_channel(canvas, 0, scene);
	if (scene) {
		config_set_string(App()->GetUserConfig(), kSection, "LastScene", obs_source_get_name(scene));
	}
	RequestSave();
}

void VerticalCanvas::SetCurrentScene(obs_source_t *scene)
{
	OBSSource cur = CurrentScene();
	if (cur.Get() == scene) {
		return;
	}
	SetChannel(scene);
	emit CurrentSceneChanged();
}

QString VerticalCanvas::UniqueSceneName(const QString &base) const
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	if (!canvas) {
		return base;
	}
	QString name = base;
	for (int i = 2;; i++) {
		OBSSourceAutoRelease existing = obs_canvas_get_source_by_name(canvas, name.toUtf8().constData());
		if (!existing) {
			return name;
		}
		name = QStringLiteral("%1 %2").arg(base).arg(i);
	}
}

OBSSource VerticalCanvas::CreateScene(const QString &name)
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	if (!canvas) {
		return nullptr;
	}
	QString unique = UniqueSceneName(name.trimmed().isEmpty() ? QStringLiteral("Scene") : name.trimmed());
	OBSSceneAutoRelease scene = obs_canvas_scene_create(canvas, unique.toUtf8().constData());
	if (!scene) {
		return nullptr;
	}
	OBSSource source = obs_scene_get_source(scene);
	emit ScenesChanged();
	return source;
}

void VerticalCanvas::RemoveScene(obs_source_t *scene)
{
	if (!scene) {
		return;
	}
	OBSSource keep = scene;
	bool wasCurrent = CurrentScene().Get() == scene;
	if (wasCurrent) {
		SetChannel(nullptr);
	}
	obs_source_remove(scene);

	if (wasCurrent) {
		std::vector<OBSSource> rest = Scenes();
		rest.erase(std::remove(rest.begin(), rest.end(), keep), rest.end());
		SetChannel(rest.empty() ? nullptr : rest.front().Get());
	}
	emit ScenesChanged();
	emit CurrentSceneChanged();
}

bool VerticalCanvas::RenameScene(obs_source_t *scene, const QString &name)
{
	OBSCanvasAutoRelease canvas = GetCanvas();
	QString n = name.trimmed();
	if (!canvas || !scene || n.isEmpty()) {
		return false;
	}
	OBSSourceAutoRelease existing = obs_canvas_get_source_by_name(canvas, n.toUtf8().constData());
	if (existing && existing.Get() != scene) {
		return false;
	}
	obs_source_set_name(scene, n.toUtf8().constData());
	emit ScenesChanged();
	return true;
}

void VerticalCanvas::FollowMainScene()
{
	if (!linked) {
		return;
	}
	OBSCanvasAutoRelease canvas = GetCanvas();
	OBSSourceAutoRelease mainScene = obs_frontend_get_current_scene();
	if (!canvas || !mainScene) {
		return;
	}
	OBSSourceAutoRelease match = obs_canvas_get_source_by_name(canvas, obs_source_get_name(mainScene));
	if (match && obs_source_is_scene(match)) {
		SetCurrentScene(match);
	}
}

void VerticalCanvas::SetLinked(bool on)
{
	linked = on;
	config_set_bool(App()->GetUserConfig(), kSection, "Linked", on);
	if (on) {
		FollowMainScene();
	}
	emit SettingsChanged();
}

void VerticalCanvas::SetSafeZones(bool on)
{
	safeZones = on;
	config_set_bool(App()->GetUserConfig(), kSection, "SafeZones", on);
	emit SettingsChanged();
}

void VerticalCanvas::CopyFromMainScene(obs_source_t *mainSceneSource)
{
	obs_scene_t *mainScene = obs_scene_from_source(mainSceneSource);
	OBSSource target = CurrentScene();
	obs_scene_t *dst = obs_scene_from_source(target);
	if (!mainScene || !dst) {
		return;
	}

	struct Ctx {
		obs_scene_t *dst;
	} ctx{dst};

	auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *c = static_cast<Ctx *>(param);
		obs_source_t *src = obs_sceneitem_get_source(item);
		/* Scenes and groups belong to the main canvas; copy plain sources */
		if (!src || obs_source_is_scene(src) || obs_source_is_group(src)) {
			return true;
		}
		obs_sceneitem_t *added = obs_scene_add(c->dst, src);
		if (added) {
			obs_sceneitem_set_visible(added, obs_sceneitem_visible(item));
			FitItem(added, Fit::Width);
		}
		return true;
	};
	obs_scene_enum_items(mainScene, cb, &ctx);
}

/* ------------------------------------------------------------------------ */
/* Item placement                                                           */

void VerticalCanvas::FitItem(obs_sceneitem_t *item, Fit mode)
{
	if (!item) {
		return;
	}
	obs_scene_t *scene = obs_sceneitem_get_scene(item);
	OBSCanvasAutoRelease canvas = obs_source_get_canvas(obs_scene_get_source(scene));
	uint32_t cw = 0, ch = 0;
	if (!canvas || !obs_canvas_get_base_size(canvas, &cw, &ch) || !cw || !ch) {
		return;
	}

	obs_source_t *src = obs_sceneitem_get_source(item);
	float sw = float(obs_source_get_width(src));
	float sh = float(obs_source_get_height(src));

	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	info.alignment = OBS_ALIGN_CENTER;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.rot = 0.0f;
	vec2_set(&info.pos, float(cw) / 2.0f, float(ch) / 2.0f);

	switch (mode) {
	case Fit::Width:
		info.bounds_type = OBS_BOUNDS_NONE;
		if (sw > 0.0f) {
			float s = float(cw) / sw;
			vec2_set(&info.scale, s, s);
		}
		break;
	case Fit::Inside:
		info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		info.crop_to_bounds = false;
		vec2_set(&info.bounds, float(cw), float(ch));
		break;
	case Fit::Fill:
		/* Cover the whole canvas, cropping the sides: the usual way to
		 * put 16:9 gameplay into 9:16 */
		info.bounds_type = OBS_BOUNDS_SCALE_OUTER;
		info.crop_to_bounds = true;
		vec2_set(&info.bounds, float(cw), float(ch));
		break;
	case Fit::Center:
		/* Keep size, only centre it */
		break;
	case Fit::Reset:
		info.bounds_type = OBS_BOUNDS_NONE;
		vec2_set(&info.scale, 1.0f, 1.0f);
		break;
	}
	obs_sceneitem_set_info2(item, &info);

	if (mode == Fit::Reset) {
		obs_sceneitem_crop crop = {};
		obs_sceneitem_set_crop(item, &crop);
	}
	if (mode == Fit::Width && sw <= 0.0f) {
		/* Source has no size yet (still starting): try again shortly */
		OBSSceneItem ref = item;
		QTimer::singleShot(700, [ref] {
			if (obs_source_get_width(obs_sceneitem_get_source(ref)) > 0) {
				FitItem(ref, Fit::Width);
			}
		});
	}
	(void)sh;
}

/* ------------------------------------------------------------------------ */
/* Video mix for encoders                                                   */

video_t *VerticalCanvas::AcquireVideo()
{
	if (holdCount > 0 && heldCanvas) {
		holdCount++;
		return obs_canvas_get_video(heldCanvas);
	}

	OBSCanvasAutoRelease canvas = GetCanvas();
	obs_video_info ovi;
	if (!canvas || !ComputeVideoInfo(ovi)) {
		return nullptr;
	}
	if (!obs_canvas_has_video(canvas)) {
		if (!obs_canvas_reset_video(canvas, &ovi)) {
			blog(LOG_WARNING, "[vertical] Could not start the vertical canvas video");
			return nullptr;
		}
		blog(LOG_INFO, "[vertical] Vertical canvas video on (%ux%u)", ovi.base_width, ovi.base_height);
	}
	/* Keep a strong ref so a scene collection switch can't free the mix
	 * under a live encoder */
	heldCanvas = canvas.Get();
	holdCount = 1;
	return obs_canvas_get_video(canvas);
}

void VerticalCanvas::ReleaseVideo()
{
	if (holdCount <= 0) {
		return;
	}
	if (--holdCount == 0) {
		TryStopVideo(0);
	}
}

void VerticalCanvas::TryStopVideo(int attempt)
{
	if (holdCount > 0 || !heldCanvas) {
		return;
	}
	/* Encoders can take a moment to let go after their output stops */
	if (obs_canvas_stop_video(heldCanvas)) {
		blog(LOG_INFO, "[vertical] Vertical canvas video off");
		heldCanvas = nullptr;
		return;
	}
	if (attempt < 40 && !shuttingDown) {
		QTimer::singleShot(250, this, [this, attempt] { TryStopVideo(attempt + 1); });
	} else {
		heldCanvas = nullptr;
	}
}
