/******************************************************************************
    Native multistreaming for OBS Studio.

    Streams the main program output to any number of extra RTMP/RTMPS
    destinations at the same time as the primary stream configured in
    Settings -> Stream. Every destination reuses the primary stream's
    encoders, so the picture is encoded once no matter how many platforms
    you go live on; the only cost per destination is upload bandwidth.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUuid>

#include <filesystem>
#include <vector>

struct MultistreamDestination {
	QString id;
	QString name;
	QString server;
	QString key;
	bool enabled = true;

	static MultistreamDestination Create()
	{
		MultistreamDestination d;
		d.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
		return d;
	}
};

enum class MultistreamState {
	Offline,
	Starting,
	Live,
	Reconnecting,
	Failed,
};

struct MultistreamStatus {
	MultistreamState state = MultistreamState::Offline;
	QString message;
	double kbps = 0.0;
};

class MultistreamManager : public QObject {
	Q_OBJECT

public:
	explicit MultistreamManager(QObject *parent = nullptr);
	~MultistreamManager() override;

	/* Destination list (persisted per profile in multistream.json) */
	const std::vector<MultistreamDestination> &Destinations() const { return destinations; }
	void AddDestination(const MultistreamDestination &dest);
	void UpdateDestination(const MultistreamDestination &dest);
	void RemoveDestination(const QString &id);
	void SetEnabled(const QString &id, bool enabled);

	MultistreamStatus Status(const QString &id) const;
	int EnabledCount() const;
	int LiveCount() const;

	/* Profile handling */
	void LoadForProfile(const std::filesystem::path &profileDir);

	/* Lifecycle, driven by the main stream */
	bool PrimaryActive() const { return primaryActive; }
	void StartAll();
	void StopAll();
	void StartOne(const QString &id);
	void StopOne(const QString &id);

	/* Called by OBSBasic during shutdown, before obs_shutdown() */
	void Shutdown();

signals:
	void DestinationsChanged();
	void StatusChanged(const QString &id);

private slots:
	void OnPrimaryStarted();
	void OnPrimaryStopping();
	void UpdateBitrates();

private:
	struct Runtime {
		QString id;
		OBSOutputAutoRelease output;
		OBSServiceAutoRelease service;
		OBSSignal startSig;
		OBSSignal stopSig;
		OBSSignal reconnectSig;
		OBSSignal reconnectOkSig;
		MultistreamStatus status;
		uint64_t lastBytes = 0;
		uint64_t lastTimeNs = 0;
		bool stopRequested = false;
	};

	void Save() const;
	MultistreamDestination *Find(const QString &id);
	Runtime *FindRuntime(const QString &id);
	const Runtime *FindRuntime(const QString &id) const;
	void StartDestination(const MultistreamDestination &dest);
	void StopRuntime(Runtime &rt, bool force);
	void SetStatus(const QString &id, MultistreamState state, const QString &message = {});
	void ReleaseRuntime(const QString &id);

	static void OutputStart(void *data, calldata_t *cd);
	static void OutputStop(void *data, calldata_t *cd);
	static void OutputReconnect(void *data, calldata_t *cd);
	static void OutputReconnectSuccess(void *data, calldata_t *cd);

	std::filesystem::path configPath;
	std::vector<MultistreamDestination> destinations;
	std::vector<std::unique_ptr<Runtime>> runtimes;
	QTimer bitrateTimer;
	bool primaryActive = false;
	bool shuttingDown = false;
};
