/******************************************************************************
    Siegfried: Vertical (9:16) canvas.

    A second, separately arranged picture for phone platforms (TikTok,
    Shorts, Reels, Kick/Twitch mobile). It is a real OBS canvas with its own
    scenes; the sources inside those scenes are the same sources used by the
    main canvas, so a webcam or game capture is only captured once.

    Size follows the main canvas: a 1920x1080 canvas gives 1080x1920,
    2560x1440 gives 1440x2560, 3840x2160 gives 2160x3840.

    Cost: the canvas only renders while something uses it. The preview draws
    it while the dock is visible; a video mix (needed for encoding) exists
    only while a vertical destination or Twitch Dual Format is live.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>
#include <obs-frontend-api.h>

#include <QObject>
#include <QString>

#include <mutex>
#include <vector>

class OBSBasic;

class VerticalCanvas : public QObject {
	Q_OBJECT

public:
	static constexpr const char *kCanvasName = "Vertical";

	explicit VerticalCanvas(OBSBasic *main);
	~VerticalCanvas() override;

	/* Thread safe (used by the preview's render thread). May return null. */
	OBSCanvasAutoRelease GetCanvas() const;
	bool GetSize(uint32_t &cx, uint32_t &cy) const;
	QString Uuid() const;

	/* Scenes of the vertical canvas, in creation order */
	std::vector<OBSSource> Scenes() const;
	OBSSource CurrentScene() const;
	void SetCurrentScene(obs_source_t *scene);
	OBSSource CreateScene(const QString &name);
	void RemoveScene(obs_source_t *scene);
	bool RenameScene(obs_source_t *scene, const QString &name);
	QString UniqueSceneName(const QString &base) const;

	/* Copy every source of a main scene into the current vertical scene */
	void CopyFromMainScene(obs_source_t *mainScene);

	/* Vertical scene follows the main scene with the same name */
	bool Linked() const { return linked; }
	void SetLinked(bool on);

	bool SafeZones() const { return safeZones; }
	void SetSafeZones(bool on);

	/* Encoding: a vertical destination holds the video while it is live */
	video_t *AcquireVideo();
	void ReleaseVideo();
	bool VideoHeld() const { return holdCount > 0; }

	void Shutdown();

	/* Save the scene collection soon (debounced) after a vertical edit */
	void RequestSave();

	/* Fit a scene item into the vertical canvas */
	enum class Fit { Width, Inside, Fill, Center, Reset };
	static void FitItem(obs_sceneitem_t *item, Fit mode);

signals:
	void CanvasChanged();
	void ScenesChanged();
	void CurrentSceneChanged();
	void SettingsChanged();

private:
	static void FrontendEvent(obs_frontend_event event, void *data);
	static void VideoResetSignal(void *data, calldata_t *cd);
	static void CanvasSourceSignal(void *data, calldata_t *cd);

	void EnsureCanvas();
	void DropCanvas();
	void ConnectCanvasSignals();
	bool ComputeVideoInfo(obs_video_info &ovi) const;
	void UpdateVideoInfo();
	void FollowMainScene();
	void TryStopVideo(int attempt);
	void SetChannel(obs_source_t *scene);

	OBSBasic *main;

	mutable std::mutex canvasMutex;
	OBSWeakCanvas weakCanvas;

	OBSSignal videoResetSig;
	OBSSignal sourceAddSig;
	OBSSignal sourceRemoveSig;
	OBSSignal sourceRenameSig;

	/* Video mix hold for encoders */
	int holdCount = 0;
	OBSCanvas heldCanvas;
	bool twitchHold = false;

	class QTimer *saveTimer = nullptr;

	bool linked = true;
	bool safeZones = true;
	bool shuttingDown = false;
};
