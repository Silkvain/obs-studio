/******************************************************************************
    Siegfried: Vertical canvas UI: the preview that sits beside the main
    preview, and the scenes/sources panels shown in the Vertical tabs.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <widgets/OBSQTDisplay.hpp>

#include <obs.hpp>
#include <graphics/vec2.h>

#include <QWidget>

class QAction;
class QLabel;
class QListWidget;
class QMenu;
class QToolBar;
class VerticalCanvas;

/* Live, editable preview of the vertical canvas */
class VerticalPreview : public OBSQTDisplay {
	Q_OBJECT

public:
	VerticalPreview(QWidget *parent, VerticalCanvas *vc);
	~VerticalPreview() override;

signals:
	void SelectionChanged();
	void Activated();

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

private:
	static void Render(void *data, uint32_t cx, uint32_t cy);

	bool MapToCanvas(const QPointF &widgetPos, vec2 &out, float *scaleOut = nullptr) const;
	OBSSceneItem ItemAt(const vec2 &pos) const;
	OBSSceneItem HandleAt(const vec2 &pos, float scale) const;
	OBSSceneItem Selected() const;
	void Select(obs_sceneitem_t *item);
	void Snap(obs_sceneitem_t *item, float tolerance);
	void ScaleAroundCenter(obs_sceneitem_t *item, float factor);

	VerticalCanvas *vc;

	enum class Mode { None, Move, Scale };
	Mode mode = Mode::None;
	OBSSceneItem dragItem;
	vec2 dragStart;
	vec2 itemStartPos;
	vec2 itemStartScale;
	vec2 itemStartBounds;
	vec2 center;
	float startDistance = 1.0f;
};

/* The preview with a small title line, placed beside the main preview */
class VerticalPane : public QWidget {
	Q_OBJECT

public:
	VerticalPane(QWidget *parent, VerticalCanvas *vc);

	VerticalPreview *Preview() const { return preview; }

private slots:
	void UpdateTitle();

private:
	VerticalCanvas *vc;
	VerticalPreview *preview;
	QLabel *title;
};

/* Vertical scenes, shown in the Scenes dock's Vertical tab */
class VerticalScenesPanel : public QWidget {
	Q_OBJECT

public:
	VerticalScenesPanel(QWidget *parent, VerticalCanvas *vc);

private slots:
	void Refresh();
	void UpdateTooltips();

private:
	QString LinkTooltip(obs_source_t *scene) const;
	void ShowContextMenu(const QPoint &pos);
	void ShowOptionsMenu();
	void FillSceneMenu(QMenu *menu, OBSSource scene);
	void AddScene();
	void RemoveScene();
	void RenameScene();

	VerticalCanvas *vc;
	QListWidget *list;
	QToolBar *toolbar;
	QAction *optionsAction;
	bool updating = false;
};

/* Sources of the current vertical scene, shown in the Sources dock's Vertical tab */
class VerticalSourcesPanel : public QWidget {
	Q_OBJECT

public:
	VerticalSourcesPanel(QWidget *parent, VerticalCanvas *vc, VerticalPreview *preview);
	~VerticalSourcesPanel() override;

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
	void Refresh();
	void QueueRefresh();
	void ConnectScene();

private:
	static void SceneSignal(void *data, calldata_t *cd);
	static void TransformSignal(void *data, calldata_t *cd);

	OBSSceneItem SelectedItem() const;
	QWidget *MakeRow(obs_sceneitem_t *item);
	void ShowAddMenu();
	void ShowContextMenu(const QPoint &pos);
	void RemoveSelected();

	VerticalCanvas *vc;
	VerticalPreview *preview;
	QListWidget *list;
	QToolBar *toolbar;
	QAction *addAction;

	OBSWeakSource connectedScene;
	std::vector<OBSSignal> sceneSignals;
	bool refreshQueued = false;
	bool updating = false;
};
