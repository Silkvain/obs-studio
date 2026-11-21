/******************************************************************************
    Siegfried: Vertical canvas dock (preview, scenes, sources).

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

class QComboBox;
class QLabel;
class QListWidget;
class QToolButton;
class VerticalCanvas;

/* Live, editable preview of the vertical canvas */
class VerticalPreview : public OBSQTDisplay {
	Q_OBJECT

public:
	VerticalPreview(QWidget *parent, VerticalCanvas *vc);
	~VerticalPreview() override;

signals:
	void SelectionChanged();

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

class VerticalDock : public QWidget {
	Q_OBJECT

public:
	VerticalDock(QWidget *parent, VerticalCanvas *vc);
	~VerticalDock() override;

private slots:
	void RefreshScenes();
	void RefreshSources();
	void QueueRefreshSources();
	void ConnectScene();

private:
	static void SceneSignal(void *data, calldata_t *cd);
	static void TransformSignal(void *data, calldata_t *cd);

	OBSSceneItem SelectedItem() const;
	void ShowAddMenu();
	void ShowSceneMenu();
	void AddScene();
	void RemoveScene();
	void RenameScene();

	VerticalCanvas *vc;
	VerticalPreview *preview;
	QComboBox *scenes;
	QListWidget *sources;
	QLabel *sizeLabel;
	QToolButton *addSource;
	QToolButton *sceneMenu;

	OBSWeakSource connectedScene;
	std::vector<OBSSignal> sceneSignals;
	bool refreshQueued = false;
	bool updating = false;
};
