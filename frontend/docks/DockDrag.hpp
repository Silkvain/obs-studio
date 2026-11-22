/******************************************************************************
    Siegfried: panel (dock) dragging that works on Wayland.

    Qt drags a panel by turning it into its own window and moving that window
    under the cursor. Wayland doesn't let apps place windows or see where the
    cursor is on screen, so on Wayland the panel stays put and can't be
    docked back (QTBUG-87332).

    This replaces that with dragging inside the main window: a see-through
    copy of the panel follows the mouse, the spot it will land in is
    highlighted, and on release it is docked there. Released outside the
    window it becomes a floating window; floating windows are moved by the
    compositor (xdg_toplevel move), and double-clicking the title docks them
    back.

    Used on Wayland only; X11, Windows and macOS keep Qt's own dragging.
    SIEGFRIED_DOCK_DRAG=1 forces it on (for testing), =0 forces it off.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>

class QDockWidget;
class QLabel;
class QMainWindow;
class QMouseEvent;
class QWidget;

class DockDrag : public QObject {
	Q_OBJECT

public:
	/* Installs on every dock of `window`, including ones added later */
	static void Install(QMainWindow *window);
	static bool Wanted();

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	explicit DockDrag(QMainWindow *window);

	enum class Drop { None, Area, Split, Tab, Float };
	struct Target {
		Drop kind = Drop::None;
		QDockWidget *dock = nullptr; /* split/tab next to this one */
		Qt::DockWidgetArea area = Qt::NoDockWidgetArea;
		Qt::Orientation orientation = Qt::Horizontal;
		bool before = false; /* left/top of `dock` */
		QRect highlight;     /* in window coordinates */
	};

	void Watch(QDockWidget *dock);
	bool InTitle(QDockWidget *dock, const QPoint &pos) const;
	QPoint ToWindow(QWidget *from, const QPointF &pos) const;
	Target FindTarget(const QPoint &pos) const;
	void Begin(QDockWidget *dock, const QPoint &windowPos);
	void Move(const QPoint &windowPos);
	void Finish(const QPoint &windowPos);
	void Cancel();
	void Apply(QDockWidget *dock, const Target &target);

	QMainWindow *window;

	QPointer<QDockWidget> pressed;
	QPoint pressPos; /* window coordinates */
	QPoint grabOffset;

	QPointer<QDockWidget> dragging;
	QLabel *ghost = nullptr;
	QWidget *highlight = nullptr;
	Target current;
};
