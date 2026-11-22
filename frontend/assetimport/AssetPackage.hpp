/******************************************************************************
    Siegfried: overlay bundle model.

    Every supported format (Streamlabs .overlay, OBS scene collection,
    loose files) is read into this one model. The installer only knows
    this model, never the original formats.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ZipReader.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

struct AssetPackage {
	/* A file inside the bundle (or inside a bundle within the bundle) */
	struct File {
		int archive = 0; /* index into Archives::readers */
		QString entry;   /* path inside that archive */
		QString fileName;
		quint64 size = 0;
	};

	enum class Kind {
		Video,       /* looping/one-shot media file */
		Image,
		Text,        /* settings in GDI+ text format */
		Label,       /* live text (latest follower...) */
		GameCapture, /* placeholder: the user's game */
		Webcam,      /* placeholder: the user's camera */
		SceneRef,    /* nested scene */
		Other,       /* passed through by source id */
	};

	struct Item {
		QString name;
		Kind kind = Kind::Other;
		int file = -1;
		QJsonObject settings;
		QString otherId;   /* Kind::Other */
		QString sceneRef;  /* Kind::SceneRef: Scene::key */
		QString labelType; /* Kind::Label */
		QJsonArray filters;

		/* Transform in package canvas pixels */
		double x = 0, y = 0, scaleX = 1, scaleY = 1, rot = 0;
		int align = 5; /* top-left */
		int boundsType = 0, boundsAlign = 0;
		double boundsX = 0, boundsY = 0;
		int cropL = 0, cropT = 0, cropR = 0, cropB = 0;
		bool visible = true;
	};

	struct Scene {
		QString key; /* id used by SceneRef */
		QString name;
		std::vector<Item> items;
	};

	struct Transition {
		bool present = false;
		QString id;   /* e.g. obs_stinger_transition */
		QString name;
		int file = -1;
		QJsonObject settings;
		int durationMs = 300;
	};

	struct Alert {
		QString event; /* follow, subscribe, bits, donation, raid, ... */
		int file = -1;
	};

	enum class Severity { Info, Warning, Problem };
	struct Issue {
		Severity severity;
		QString text;
	};

	struct Archives {
		QString path;
		std::vector<std::unique_ptr<ZipReader>> readers;
		QStringList names; /* display name per reader */
	};

	QString title;
	QString format; /* what the scenes were read from */
	int canvasW = 1920, canvasH = 1080;

	std::vector<File> files;
	std::vector<Scene> scenes;
	Transition transition;
	std::vector<Alert> alerts;
	std::vector<int> fontFiles;
	QStringList fontsUsed;
	std::vector<Issue> issues;
	int junkFiles = 0;
	int repairedPaths = 0;

	std::shared_ptr<Archives> archives;

	void Add(Severity s, const QString &text) { issues.push_back({s, text}); }
	bool HasProblems() const
	{
		for (const Issue &i : issues) {
			if (i.severity == Severity::Problem) {
				return true;
			}
		}
		return false;
	}
};

namespace AssetImport {

/* Read a bundle (zip) into the model. Returns false only if it can't be
 * read at all; partial results come back with issues. */
bool Scan(const QString &zipPath, AssetPackage &pkg, QString &error);

/* Friendly name of an alert event */
QString AlertEventTitle(const QString &event);

/* Friendly placeholder text for a live label type */
QString LabelText(const QString &labelType);

struct InstallResult {
	QString collectionName;
	QString assetDir;
	int filesCopied = 0;
	int fontsInstalled = 0;
};

/* Copies the files into Siegfried's asset folder and writes a new scene
 * collection. Does not switch to it. */
bool Install(const AssetPackage &pkg, const QString &collectionName, bool installFonts, InstallResult &result,
	     QString &error);

} // namespace AssetImport
