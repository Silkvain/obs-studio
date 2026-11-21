/******************************************************************************
    Native multistreaming for OBS Studio. See MultistreamManager.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamManager.hpp"

#include <widgets/OBSBasic.hpp>
#include <vertical/VerticalCanvas.hpp>

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "moc_MultistreamManager.cpp"

namespace {
constexpr const char *kConfigFile = "multistream.json";
constexpr int kReconnectRetries = 20;
constexpr int kReconnectDelaySec = 2;

QString DescribeStopCode(int code, const char *lastError)
{
	QString detail = lastError && *lastError ? QString::fromUtf8(lastError) : QString();
	QString base;
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return {};
	case OBS_OUTPUT_BAD_PATH:
		base = QTStr("Multistream.Error.BadPath");
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		base = QTStr("Multistream.Error.ConnectFailed");
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		base = QTStr("Multistream.Error.InvalidStream");
		break;
	case OBS_OUTPUT_DISCONNECTED:
		base = QTStr("Multistream.Error.Disconnected");
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		base = QTStr("Multistream.Error.Encode");
		break;
	default:
		base = QTStr("Multistream.Error.Generic");
		break;
	}
	return detail.isEmpty() ? base : base + QStringLiteral(" (") + detail + QStringLiteral(")");
}
} // namespace

MultistreamManager::MultistreamManager(QObject *parent) : QObject(parent)
{
	bitrateTimer.setInterval(1000);
	connect(&bitrateTimer, &QTimer::timeout, this, &MultistreamManager::UpdateBitrates);

	OBSBasic *main = OBSBasic::Get();
	if (main) {
		connect(main, &OBSBasic::StreamingStarted, this, &MultistreamManager::OnPrimaryStarted);
		connect(main, &OBSBasic::StreamingStopping, this, &MultistreamManager::OnPrimaryStopping);
		connect(main, &OBSBasic::StreamingStopped, this, &MultistreamManager::OnPrimaryStopping);
	}
}

MultistreamManager::~MultistreamManager()
{
	Shutdown();
}

void MultistreamManager::Shutdown()
{
	if (shuttingDown) {
		return;
	}
	shuttingDown = true;
	bitrateTimer.stop();
	for (auto &rt : runtimes) {
		rt->startSig.Disconnect();
		rt->stopSig.Disconnect();
		rt->reconnectSig.Disconnect();
		rt->reconnectOkSig.Disconnect();
		if (rt->output && obs_output_active(rt->output)) {
			obs_output_force_stop(rt->output);
		}
	}
	runtimes.clear();
}

/* ------------------------------------------------------------------------ */
/* Persistence                                                              */

void MultistreamManager::LoadForProfile(const std::filesystem::path &profileDir)
{
	if (!runtimes.empty()) {
		StopAll();
	}

	configPath = profileDir / kConfigFile;
	destinations.clear();

	QFile file(QString::fromStdString(configPath.u8string()));
	if (file.open(QIODevice::ReadOnly)) {
		QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
		for (const QJsonValue &v : doc.object().value("destinations").toArray()) {
			QJsonObject o = v.toObject();
			MultistreamDestination d;
			d.id = o.value("id").toString();
			if (d.id.isEmpty()) {
				d = MultistreamDestination::Create();
			}
			d.name = o.value("name").toString();
			d.server = o.value("server").toString();
			d.key = o.value("key").toString();
			d.enabled = o.value("enabled").toBool(true);
			d.customQuality = o.value("custom_quality").toBool(false);
			d.videoBitrate = o.value("video_bitrate").toInt(6000);
			d.audioBitrate = o.value("audio_bitrate").toInt(160);
			d.width = o.value("width").toInt(0);
			d.height = o.value("height").toInt(0);
			d.vertical = o.value("canvas").toString() == QStringLiteral("vertical");
			destinations.push_back(std::move(d));
		}
	}

	emit DestinationsChanged();
}

void MultistreamManager::Save() const
{
	if (configPath.empty()) {
		return;
	}

	QJsonArray arr;
	for (const auto &d : destinations) {
		QJsonObject o;
		o["id"] = d.id;
		o["name"] = d.name;
		o["server"] = d.server;
		o["key"] = d.key;
		o["enabled"] = d.enabled;
		o["custom_quality"] = d.customQuality;
		o["video_bitrate"] = d.videoBitrate;
		o["audio_bitrate"] = d.audioBitrate;
		o["width"] = d.width;
		o["height"] = d.height;
		o["canvas"] = d.vertical ? QStringLiteral("vertical") : QStringLiteral("main");
		arr.append(o);
	}
	QJsonObject root;
	root["version"] = 1;
	root["destinations"] = arr;

	QSaveFile file(QString::fromStdString(configPath.u8string()));
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
		if (!file.commit()) {
			blog(LOG_WARNING, "[multistream] Failed to save %s", configPath.u8string().c_str());
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Destination editing                                                      */

MultistreamDestination *MultistreamManager::Find(const QString &id)
{
	for (auto &d : destinations) {
		if (d.id == id) {
			return &d;
		}
	}
	return nullptr;
}

void MultistreamManager::AddDestination(const MultistreamDestination &dest)
{
	destinations.push_back(dest);
	Save();
	emit DestinationsChanged();

	if (primaryActive && dest.enabled) {
		StartDestination(dest);
	}
}

void MultistreamManager::UpdateDestination(const MultistreamDestination &dest)
{
	MultistreamDestination *d = Find(dest.id);
	if (!d) {
		return;
	}

	bool wasRunning = FindRuntime(dest.id) != nullptr;
	bool connectionChanged = d->server != dest.server || d->key != dest.key || !d->SameQuality(dest);
	*d = dest;
	Save();
	emit DestinationsChanged();

	/* Apply connection changes live by restarting that destination only */
	if (wasRunning && connectionChanged) {
		StopOne(dest.id);
		ReleaseRuntime(dest.id);
		if (primaryActive && dest.enabled) {
			StartDestination(dest);
		}
	}
}

void MultistreamManager::RemoveDestination(const QString &id)
{
	if (Runtime *rt = FindRuntime(id)) {
		StopRuntime(*rt, true);
	}
	ReleaseRuntime(id);

	destinations.erase(std::remove_if(destinations.begin(), destinations.end(),
					  [&](const MultistreamDestination &d) { return d.id == id; }),
			   destinations.end());
	Save();
	emit DestinationsChanged();
}

void MultistreamManager::SetEnabled(const QString &id, bool enabled)
{
	MultistreamDestination *d = Find(id);
	if (!d || d->enabled == enabled) {
		return;
	}
	d->enabled = enabled;
	Save();

	if (primaryActive) {
		if (enabled) {
			StartDestination(*d);
		} else {
			StopOne(id);
		}
	}
	emit StatusChanged(id);
}

/* ------------------------------------------------------------------------ */
/* Status                                                                   */

MultistreamManager::Runtime *MultistreamManager::FindRuntime(const QString &id)
{
	for (auto &rt : runtimes) {
		if (rt->id == id) {
			return rt.get();
		}
	}
	return nullptr;
}

const MultistreamManager::Runtime *MultistreamManager::FindRuntime(const QString &id) const
{
	for (const auto &rt : runtimes) {
		if (rt->id == id) {
			return rt.get();
		}
	}
	return nullptr;
}

MultistreamStatus MultistreamManager::Status(const QString &id) const
{
	const Runtime *rt = FindRuntime(id);
	return rt ? rt->status : MultistreamStatus{};
}

int MultistreamManager::EnabledCount() const
{
	return (int)std::count_if(destinations.begin(), destinations.end(),
				  [](const MultistreamDestination &d) { return d.enabled; });
}

int MultistreamManager::LiveCount() const
{
	return (int)std::count_if(runtimes.begin(), runtimes.end(), [](const std::unique_ptr<Runtime> &rt) {
		return rt->status.state == MultistreamState::Live;
	});
}

void MultistreamManager::SetStatus(const QString &id, MultistreamState state, const QString &message)
{
	Runtime *rt = FindRuntime(id);
	if (!rt) {
		return;
	}
	rt->status.state = state;
	rt->status.message = message;
	if (state != MultistreamState::Live) {
		rt->status.kbps = 0.0;
	}
	emit StatusChanged(id);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */

void MultistreamManager::OnPrimaryStarted()
{
	primaryActive = true;
	StartAll();
}

void MultistreamManager::OnPrimaryStopping()
{
	if (!primaryActive) {
		return;
	}
	primaryActive = false;
	StopAll();
}

void MultistreamManager::StartAll()
{
	for (const auto &d : destinations) {
		if (d.enabled) {
			StartDestination(d);
		}
	}
}

void MultistreamManager::StopAll()
{
	for (auto &rt : runtimes) {
		StopRuntime(*rt, false);
	}
}

void MultistreamManager::StartOne(const QString &id)
{
	if (MultistreamDestination *d = Find(id)) {
		StartDestination(*d);
	}
}

void MultistreamManager::StopOne(const QString &id)
{
	if (Runtime *rt = FindRuntime(id)) {
		StopRuntime(*rt, false);
	}
}

void MultistreamManager::StartDestination(const MultistreamDestination &dest)
{
	if (shuttingDown || !primaryActive) {
		return;
	}

	if (Runtime *existing = FindRuntime(dest.id)) {
		if (existing->output && obs_output_active(existing->output)) {
			return;
		}
		ReleaseRuntime(dest.id);
	}

	auto rt = std::make_unique<Runtime>();
	rt->id = dest.id;
	Runtime *raw = rt.get();
	runtimes.push_back(std::move(rt));

	auto fail = [&](const QString &msg) {
		blog(LOG_WARNING, "[multistream] '%s': %s", dest.name.toUtf8().constData(), msg.toUtf8().constData());
		SetStatus(dest.id, MultistreamState::Failed, msg);
		if (raw->vertical) {
			FreeRuntimeOutput(*raw);
		}
	};

	if (dest.server.trimmed().isEmpty()) {
		fail(QTStr("Multistream.Error.NoServer"));
		return;
	}

	/* By default borrow the primary stream's encoders: encode once, upload N
	 * times. Custom-quality destinations get their own encoders instead. */
	OBSOutputAutoRelease primary = obs_frontend_get_streaming_output();
	obs_encoder_t *venc = primary ? obs_output_get_video_encoder(primary) : nullptr;
	obs_encoder_t *aenc = primary ? obs_output_get_audio_encoder(primary, 0) : nullptr;
	if (!venc || !aenc) {
		fail(QTStr("Multistream.Error.NoEncoders"));
		return;
	}
	if (dest.vertical) {
		QString error;
		if (!CreateVerticalEncoders(*raw, dest, venc, aenc, error)) {
			fail(error);
			return;
		}
		venc = raw->videoEncoder;
		if (raw->audioEncoder) {
			aenc = raw->audioEncoder;
		}
	} else if (dest.customQuality) {
		QString error;
		if (!CreateCustomEncoders(*raw, dest, venc, aenc, error)) {
			fail(error);
			return;
		}
		venc = raw->videoEncoder;
		aenc = raw->audioEncoder;
	}

	OBSDataAutoRelease serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", dest.server.trimmed().toUtf8().constData());
	obs_data_set_string(serviceSettings, "key", dest.key.trimmed().toUtf8().constData());
	obs_data_set_bool(serviceSettings, "use_auth", false);

	std::string baseName = "multistream_" + dest.id.toStdString();
	raw->service = obs_service_create_private("rtmp_custom", (baseName + "_service").c_str(), serviceSettings);
	raw->output = obs_output_create("rtmp_output", (baseName + "_output").c_str(), nullptr, nullptr);
	if (!raw->service || !raw->output) {
		fail(QTStr("Multistream.Error.Generic"));
		return;
	}

	obs_output_set_service(raw->output, raw->service);
	obs_output_set_video_encoder(raw->output, venc);
	obs_output_set_audio_encoder(raw->output, aenc, 0);
	obs_output_set_reconnect_settings(raw->output, kReconnectRetries, kReconnectDelaySec);

	signal_handler_t *sh = obs_output_get_signal_handler(raw->output);
	raw->startSig.Connect(sh, "start", OutputStart, this);
	raw->stopSig.Connect(sh, "stop", OutputStop, this);
	raw->reconnectSig.Connect(sh, "reconnect", OutputReconnect, this);
	raw->reconnectOkSig.Connect(sh, "reconnect_success", OutputReconnectSuccess, this);

	SetStatus(dest.id, MultistreamState::Starting);
	blog(LOG_INFO, "[multistream] Starting destination '%s'", dest.name.toUtf8().constData());

	if (!obs_output_start(raw->output)) {
		const char *err = obs_output_get_last_error(raw->output);
		fail(DescribeStopCode(OBS_OUTPUT_ERROR, err));
		return;
	}

	if (!bitrateTimer.isActive()) {
		bitrateTimer.start();
	}
}

bool MultistreamManager::CreateCustomEncoders(Runtime &rt, const MultistreamDestination &dest, obs_encoder_t *mainVideo,
					      obs_encoder_t *mainAudio, QString &error)
{
	std::string base = "multistream_" + dest.id.toStdString();

	/* Same encoder type and settings as the main stream (NVENC, x264, ...),
	 * with this destination's bitrate. Force CBR so the bitrate applies. */
	OBSDataAutoRelease mainVideoSettings = obs_encoder_get_settings(mainVideo);
	OBSDataAutoRelease videoSettings = obs_data_create_from_json(obs_data_get_json(mainVideoSettings));
	obs_data_set_int(videoSettings, "bitrate", std::max(500, dest.videoBitrate));
	obs_data_set_string(videoSettings, "rate_control", "CBR");

	rt.videoEncoder = obs_video_encoder_create(obs_encoder_get_id(mainVideo), (base + "_video").c_str(),
						   videoSettings, nullptr);
	if (!rt.videoEncoder) {
		error = QTStr("Multistream.Error.Encode");
		return false;
	}

	video_t *video = obs_encoder_parent_video(mainVideo);
	obs_encoder_set_video(rt.videoEncoder, video ? video : obs_get_video());
	obs_encoder_set_gpu_scale_type(rt.videoEncoder, obs_encoder_get_scale_type(mainVideo));
	obs_encoder_set_frame_rate_divisor(rt.videoEncoder, obs_encoder_get_frame_rate_divisor(mainVideo));

	uint32_t baseW = obs_encoder_get_width(mainVideo);
	uint32_t baseH = obs_encoder_get_height(mainVideo);
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	bool fitsCanvas = dest.width > 0 && dest.height > 0 && uint32_t(dest.width) <= ovi.base_width &&
			  uint32_t(dest.height) <= ovi.base_height;
	if (dest.width > 0 && !fitsCanvas) {
		blog(LOG_WARNING, "[multistream] '%s': %dx%d is larger than the canvas; using the main stream size",
		     dest.name.toUtf8().constData(), dest.width, dest.height);
	}
	if (fitsCanvas) {
		obs_encoder_set_scaled_size(rt.videoEncoder, uint32_t(dest.width), uint32_t(dest.height));
	} else if (baseW && baseH) {
		/* Match the main stream's output size */
		obs_encoder_set_scaled_size(rt.videoEncoder, baseW, baseH);
	}

	OBSDataAutoRelease mainAudioSettings = obs_encoder_get_settings(mainAudio);
	OBSDataAutoRelease audioSettings = obs_data_create_from_json(obs_data_get_json(mainAudioSettings));
	obs_data_set_int(audioSettings, "bitrate", dest.audioBitrate > 0 ? dest.audioBitrate : 160);
	rt.audioEncoder = obs_audio_encoder_create(obs_encoder_get_id(mainAudio), (base + "_audio").c_str(),
						   audioSettings, obs_encoder_get_mixer_index(mainAudio), nullptr);
	if (!rt.audioEncoder) {
		error = QTStr("Multistream.Error.Encode");
		return false;
	}
	obs_encoder_set_audio(rt.audioEncoder, obs_get_audio());

	blog(LOG_INFO, "[multistream] '%s' uses custom quality: %d kbps video, %d kbps audio, %s",
	     dest.name.toUtf8().constData(), dest.videoBitrate, dest.audioBitrate,
	     dest.width > 0 ? QStringLiteral("%1x%2").arg(dest.width).arg(dest.height).toUtf8().constData()
			    : "main stream size");
	return true;
}

bool MultistreamManager::CreateVerticalEncoders(Runtime &rt, const MultistreamDestination &dest,
						obs_encoder_t *mainVideo, obs_encoder_t *mainAudio, QString &error)
{
	OBSBasic *main = OBSBasic::Get();
	VerticalCanvas *vc = main ? main->GetVerticalCanvas() : nullptr;
	video_t *video = vc ? vc->AcquireVideo() : nullptr;
	if (!video) {
		error = QTStr("Multistream.Error.NoVertical");
		return false;
	}
	rt.vertical = vc;

	std::string base = "multistream_" + dest.id.toStdString();

	/* Same encoder and settings as the main stream, fed from the vertical
	 * canvas. Custom quality only changes bitrate and size. */
	OBSDataAutoRelease mainVideoSettings = obs_encoder_get_settings(mainVideo);
	OBSDataAutoRelease videoSettings = obs_data_create_from_json(obs_data_get_json(mainVideoSettings));
	if (dest.customQuality) {
		obs_data_set_int(videoSettings, "bitrate", std::max(500, dest.videoBitrate));
		obs_data_set_string(videoSettings, "rate_control", "CBR");
	}
	rt.videoEncoder = obs_video_encoder_create(obs_encoder_get_id(mainVideo), (base + "_video").c_str(),
						   videoSettings, nullptr);
	if (!rt.videoEncoder) {
		error = QTStr("Multistream.Error.Encode");
		return false;
	}
	obs_encoder_set_video(rt.videoEncoder, video);
	obs_encoder_set_gpu_scale_type(rt.videoEncoder, obs_encoder_get_scale_type(mainVideo));
	obs_encoder_set_frame_rate_divisor(rt.videoEncoder, obs_encoder_get_frame_rate_divisor(mainVideo));

	uint32_t cw = 0, ch = 0;
	vc->GetSize(cw, ch);
	if (dest.customQuality && dest.width > 0 && dest.height > 0) {
		if (uint32_t(dest.width) <= cw && uint32_t(dest.height) <= ch) {
			obs_encoder_set_scaled_size(rt.videoEncoder, uint32_t(dest.width), uint32_t(dest.height));
		} else {
			blog(LOG_WARNING, "[multistream] '%s': %dx%d is larger than the vertical canvas (%ux%u)",
			     dest.name.toUtf8().constData(), dest.width, dest.height, cw, ch);
		}
	}

	if (dest.customQuality) {
		OBSDataAutoRelease mainAudioSettings = obs_encoder_get_settings(mainAudio);
		OBSDataAutoRelease audioSettings = obs_data_create_from_json(obs_data_get_json(mainAudioSettings));
		obs_data_set_int(audioSettings, "bitrate", dest.audioBitrate > 0 ? dest.audioBitrate : 160);
		rt.audioEncoder = obs_audio_encoder_create(obs_encoder_get_id(mainAudio), (base + "_audio").c_str(),
							   audioSettings, obs_encoder_get_mixer_index(mainAudio), nullptr);
		if (!rt.audioEncoder) {
			error = QTStr("Multistream.Error.Encode");
			return false;
		}
		obs_encoder_set_audio(rt.audioEncoder, obs_get_audio());
	}

	blog(LOG_INFO, "[multistream] '%s' streams the vertical canvas (%ux%u)", dest.name.toUtf8().constData(), cw,
	     ch);
	return true;
}

MultistreamManager::Runtime::~Runtime()
{
	FreeRuntimeOutput(*this);
}

void MultistreamManager::FreeRuntimeOutput(Runtime &rt)
{
	/* Output first, then its encoders, then the canvas video they read */
	rt.startSig.Disconnect();
	rt.stopSig.Disconnect();
	rt.reconnectSig.Disconnect();
	rt.reconnectOkSig.Disconnect();
	if (rt.output && obs_output_active(rt.output)) {
		obs_output_force_stop(rt.output);
	}
	rt.output = nullptr;
	rt.service = nullptr;
	rt.videoEncoder = nullptr;
	rt.audioEncoder = nullptr;
	if (rt.vertical) {
		rt.vertical->ReleaseVideo();
		rt.vertical = nullptr;
	}
}

void MultistreamManager::StopRuntime(Runtime &rt, bool force)
{
	rt.stopRequested = true;
	if (!rt.output || (!obs_output_active(rt.output) && !obs_output_reconnecting(rt.output))) {
		SetStatus(rt.id,
			  rt.status.state == MultistreamState::Failed ? MultistreamState::Failed
								      : MultistreamState::Offline,
			  rt.status.state == MultistreamState::Failed ? rt.status.message : QString());
		return;
	}
	if (force) {
		obs_output_force_stop(rt.output);
	} else {
		obs_output_stop(rt.output);
	}
}

void MultistreamManager::ReleaseRuntime(const QString &id)
{
	auto it = std::find_if(runtimes.begin(), runtimes.end(),
			       [&](const std::unique_ptr<Runtime> &rt) { return rt->id == id; });
	if (it == runtimes.end()) {
		return;
	}

	Runtime &rt = **it;
	rt.startSig.Disconnect();
	rt.stopSig.Disconnect();
	rt.reconnectSig.Disconnect();
	rt.reconnectOkSig.Disconnect();
	if (rt.output && obs_output_active(rt.output)) {
		obs_output_force_stop(rt.output);
	}
	runtimes.erase(it);

	if (runtimes.empty()) {
		bitrateTimer.stop();
	}
}

void MultistreamManager::UpdateBitrates()
{
	uint64_t now = os_gettime_ns();
	for (auto &rt : runtimes) {
		if (!rt->output || rt->status.state != MultistreamState::Live) {
			continue;
		}
		uint64_t bytes = obs_output_get_total_bytes(rt->output);
		if (rt->lastTimeNs && now > rt->lastTimeNs && bytes >= rt->lastBytes) {
			double seconds = double(now - rt->lastTimeNs) / 1e9;
			rt->status.kbps = double(bytes - rt->lastBytes) * 8.0 / 1000.0 / seconds;
			emit StatusChanged(rt->id);
		}
		rt->lastBytes = bytes;
		rt->lastTimeNs = now;
	}
}

/* ------------------------------------------------------------------------ */
/* libobs signal callbacks (called from libobs threads)                     */

static QString OutputDestinationId(calldata_t *cd)
{
	obs_output_t *output = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	const char *name = output ? obs_output_get_name(output) : nullptr;
	if (!name) {
		return {};
	}
	/* name is "multistream_<id>_output" */
	QString n = QString::fromUtf8(name);
	n.remove(0, int(strlen("multistream_")));
	n.chop(int(strlen("_output")));
	return n;
}

void MultistreamManager::OutputStart(void *data, calldata_t *cd)
{
	auto *self = static_cast<MultistreamManager *>(data);
	QString id = OutputDestinationId(cd);
	QMetaObject::invokeMethod(
		self,
		[self, id] {
			if (Runtime *rt = self->FindRuntime(id)) {
				rt->lastBytes = 0;
				rt->lastTimeNs = 0;
			}
			self->SetStatus(id, MultistreamState::Live);
		},
		Qt::QueuedConnection);
}

void MultistreamManager::OutputStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<MultistreamManager *>(data);
	QString id = OutputDestinationId(cd);
	int code = (int)calldata_int(cd, "code");
	const char *lastError = calldata_string(cd, "last_error");
	QString message = DescribeStopCode(code, lastError);

	QMetaObject::invokeMethod(
		self,
		[self, id, code, message] {
			Runtime *rt = self->FindRuntime(id);
			if (!rt) {
				return;
			}
			bool userStop = rt->stopRequested && (code == OBS_OUTPUT_SUCCESS);
			if (code == OBS_OUTPUT_SUCCESS || userStop) {
				self->SetStatus(id, MultistreamState::Offline);
			} else {
				self->SetStatus(id, MultistreamState::Failed, message);
			}
			blog(LOG_INFO, "[multistream] Destination %s stopped (code %d)", id.toUtf8().constData(), code);
			/* Vertical: free the encoder now so the vertical canvas stops
			 * rendering for encoding as soon as nothing streams it */
			if (rt->vertical) {
				FreeRuntimeOutput(*rt);
			}
		},
		Qt::QueuedConnection);
}

void MultistreamManager::OutputReconnect(void *data, calldata_t *cd)
{
	auto *self = static_cast<MultistreamManager *>(data);
	QString id = OutputDestinationId(cd);
	QMetaObject::invokeMethod(
		self, [self, id] { self->SetStatus(id, MultistreamState::Reconnecting); }, Qt::QueuedConnection);
}

void MultistreamManager::OutputReconnectSuccess(void *data, calldata_t *cd)
{
	auto *self = static_cast<MultistreamManager *>(data);
	QString id = OutputDestinationId(cd);
	QMetaObject::invokeMethod(
		self, [self, id] { self->SetStatus(id, MultistreamState::Live); }, Qt::QueuedConnection);
}
