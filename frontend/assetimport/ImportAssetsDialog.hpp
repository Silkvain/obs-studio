/******************************************************************************
    Siegfried: Scene Collection > Import Assets...

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "AssetPackage.hpp"

#include <QDialog>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

class ImportAssetsDialog : public QDialog {
	Q_OBJECT

public:
	/* nameTaken: is there already a scene collection with this name? */
	ImportAssetsDialog(QWidget *parent, std::function<bool(const QString &)> nameTaken);

	QString InstalledCollection() const { return installed; }
	bool SwitchAfterImport() const;

	/* Open a bundle right away (drag & drop onto the main window) */
	void LoadBundle(const QString &path);

protected:
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	void ChooseFile();
	void ShowPackage();
	void Import();
	QString UniqueName(const QString &base) const;

	std::function<bool(const QString &)> nameTaken;
	AssetPackage pkg;
	bool loaded = false;
	QString installed;

	QStackedWidget *pages;
	QLabel *dropLabel;
	QLabel *title;
	QLabel *format;
	QTreeWidget *tree;
	QLineEdit *name;
	QCheckBox *installFonts;
	QCheckBox *switchAfter;
	QPushButton *importButton;
	QPushButton *otherButton;
};
