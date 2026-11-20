/******************************************************************************
    Compact on/off switch: grey track when off, green when on, the knob
    slides right when on. Behaves like a checkable button.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QAbstractButton>

class QVariantAnimation;

class ToggleSwitch : public QAbstractButton {
	Q_OBJECT

public:
	explicit ToggleSwitch(QWidget *parent = nullptr);

	QSize sizeHint() const override { return QSize(36, 20); }
	QSize minimumSizeHint() const override { return sizeHint(); }

	/* Set without animating or emitting toggled() */
	void SetCheckedSilently(bool on);

protected:
	void paintEvent(QPaintEvent *) override;
	void checkStateSet() override;
	void nextCheckState() override;

private:
	void Animate(bool on);

	qreal position = 0.0; /* 0 = off (knob left), 1 = on (knob right) */
	QVariantAnimation *anim;
};
