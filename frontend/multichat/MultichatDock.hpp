/******************************************************************************
    Multichat dock: one feed for every platform, a send box that posts to
    every platform you toggle on, and a right-click user card with that
    viewer's messages plus timeout / ban / unban.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ChatHub.hpp"

#include <QDialog>
#include <QPixmap>
#include <QPointer>
#include <QTextBrowser>
#include <QWidget>

#include <map>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QCheckBox;

QPixmap ChatBadge(ChatPlatform p, int size);

/* QTextBrowser that reports right/left clicks on usernames */
class ChatView : public QTextBrowser {
	Q_OBJECT

public:
	explicit ChatView(QWidget *parent);
	void AppendHtml(const QString &html);

signals:
	void UserRequested(const QString &userKey);

protected:
	void contextMenuEvent(QContextMenuEvent *event) override;
};

class ChatUserCard : public QWidget {
	Q_OBJECT

public:
	ChatUserCard(ChatHub *hub, const ChatUser &user, QWidget *parent);
	void AddMessage(const ChatMessage &msg);
	void ShowNotice(const QString &text, bool error);
	const ChatUser &User() const { return user; }

private:
	void Moderate(int seconds); /* 0 = permanent ban */
	void RefreshActions();

	ChatHub *hub;
	ChatUser user;
	ChatView *log;
	QLabel *count;
	QLabel *status;
	QLineEdit *reason;
	QToolButton *timeoutButton;
	QPushButton *banButton;
	QPushButton *unbanButton;
	QLabel *modHint;
};

class ChatAccountsDialog : public QDialog {
	Q_OBJECT

public:
	ChatAccountsDialog(ChatHub *hub, QWidget *parent);

private:
	struct Fields {
		QCheckBox *enabled = nullptr;
		std::map<QString, QLineEdit *> edits; /* config key -> field */
		QLabel *status = nullptr;
		QPushButton *login = nullptr;
		QPushButton *logout = nullptr;
	};

	QWidget *BuildTab(ChatPlatform p, const QList<QPair<QString, QString>> &fields, const QString &help,
			  bool hasLogin, const QStringList &secretKeys);
	void Apply(ChatPlatform p);
	void RefreshStatus(ChatPlatform p);

	ChatHub *hub;
	std::map<int, Fields> tabs;
};

class MultichatDock : public QWidget {
	Q_OBJECT

public:
	MultichatDock(QWidget *parent, ChatHub *hub);

private slots:
	void OnMessage(const ChatMessage &msg);
	void OnNotice(ChatPlatform p, const QString &text, bool error);
	void RefreshPlatforms();
	void SendClicked();
	void OpenUserCard(const QString &userKey);
	void OpenAccounts();

public:
	static void ShowAccounts(ChatHub *hub, QWidget *parent);

private:
	ChatHub *hub;
	ChatView *view;
	QLineEdit *input;
	QPushButton *sendButton;
	std::map<QString, QPointer<ChatUserCard>> cards;
};

class ToggleSwitch;

/* "Send chat to": one ON/OFF switch per connected platform */
class ChatTargetsPanel : public QWidget {
	Q_OBJECT

public:
	ChatTargetsPanel(QWidget *parent, ChatHub *hub);

private slots:
	void Rebuild();
	void Refresh();

private:
	struct Row {
		QWidget *widget;
		QLabel *badge;
		ToggleSwitch *toggle;
	};
	ChatHub *hub;
	class QVBoxLayout *rowsLayout;
	QLabel *empty;
	std::map<int, Row> rows;
	QString enabledSignature;
};
