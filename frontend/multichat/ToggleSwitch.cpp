/******************************************************************************
    ON/OFF switch. See ToggleSwitch.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ToggleSwitch.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include "moc_ToggleSwitch.cpp"

namespace {
const QColor kOn("#3fa34d");
const QColor kOff("#5a5f6a");
const QColor kDisabled("#3a3e46");

QColor Mix(const QColor &a, const QColor &b, qreal t)
{
	const float f = float(t);
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f, a.greenF() + (b.greenF() - a.greenF()) * f,
				a.blueF() + (b.blueF() - a.blueF()) * f);
}
} // namespace

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	position = 0.0;

	anim = new QVariantAnimation(this);
	anim->setDuration(140);
	anim->setEasingCurve(QEasingCurve::OutCubic);
	connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
		position = v.toReal();
		update();
	});
}

void ToggleSwitch::SetCheckedSilently(bool on)
{
	QSignalBlocker block(this);
	setChecked(on);
	anim->stop();
	position = on ? 1.0 : 0.0;
	update();
}

void ToggleSwitch::checkStateSet()
{
	Animate(isChecked());
}

void ToggleSwitch::nextCheckState()
{
	QAbstractButton::nextCheckState();
	Animate(isChecked());
}

void ToggleSwitch::Animate(bool on)
{
	anim->stop();
	anim->setStartValue(position);
	anim->setEndValue(on ? 1.0 : 0.0);
	anim->start();
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	qreal radius = r.height() / 2.0;

	/* Track */
	p.setPen(Qt::NoPen);
	p.setBrush(isEnabled() ? Mix(kOff, kOn, position) : kDisabled);
	p.drawRoundedRect(r, radius, radius);

	/* Knob */
	qreal knobD = r.height() - 4;
	qreal travel = r.width() - knobD - 4;
	QRectF knob(r.left() + 2 + travel * position, r.top() + 2, knobD, knobD);
	p.setBrush(isEnabled() ? QColor("#f4f5f7") : QColor("#6b707a"));
	p.drawEllipse(knob);

	if (hasFocus()) {
		p.setPen(QPen(QColor(255, 255, 255, 110), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r, radius, radius);
	}
}
