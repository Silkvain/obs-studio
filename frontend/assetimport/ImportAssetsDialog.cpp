/******************************************************************************
    Siegfried: Scene Collection > Import Assets...

    Drop a purchased overlay bundle (.zip), see what's in it and what
    won't work, then install it as a ready scene collection.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ImportAssetsDialog.hpp"

#include <OBSApp.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QUrl>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "moc_ImportAssetsDialog.cpp"

using Pkg = AssetPackage;

ImportAssetsDialog::ImportAssetsDialog(QWidget *parent, std::function<bool(const QString &)> taken)
	: QDialog(parent),
	  nameTaken(std::move(taken))
{
	setWindowTitle(QTStr("ImportAssets.Title"));
	setAcceptDrops(true);
	resize(620, 560);

	pages = new QStackedWidget(this);

	/* Page 1: drop zone */
	QWidget *dropPage = new QWidget(pages);
	dropLabel = new QLabel(QTStr("ImportAssets.Drop"), dropPage);
	dropLabel->setAlignment(Qt::AlignCenter);
	dropLabel->setWordWrap(true);
	dropLabel->setMinimumHeight(220);
	dropLabel->setStyleSheet(
		"QLabel { border: 2px dashed rgba(255, 255, 255, 70); border-radius: 10px; padding: 24px; font-size: 15px; }");
	QPushButton *choose = new QPushButton(QTStr("ImportAssets.Choose"), dropPage);
	QLabel *hint = new QLabel(QTStr("ImportAssets.Hint"), dropPage);
	hint->setWordWrap(true);
	hint->setStyleSheet("color: palette(placeholder-text);");
	QVBoxLayout *dl = new QVBoxLayout(dropPage);
	dl->addWidget(dropLabel, 1);
	dl->addWidget(choose, 0, Qt::AlignHCenter);
	dl->addWidget(hint);
	connect(choose, &QPushButton::clicked, this, &ImportAssetsDialog::ChooseFile);

	/* Page 2: what's in the bundle */
	QWidget *infoPage = new QWidget(pages);
	title = new QLabel(infoPage);
	title->setStyleSheet("font-size: 16px; font-weight: bold;");
	format = new QLabel(infoPage);
	format->setWordWrap(true);
	format->setStyleSheet("color: palette(placeholder-text);");
	tree = new QTreeWidget(infoPage);
	tree->setHeaderHidden(true);
	tree->setColumnCount(1);
	tree->setWordWrap(true);
	tree->setTextElideMode(Qt::ElideRight);
	tree->setRootIsDecorated(true);
	name = new QLineEdit(infoPage);
	installFonts = new QCheckBox(QTStr("ImportAssets.InstallFonts"), infoPage);
	switchAfter = new QCheckBox(QTStr("ImportAssets.Switch"), infoPage);
	switchAfter->setChecked(true);
	QFormLayout *form = new QFormLayout();
	form->addRow(QTStr("ImportAssets.Name"), name);
	QVBoxLayout *il = new QVBoxLayout(infoPage);
	il->setContentsMargins(0, 0, 0, 0);
	il->addWidget(title);
	il->addWidget(format);
	il->addWidget(tree, 1);
	il->addLayout(form);
	il->addWidget(installFonts);
	il->addWidget(switchAfter);

	pages->addWidget(dropPage);
	pages->addWidget(infoPage);

	QDialogButtonBox *buttons = new QDialogButtonBox(this);
	otherButton = buttons->addButton(QTStr("ImportAssets.Other"), QDialogButtonBox::ResetRole);
	otherButton->setVisible(false);
	importButton = buttons->addButton(QTStr("ImportAssets.Import"), QDialogButtonBox::AcceptRole);
	importButton->setEnabled(false);
	buttons->addButton(QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(importButton, &QPushButton::clicked, this, &ImportAssetsDialog::Import);
	connect(otherButton, &QPushButton::clicked, this, &ImportAssetsDialog::ChooseFile);
	connect(name, &QLineEdit::textChanged, this,
		[this](const QString &t) { importButton->setEnabled(loaded && !t.trimmed().isEmpty()); });

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(pages, 1);
	layout->addWidget(buttons);
}

bool ImportAssetsDialog::SwitchAfterImport() const
{
	return switchAfter->isChecked();
}

void ImportAssetsDialog::dragEnterEvent(QDragEnterEvent *event)
{
	const QMimeData *mime = event->mimeData();
	if (mime->hasUrls() && mime->urls().size() == 1 && mime->urls().first().isLocalFile()) {
		event->acceptProposedAction();
	}
}

void ImportAssetsDialog::dropEvent(QDropEvent *event)
{
	LoadBundle(event->mimeData()->urls().first().toLocalFile());
}

void ImportAssetsDialog::ChooseFile()
{
	QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
	QString path = QFileDialog::getOpenFileName(this, QTStr("ImportAssets.Choose"), dir,
						    QTStr("ImportAssets.Filter") + QStringLiteral(" (*.zip *.overlay)"));
	if (!path.isEmpty()) {
		LoadBundle(path);
	}
}

QString ImportAssetsDialog::UniqueName(const QString &base) const
{
	QString n = base.isEmpty() ? QStringLiteral("Imported") : base;
	QString candidate = n;
	for (int i = 2; nameTaken && nameTaken(candidate); i++) {
		candidate = QStringLiteral("%1 %2").arg(n).arg(i);
	}
	return candidate;
}

void ImportAssetsDialog::LoadBundle(const QString &path)
{
	QApplication::setOverrideCursor(Qt::WaitCursor);
	QString error;
	AssetPackage scanned;
	bool ok = AssetImport::Scan(path, scanned, error);
	QApplication::restoreOverrideCursor();
	if (!ok) {
		QMessageBox::warning(this, QTStr("ImportAssets.Title"),
				     QTStr("ImportAssets.CantRead").arg(QFileInfo(path).fileName(), error));
		return;
	}
	pkg = std::move(scanned);
	loaded = true;
	ShowPackage();
}

void ImportAssetsDialog::ShowPackage()
{
	pages->setCurrentIndex(1);
	otherButton->setVisible(true);
	title->setText(pkg.title);
	format->setText(QTStr("ImportAssets.ReadFrom").arg(pkg.format));
	name->setText(UniqueName(pkg.title));

	tree->clear();
	QStyle *st = style();
	QIcon ok = st->standardIcon(QStyle::SP_DialogApplyButton);
	QIcon info = st->standardIcon(QStyle::SP_MessageBoxInformation);
	QIcon warn = st->standardIcon(QStyle::SP_MessageBoxWarning);
	QIcon bad = st->standardIcon(QStyle::SP_MessageBoxCritical);

	auto section = [&](const QString &text, const QIcon &icon) {
		QTreeWidgetItem *i = new QTreeWidgetItem(tree, {text});
		i->setIcon(0, icon);
		QFont f = i->font(0);
		f.setBold(true);
		i->setFont(0, f);
		return i;
	};
	auto child = [](QTreeWidgetItem *parent, const QString &a, const QString &b = QString()) {
		QString text = b.isEmpty() ? a : QStringLiteral("%1  —  %2").arg(a, b);
		QTreeWidgetItem *i = new QTreeWidgetItem(parent, {text});
		i->setToolTip(0, text);
		return i;
	};

	/* Scenes */
	QTreeWidgetItem *scenes = section(QTStr("ImportAssets.Scenes").arg(pkg.scenes.size()), ok);
	for (const Pkg::Scene &sc : pkg.scenes) {
		QStringList parts;
		int game = 0, cam = 0;
		for (const Pkg::Item &it : sc.items) {
			game += it.kind == Pkg::Kind::GameCapture;
			cam += it.kind == Pkg::Kind::Webcam;
		}
		parts << QTStr("ImportAssets.Items").arg(sc.items.size());
		if (game) {
			parts << QTStr("ImportAssets.HasGame");
		}
		if (cam) {
			parts << QTStr("ImportAssets.HasCam");
		}
		child(scenes, sc.name, parts.join(QStringLiteral(" · ")));
	}

	/* Transition */
	if (pkg.transition.present) {
		QTreeWidgetItem *t = section(QTStr("ImportAssets.Transition"), ok);
		QString what = pkg.transition.file >= 0 ? pkg.files[size_t(pkg.transition.file)].fileName
							: pkg.transition.id;
		child(t, what, QTStr("ImportAssets.TransitionReal"));
	} else {
		section(QTStr("ImportAssets.NoTransition"), info);
	}

	/* Alerts */
	if (!pkg.alerts.empty()) {
		QTreeWidgetItem *a = section(QTStr("ImportAssets.Alerts").arg(pkg.alerts.size()), ok);
		for (const Pkg::Alert &al : pkg.alerts) {
			child(a, AssetImport::AlertEventTitle(al.event), pkg.files[size_t(al.file)].fileName);
		}
		a->setExpanded(false);
	}

	/* Fonts */
	if (!pkg.fontsUsed.isEmpty() || !pkg.fontFiles.empty()) {
		QStringList families = QFontDatabase::families();
		int missing = 0;
		for (const QString &f : pkg.fontsUsed) {
			missing += !families.contains(f, Qt::CaseInsensitive);
		}
		QTreeWidgetItem *f = section(QTStr("ImportAssets.Fonts"), missing ? warn : ok);
		for (const QString &fam : pkg.fontsUsed) {
			bool have = families.contains(fam, Qt::CaseInsensitive);
			QTreeWidgetItem *c = child(f, fam, have ? QTStr("ImportAssets.FontInstalled")
							       : QTStr("ImportAssets.FontMissing"));
			c->setIcon(0, have ? ok : warn);
		}
		for (int idx : pkg.fontFiles) {
			child(f, pkg.files[size_t(idx)].fileName, QTStr("ImportAssets.FontIncluded"));
		}
		f->setExpanded(missing > 0);
	}
	installFonts->setVisible(!pkg.fontFiles.empty());
	installFonts->setChecked(!pkg.fontFiles.empty());

	/* Things to know */
	if (!pkg.issues.empty() || pkg.repairedPaths > 0 || pkg.junkFiles > 0) {
		bool problems = pkg.HasProblems();
		QTreeWidgetItem *n = section(QTStr("ImportAssets.Notes"), problems ? bad : info);
		if (pkg.repairedPaths > 0) {
			child(n, QTStr("ImportAssets.Repaired").arg(pkg.repairedPaths))->setIcon(0, ok);
		}
		for (const Pkg::Issue &is : pkg.issues) {
			QTreeWidgetItem *c = child(n, is.text);
			c->setIcon(0, is.severity == Pkg::Severity::Problem   ? bad
				      : is.severity == Pkg::Severity::Warning ? warn
									      : info);
			c->setToolTip(0, is.text);
		}
		if (pkg.junkFiles > 0) {
			child(n, QTStr("ImportAssets.Junk").arg(pkg.junkFiles))->setIcon(0, info);
		}
		n->setExpanded(true);
	}
	scenes->setExpanded(true);

	importButton->setEnabled(!pkg.scenes.empty() && !name->text().trimmed().isEmpty());
	importButton->setText(pkg.HasProblems() ? QTStr("ImportAssets.ImportAnyway") : QTStr("ImportAssets.Import"));
}

void ImportAssetsDialog::Import()
{
	QString n = name->text().trimmed();
	if (nameTaken && nameTaken(n)) {
		QMessageBox::warning(this, QTStr("ImportAssets.Title"), QTStr("ImportAssets.NameTaken"));
		return;
	}
	QApplication::setOverrideCursor(Qt::WaitCursor);
	AssetImport::InstallResult result;
	QString error;
	bool ok = AssetImport::Install(pkg, n, installFonts->isVisible() && installFonts->isChecked(), result, error);
	QApplication::restoreOverrideCursor();
	if (!ok) {
		QMessageBox::warning(this, QTStr("ImportAssets.Title"), QTStr("ImportAssets.Failed").arg(error));
		return;
	}
	installed = result.collectionName;
	accept();
}
