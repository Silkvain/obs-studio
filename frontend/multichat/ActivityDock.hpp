/******************************************************************************
    Siegfried: Activity panel.

    Follows, subs, gifted subs, bits, raids, Super Chats, Rants... from
    every platform, newest first, with a filter per platform. Right-click
    an event to play its alert again. "Silence" stops alerts from playing
    (events still show here) for when chat gets mischievous.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatConnector.hpp"

#include <QTimer>
#include <QWidget>

#include <optional>

class ChatHub;
class QButtonGroup;
class QListWidget;
class QToolButton;

class ActivityDock : public QWidget {
	Q_OBJECT

public:
	ActivityDock(QWidget *parent, ChatHub *hub);

private:
	void Rebuild();
	void AddRow(const StreamEvent &e, bool atTop);
	bool Visible(const StreamEvent &e) const;
	void ShowMenu();
	void ShowRowMenu(const QPoint &pos);
	void UpdateSilence(bool paused);
	void RefreshTimes();

	ChatHub *hub;
	QListWidget *list;
	QButtonGroup *filters;
	QToolButton *silence;
	QToolButton *gear;
	std::optional<ChatPlatform> platformFilter;
	QTimer timesTimer;
};

/* "3m", "2h", "4d", "2mo", "1y" */
QString ActivityTimeAgo(const QDateTime &t);
