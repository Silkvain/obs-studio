/******************************************************************************
    Destinations dock for native multistreaming.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <utility/MultistreamManager.hpp>

#include <QDialog>
#include <QPointer>
#include <QWidget>

#include <map>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

class MultistreamDestinationDialog : public QDialog {
	Q_OBJECT

public:
	MultistreamDestinationDialog(QWidget *parent, const MultistreamDestination &dest, bool isNew);
	MultistreamDestination Result() const;

private slots:
	void PresetChanged(int index);
	void Validate();

private:
	MultistreamDestination dest;
	QComboBox *preset;
	QLineEdit *name;
	QLineEdit *server;
	QLineEdit *key;
	QLabel *hint;
	QPushButton *okButton;
};

class MultistreamRow : public QWidget {
	Q_OBJECT

public:
	MultistreamRow(QWidget *parent, const QString &id, bool primary);

	void SetName(const QString &name);
	void SetEnabled(bool enabled);
	void SetStatus(MultistreamState state, const QString &message, double kbps);
	void SetPrimaryLive(bool live);

	const QString &Id() const { return id; }

signals:
	void EnabledToggled(const QString &id, bool enabled);
	void StartClicked(const QString &id);
	void StopClicked(const QString &id);
	void EditClicked(const QString &id);
	void RemoveClicked(const QString &id);

private:
	QString id;
	bool primary;
	bool primaryLive = false;
	MultistreamState state = MultistreamState::Offline;
	QCheckBox *enabled = nullptr;
	QLabel *dot;
	QLabel *name;
	QLabel *status;
	QLabel *bitrate;
	QToolButton *startStop = nullptr;
	QToolButton *menu = nullptr;

	void UpdateStartStop();
};

class MultistreamDock : public QWidget {
	Q_OBJECT

public:
	MultistreamDock(QWidget *parent, MultistreamManager *manager);

private slots:
	void Rebuild();
	void RefreshStatus(const QString &id);
	void RefreshPrimary();
	void AddClicked();
	void EditDestination(const QString &id);
	void RemoveDestination(const QString &id);

private:
	MultistreamManager *manager;
	QVBoxLayout *rowsLayout;
	QLabel *summary;
	QPointer<MultistreamRow> primaryRow;
	std::map<QString, QPointer<MultistreamRow>> rows;
	bool primaryLive = false;
	bool primaryStarting = false;
	MultistreamState primaryState = MultistreamState::Offline;

	void UpdateSummary();
};
