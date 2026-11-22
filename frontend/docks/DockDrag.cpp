/******************************************************************************
    Siegfried: panel (dock) dragging that works on Wayland. See DockDrag.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "DockDrag.hpp"

#include <QApplication>
#include <QChildEvent>
#include <QDockWidget>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QStatusBar>
#include <QWindow>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "moc_DockDrag.cpp"

namespace {

constexpr int kEdge = 36;      /* px from the window edge that docks to that side */
constexpr int kGhostWidth = 260; /* the see-through copy that follows the mouse */
constexpr float kTabZone = 0.3f; /* middle of a panel = put it in a tab there */

} // namespace

bool DockDrag::Wanted()
{
	const char *env = getenv("SIEGFRIED_DOCK_DRAG");
	if (env && *env) {
		return strcmp(env, "0") != 0;
	}
	return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
}

void DockDrag::Install(QMainWindow *window)
{
	if (!Wanted()) {
		return;
	}
	new DockDrag(window);
}

DockDrag::DockDrag(QMainWindow *window_) : QObject(window_), window(window_)
{
	for (QDockWidget *dock : window->findChildren<QDockWidget *>(Qt::FindDirectChildrenOnly)) {
		Watch(dock);
	}
	/* Docks added later (plugins, browser docks) */
	window->installEventFilter(this);

	highlight = new QWidget(window);
	highlight->setObjectName(QStringLiteral("dockDropHighlight"));
	highlight->setAttribute(Qt::WA_TransparentForMouseEvents);
	highlight->setAttribute(Qt::WA_StyledBackground);
	highlight->setStyleSheet(QStringLiteral("#dockDropHighlight { background: rgba(63, 163, 77, 70);"
						" border: 2px solid rgb(63, 163, 77); border-radius: 4px; }"));
	highlight->hide();

	ghost = new QLabel(window);
	ghost->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto *opacity = new QGraphicsOpacityEffect(ghost);
	opacity->setOpacity(0.7);
	ghost->setGraphicsEffect(opacity);
	ghost->hide();
}

void DockDrag::Watch(QDockWidget *dock)
{
	if (dock->property("siegfriedDockDrag").toBool()) {
		return;
	}
	dock->setProperty("siegfriedDockDrag", true);
	dock->installEventFilter(this);
}

/* The title bar: above the panel's contents (the float/close buttons are
 * child widgets and get their own clicks) */
bool DockDrag::InTitle(QDockWidget *dock, const QPoint &pos) const
{
	if (QWidget *title = dock->titleBarWidget()) {
		return title->isVisible() && title->geometry().contains(pos);
	}
	QWidget *content = dock->widget();
	int top = content ? content->geometry().top() : 24;
	return pos.y() >= 0 && pos.y() < top;
}

QPoint DockDrag::ToWindow(QWidget *from, const QPointF &pos) const
{
	return from->mapTo(window, pos.toPoint());
}

DockDrag::Target DockDrag::FindTarget(const QPoint &pos) const
{
	Target t;
	QRect win = window->rect();
	if (!win.contains(pos)) {
		t.kind = Drop::Float;
		return t;
	}

	/* Content area between the menu bar and the status bar */
	QRect inner = win;
	if (QWidget *menu = window->menuWidget()) {
		inner.setTop(menu->geometry().bottom() + 1);
	}
	if (auto *status = window->findChild<QStatusBar *>(QString(), Qt::FindDirectChildrenOnly);
	    status && status->isVisible()) {
		inner.setBottom(status->geometry().top() - 1);
	}

	auto area = [&](Qt::DockWidgetArea a, QRect r) {
		t.kind = Drop::Area;
		t.area = a;
		t.highlight = r;
	};
	int sideW = std::max(inner.width() / 6, 120);
	int sideH = std::max(inner.height() / 5, 100);
	if (pos.x() < inner.left() + kEdge) {
		area(Qt::LeftDockWidgetArea, QRect(inner.left(), inner.top(), sideW, inner.height()));
		return t;
	}
	if (pos.x() > inner.right() - kEdge) {
		area(Qt::RightDockWidgetArea, QRect(inner.right() - sideW, inner.top(), sideW, inner.height()));
		return t;
	}
	if (pos.y() < inner.top() + kEdge / 2) {
		area(Qt::TopDockWidgetArea, QRect(inner.left(), inner.top(), inner.width(), sideH));
		return t;
	}
	if (pos.y() > inner.bottom() - kEdge) {
		area(Qt::BottomDockWidgetArea, QRect(inner.left(), inner.bottom() - sideH, inner.width(), sideH));
		return t;
	}

	/* Over another panel: its middle makes a tab, its sides split it */
	for (QDockWidget *other : window->findChildren<QDockWidget *>(Qt::FindDirectChildrenOnly)) {
		if (other == dragging || !other->isVisible() || other->isFloating()) {
			continue;
		}
		QRect r = other->geometry();
		if (!r.contains(pos)) {
			continue;
		}
		t.dock = other;
		float fx = float(pos.x() - r.left()) / float(std::max(r.width(), 1));
		float fy = float(pos.y() - r.top()) / float(std::max(r.height(), 1));
		if (fx > kTabZone && fx < 1.0f - kTabZone && fy > kTabZone && fy < 1.0f - kTabZone) {
			t.kind = Drop::Tab;
			t.highlight = r;
			return t;
		}
		float dl = fx, dr = 1.0f - fx, dt = fy, db = 1.0f - fy;
		float m = std::min(std::min(dl, dr), std::min(dt, db));
		t.kind = Drop::Split;
		if (m == dl || m == dr) {
			t.orientation = Qt::Horizontal;
			t.before = m == dl;
			int w = r.width() / 2;
			t.highlight = t.before ? QRect(r.left(), r.top(), w, r.height())
					       : QRect(r.right() - w, r.top(), w, r.height());
		} else {
			t.orientation = Qt::Vertical;
			t.before = m == dt;
			int h = r.height() / 2;
			t.highlight = t.before ? QRect(r.left(), r.top(), r.width(), h)
					       : QRect(r.left(), r.bottom() - h, r.width(), h);
		}
		return t;
	}

	return t; /* over the previews: nothing */
}

void DockDrag::Begin(QDockWidget *dock, const QPoint &windowPos)
{
	dragging = dock;

	QPixmap shot = dock->grab();
	if (shot.width() > kGhostWidth) {
		shot = shot.scaledToWidth(kGhostWidth, Qt::SmoothTransformation);
	}
	ghost->setPixmap(shot);
	ghost->resize(shot.size() / shot.devicePixelRatio());
	grabOffset = QPoint(std::min(pressPos.x() - dock->geometry().left(), ghost->width() / 2), 12);

	dock->grabKeyboard(); /* Escape cancels */
	ghost->show();
	ghost->raise();
	Move(windowPos);
}

void DockDrag::Move(const QPoint &windowPos)
{
	ghost->move(windowPos - grabOffset);
	current = FindTarget(windowPos);
	if (current.kind == Drop::None || current.kind == Drop::Float || current.highlight.isEmpty()) {
		highlight->hide();
	} else {
		highlight->setGeometry(current.highlight);
		highlight->show();
		highlight->raise();
	}
	ghost->raise();
	QApplication::changeOverrideCursor(current.kind == Drop::None ? Qt::ForbiddenCursor : Qt::ClosedHandCursor);
}

void DockDrag::Finish(const QPoint &windowPos)
{
	QPointer<QDockWidget> dock = dragging;
	Target target = FindTarget(windowPos);
	Cancel();
	if (dock) {
		Apply(dock, target);
	}
}

void DockDrag::Cancel()
{
	if (dragging) {
		dragging->releaseKeyboard();
	}
	dragging = nullptr;
	pressed = nullptr;
	ghost->hide();
	highlight->hide();
	while (QApplication::overrideCursor()) {
		QApplication::restoreOverrideCursor();
	}
}

void DockDrag::Apply(QDockWidget *dock, const Target &t)
{
	switch (t.kind) {
	case Drop::None:
		return;
	case Drop::Float:
		if (dock->features() & QDockWidget::DockWidgetFloatable) {
			QSize size = dock->size();
			dock->setFloating(true);
			dock->resize(size);
			dock->show();
		}
		return;
	case Drop::Area:
		if (!(dock->allowedAreas() & t.area)) {
			return;
		}
		dock->setFloating(false);
		window->addDockWidget(t.area, dock,
				      (t.area == Qt::LeftDockWidgetArea || t.area == Qt::RightDockWidgetArea)
					      ? Qt::Horizontal
					      : Qt::Vertical);
		break;
	case Drop::Split:
		if (!t.dock || t.dock == dock) {
			return;
		}
		dock->setFloating(false);
		window->splitDockWidget(t.dock, dock, t.orientation);
		if (t.before) {
			/* splitDockWidget puts the new one after; swap to put it before */
			window->splitDockWidget(dock, t.dock, t.orientation);
		}
		break;
	case Drop::Tab:
		if (!t.dock || t.dock == dock) {
			return;
		}
		dock->setFloating(false);
		window->tabifyDockWidget(t.dock, dock);
		break;
	}
	dock->show();
	dock->raise();
}

bool DockDrag::eventFilter(QObject *obj, QEvent *event)
{
	if (obj == window) {
		if (event->type() == QEvent::ChildPolished) {
			auto *child = static_cast<QChildEvent *>(event);
			if (auto *dock = qobject_cast<QDockWidget *>(child->child())) {
				Watch(dock);
			}
		}
		return false;
	}

	auto *dock = qobject_cast<QDockWidget *>(obj);
	if (!dock) {
		return false;
	}

	switch (event->type()) {
	case QEvent::MouseButtonPress: {
		auto *me = static_cast<QMouseEvent *>(event);
		if (me->button() != Qt::LeftButton || !InTitle(dock, me->position().toPoint())) {
			return false;
		}
		if (!(dock->features() & QDockWidget::DockWidgetMovable)) {
			return false; /* docks are locked */
		}
		if (dock->isFloating()) {
			/* Let the compositor move the floating window */
			if (QWindow *w = dock->windowHandle()) {
				w->startSystemMove();
			}
			return true;
		}
		pressed = dock;
		pressPos = ToWindow(dock, me->position());
		return true; /* keep Qt's own drag from starting */
	}
	case QEvent::MouseMove: {
		if (pressed != dock) {
			return false;
		}
		auto *me = static_cast<QMouseEvent *>(event);
		QPoint pos = ToWindow(dock, me->position());
		if (!dragging) {
			if ((pos - pressPos).manhattanLength() < QApplication::startDragDistance()) {
				return true;
			}
			QApplication::setOverrideCursor(Qt::ClosedHandCursor);
			Begin(dock, pos);
		} else {
			Move(pos);
		}
		return true;
	}
	case QEvent::MouseButtonRelease: {
		if (pressed != dock) {
			return false;
		}
		auto *me = static_cast<QMouseEvent *>(event);
		if (dragging) {
			Finish(ToWindow(dock, me->position()));
		} else {
			pressed = nullptr;
		}
		return true;
	}
	case QEvent::KeyPress:
		if (dragging == dock && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
			Cancel();
			return true;
		}
		return false;
	default:
		return false;
	}
}
