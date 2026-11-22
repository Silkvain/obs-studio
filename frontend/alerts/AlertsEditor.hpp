/******************************************************************************
    Siegfried: alert editor (Alerts source > Properties > Edit alerts…).

    Left: every event type, tick to turn it on. Right: the alert itself,
    frozen at the time on the slider, with the name box you can drag and
    resize, plus "Name appears here" / "Name hides here" buttons, so the
    name lands on the banner after its intro, not in empty space.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "AlertsSource.hpp"

#include <widgets/OBSQTDisplay.hpp>

#include <QDialog>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;

class AlertsEditor;

/* Preview of the Alerts source with a draggable name box */
class AlertPreview : public OBSQTDisplay {
	Q_OBJECT

public:
	AlertPreview(QWidget *parent, AlertsEditor *editor);
	~AlertPreview() override;

signals:
	void BoxChanged(const QRectF &box);

protected:
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	static void Render(void *data, uint32_t cx, uint32_t cy);
	QRectF BoxOnScreen() const; /* widget coordinates */

	AlertsEditor *editor;
	enum class Drag { None, Move, Resize } drag = Drag::None;
	QPointF dragStart;
	QRectF boxStart;
};

class AlertsEditor : public QDialog {
	Q_OBJECT

public:
	static void Open(obs_source_t *alertsSource);

	obs_source_t *Source() const { return source; }
	const AlertStyle &Style() const { return style; }
	QRectF MediaArea(int w, int h) const; /* where the animation sits */

	~AlertsEditor() override;

private:
	AlertsEditor(QWidget *parent, obs_source_t *source);

	void SelectType(int row);
	void LoadStyle();
	void SaveStyle();
	void UpdatePreview();
	void UpdateTimeline();
	StreamEvent SampleEvent() const;

	OBSWeakSource weakSource;
	obs_source_t *source;
	StreamEventType type = StreamEventType::Follow;
	AlertStyle style;
	bool loading = false;

	QListWidget *types;
	AlertPreview *preview;
	QSlider *time;
	QLabel *timeLabel;
	QLabel *timingLabel;
	QLineEdit *media;
	QLineEdit *sound;
	QSlider *volume;
	QLineEdit *textTemplate;
	QCheckBox *showMessage;
	QPushButton *font;
	QPushButton *color;
	QCheckBox *outline;
	QSpinBox *duration;
	QDoubleSpinBox *minAmount;
	QLabel *minAmountLabel;
	QLineEdit *blocked;
	QTimer durationPoll;

	static QPointer<AlertsEditor> open;
};
