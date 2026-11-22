/******************************************************************************
    Siegfried: where the vertical canvas lives in the main window.

    - The vertical preview sits right of the main preview, in a splitter, so
      both pictures are visible side by side.
    - The Scenes and Sources docks get Main / Vertical tabs. Both docks switch
      together, and clicking either preview switches to its tab.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OBSBasic.hpp"

#include <vertical/VerticalCanvas.hpp>
#include <vertical/VerticalDock.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>

#include <algorithm>
#include <QVBoxLayout>

namespace {

/* Switches to the Main tab when the main preview is clicked */
class MainPreviewClickFilter : public QObject {
public:
	explicit MainPreviewClickFilter(OBSBasic *main_) : QObject(main_), main(main_) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		if (event->type() == QEvent::MouseButtonPress) {
			QMetaObject::invokeMethod(
				main, [m = main] { m->SetCanvasTab(0); }, Qt::QueuedConnection);
		}
		return QObject::eventFilter(obj, event);
	}

private:
	OBSBasic *main;
};

QTabBar *MakeCanvasTabs(QWidget *parent)
{
	QTabBar *tabs = new QTabBar(parent);
	tabs->setObjectName(QStringLiteral("canvasTabs"));
	tabs->addTab(QTStr("Vertical.Tab.Main"));
	tabs->addTab(QTStr("Vertical.Tab.Vertical"));
	tabs->setExpanding(true);
	tabs->setDocumentMode(true);
	tabs->setDrawBase(false);
	tabs->setFocusPolicy(Qt::NoFocus);
	tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	return tabs;
}

/* Put tabs above `frame` and a stack holding `frame` + `verticalPage` in its place */
QStackedWidget *WrapInTabs(QWidget *frame, QWidget *verticalPage, QTabBar *tabs)
{
	QWidget *container = frame->parentWidget();
	QBoxLayout *layout = qobject_cast<QBoxLayout *>(container->layout());
	int index = layout ? layout->indexOf(frame) : -1;

	QStackedWidget *stack = new QStackedWidget(container);
	if (layout && index >= 0) {
		layout->removeWidget(frame);
		layout->insertWidget(index, tabs);
		layout->insertWidget(index + 1, stack, 1);
	}
	stack->addWidget(frame);
	stack->addWidget(verticalPage);
	return stack;
}

} // namespace

void OBSBasic::SetupVerticalUI()
{
	/* Preview beside the main preview */
	verticalPane = new VerticalPane(ui->centralwidget, vertical);

	QBoxLayout *central = qobject_cast<QBoxLayout *>(ui->centralwidget->layout());
	int index = central ? central->indexOf(ui->canvasEditor) : -1;
	canvasSplitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
	canvasSplitter->setObjectName(QStringLiteral("canvasSplitter"));
	canvasSplitter->setChildrenCollapsible(false);
	if (central && index >= 0) {
		central->removeWidget(ui->canvasEditor);
		central->insertWidget(index, canvasSplitter, 1);
	}
	canvasSplitter->addWidget(ui->canvasEditor);
	canvasSplitter->addWidget(verticalPane);
	canvasSplitter->setStretchFactor(0, 1);
	canvasSplitter->setStretchFactor(1, 0);

	const char *splitterState = config_get_string(App()->GetUserConfig(), "Vertical", "SplitterState");
	if (!splitterState || !canvasSplitter->restoreState(QByteArray::fromBase64(splitterState))) {
		/* Wide enough for a 9:16 picture at the preview's height */
		QTimer::singleShot(0, this, [this] {
			if (!canvasSplitter) {
				return;
			}
			int total = canvasSplitter->width();
			int side = std::clamp(canvasSplitter->height() * 9 / 16 + 16, 160, total / 3);
			canvasSplitter->setSizes({total - side, side});
		});
	}

	/* View > Vertical Preview */
	config_set_default_bool(App()->GetUserConfig(), "Vertical", "ShowPreview", true);
	bool show = config_get_bool(App()->GetUserConfig(), "Vertical", "ShowPreview");
	showVerticalPreview = new QAction(QTStr("Vertical.ShowPreview"), this);
	showVerticalPreview->setCheckable(true);
	showVerticalPreview->setChecked(show);
	verticalPane->setVisible(show);
	ui->viewMenu->insertAction(ui->toggleListboxToolbars, showVerticalPreview);
	connect(showVerticalPreview, &QAction::toggled, this, [this](bool on) {
		verticalPane->setVisible(on);
		config_set_bool(App()->GetUserConfig(), "Vertical", "ShowPreview", on);
	});

	/* Main / Vertical tabs in the Scenes and Sources docks */
	scenesCanvasTabs = MakeCanvasTabs(ui->scenesFrame->parentWidget());
	sourcesCanvasTabs = MakeCanvasTabs(ui->sourcesFrame->parentWidget());
	scenesCanvasStack = WrapInTabs(ui->scenesFrame, new VerticalScenesPanel(nullptr, vertical), scenesCanvasTabs);
	sourcesCanvasStack = WrapInTabs(
		ui->sourcesFrame, new VerticalSourcesPanel(nullptr, vertical, verticalPane->Preview()), sourcesCanvasTabs);

	connect(scenesCanvasTabs, &QTabBar::currentChanged, this, &OBSBasic::SetCanvasTab);
	connect(sourcesCanvasTabs, &QTabBar::currentChanged, this, &OBSBasic::SetCanvasTab);

	connect(verticalPane->Preview(), &VerticalPreview::Activated, this, [this] { SetCanvasTab(1); });
	ui->preview->installEventFilter(new MainPreviewClickFilter(this));
}

void OBSBasic::SetCanvasTab(int index)
{
	for (QTabBar *tabs : {scenesCanvasTabs.data(), sourcesCanvasTabs.data()}) {
		if (tabs && tabs->currentIndex() != index) {
			QSignalBlocker block(tabs);
			tabs->setCurrentIndex(index);
		}
	}
	for (QStackedWidget *stack : {scenesCanvasStack.data(), sourcesCanvasStack.data()}) {
		if (stack) {
			stack->setCurrentIndex(index);
		}
	}
	/* Show which preview the lists belong to */
	if (verticalPane) {
		verticalPane->setProperty("active", index == 1);
	}
}

void OBSBasic::SaveVerticalUI()
{
	if (canvasSplitter && verticalPane && verticalPane->isVisible()) {
		config_set_string(App()->GetUserConfig(), "Vertical", "SplitterState",
				  canvasSplitter->saveState().toBase64().constData());
	}
}
