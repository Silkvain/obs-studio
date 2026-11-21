/******************************************************************************
    Multichat dock. See MultichatDock.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultichatDock.hpp"
#include "KickChat.hpp"
#include "ToggleSwitch.hpp"

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "moc_MultichatDock.cpp"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */

namespace {
constexpr int kMaxFeedLines = 1000;

const char *BadgeLetter(ChatPlatform p)
{
	switch (p) {
	case ChatPlatform::Twitch:
		return "T";
	case ChatPlatform::Kick:
		return "K";
	case ChatPlatform::YouTube:
		return "Y";
	case ChatPlatform::Rumble:
		return "R";
	}
	return "?";
}

QString BadgeUrl(ChatPlatform p)
{
	return QStringLiteral("badge:") + QString::fromUtf8(ChatPlatformKey(p));
}

/* Usernames must stay readable on a dark chat background */
QString ReadableColor(const ChatUser &u)
{
	QColor c(u.color);
	if (!c.isValid()) {
		static const char *palette[] = {"#ff7a7a", "#ffb86b", "#ffe66b", "#8aff80", "#6bd8ff",
						"#8aa4ff", "#c38aff", "#ff8ad8", "#5effc8", "#ffa0a0"};
		c = QColor(palette[qHash(u.Key()) % std::size(palette)]);
	}
	float h, s, l, a;
	c.getHslF(&h, &s, &l, &a);
	if (l < 0.6f) {
		c.setHslF(h, s, 0.6f, a);
	}
	return c.name();
}

QString EncodeKey(const QString &key)
{
	return QString::fromLatin1(QUrl::toPercentEncoding(key));
}

QString MessageHtml(const ChatMessage &m, bool withTime)
{
	QString html;
	html += QStringLiteral("<img src=\"%1\" width=\"16\" height=\"16\"> ").arg(BadgeUrl(m.platform));
	if (withTime) {
		html += QStringLiteral("<span style=\"color:#7f8796\">%1</span> ").arg(m.time.toString("HH:mm"));
	}
	if (m.isBroadcaster) {
		html += QStringLiteral("<span style=\"color:#e0b44c\" title=\"Broadcaster\">★</span>");
	} else if (m.isModerator) {
		html += QStringLiteral("<span style=\"color:#4cc06a\" title=\"Moderator\">⚔</span>");
	}
	html += QStringLiteral("<a href=\"user:%1\" style=\"color:%2;text-decoration:none;font-weight:600\">%3</a>")
			.arg(EncodeKey(m.user.Key()), ReadableColor(m.user), m.user.displayName.toHtmlEscaped());
	html += QStringLiteral("<span style=\"color:#7f8796\">:</span> ");
	if (!m.highlight.isEmpty()) {
		html += QStringLiteral("<span style=\"background-color:#4a3b00;color:#ffd24a\">&nbsp;%1&nbsp;</span> ")
				.arg(m.highlight.toHtmlEscaped());
	}
	html += m.text.toHtmlEscaped();
	return html;
}

QString NoticeHtml(ChatPlatform p, const QString &text, bool error)
{
	return QStringLiteral("<img src=\"%1\" width=\"16\" height=\"16\"> <i style=\"color:%2\">%3</i>")
		.arg(BadgeUrl(p), error ? "#ff8a80" : "#9aa3b2", text.toHtmlEscaped());
}
} // namespace

QPixmap ChatBadge(ChatPlatform p, int size)
{
	/* A neutral letter tile in the platform's colour. Swap in official
	 * brand icons from each platform's press kit if you like. */
	const qreal dpr = 2.0;
	QPixmap pm(int(size * dpr), int(size * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter painter(&pm);
	painter.setRenderHint(QPainter::Antialiasing);
	QColor bg(ChatPlatformColor(p));
	painter.setBrush(bg);
	painter.setPen(Qt::NoPen);
	painter.drawRoundedRect(QRectF(0, 0, size, size), size * 0.22, size * 0.22);
	QFont f = painter.font();
	f.setPixelSize(int(size * 0.68));
	f.setBold(true);
	painter.setFont(f);
	painter.setPen(bg.lightnessF() > 0.6 ? QColor("#0b0e14") : QColor(Qt::white));
	painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QString::fromLatin1(BadgeLetter(p)));
	return pm;
}

/* ------------------------------------------------------------------------ */
/* ChatView                                                                 */

ChatView::ChatView(QWidget *parent) : QTextBrowser(parent)
{
	setOpenLinks(false);
	setOpenExternalLinks(false);
	document()->setMaximumBlockCount(kMaxFeedLines);
	document()->setDocumentMargin(6);
	for (int i = 0; i < kChatPlatformCount; i++) {
		ChatPlatform p = ChatPlatform(i);
		document()->addResource(QTextDocument::ImageResource, QUrl(BadgeUrl(p)), ChatBadge(p, 16).toImage());
	}
	connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
		QString s = url.toString(QUrl::FullyEncoded);
		if (s.startsWith("user:")) {
			emit UserRequested(QUrl::fromPercentEncoding(s.mid(5).toLatin1()));
		}
	});
}

void ChatView::AppendHtml(const QString &html)
{
	QScrollBar *bar = verticalScrollBar();
	bool atBottom = bar->value() >= bar->maximum() - 4;
	append(html);
	if (atBottom) {
		bar->setValue(bar->maximum());
	}
}

void ChatView::contextMenuEvent(QContextMenuEvent *event)
{
	QString anchor = anchorAt(event->pos());
	if (anchor.startsWith("user:")) {
		emit UserRequested(QUrl::fromPercentEncoding(anchor.mid(5).toLatin1()));
		event->accept();
		return;
	}
	QTextBrowser::contextMenuEvent(event);
}

/* ------------------------------------------------------------------------ */
/* User card                                                                */

ChatUserCard::ChatUserCard(ChatHub *hub_, const ChatUser &user_, QWidget *parent)
	: QWidget(parent, Qt::Window | Qt::Tool),
	  hub(hub_),
	  user(user_)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("%1 on %2").arg(user.displayName, ChatPlatformName(user.platform)));
	resize(420, 460);

	QVBoxLayout *layout = new QVBoxLayout(this);

	/* Header */
	QHBoxLayout *header = new QHBoxLayout();
	QLabel *badge = new QLabel(this);
	badge->setPixmap(ChatBadge(user.platform, 28));
	header->addWidget(badge);
	QVBoxLayout *names = new QVBoxLayout();
	names->setSpacing(0);
	QLabel *name = new QLabel(user.displayName, this);
	QFont f = name->font();
	f.setPointSizeF(f.pointSizeF() * 1.3);
	f.setBold(true);
	name->setFont(f);
	name->setStyleSheet(QStringLiteral("color:%1").arg(ReadableColor(user)));
	name->setTextInteractionFlags(Qt::TextSelectableByMouse);
	names->addWidget(name);
	count = new QLabel(this);
	count->setStyleSheet("color:#9aa3b2");
	names->addWidget(count);
	header->addLayout(names, 1);

	if (user.platform == ChatPlatform::Twitch || user.platform == ChatPlatform::Kick) {
		QPushButton *full = new QPushButton(tr("Full history ↗"), this);
		full->setToolTip(user.platform == ChatPlatform::Twitch
					 ? tr("Open Twitch's own mod card, with this user's complete chat log")
					 : tr("Open this user's profile on kick.com"));
		connect(full, &QPushButton::clicked, this, [this] {
			ChatConnector *c = hub->Connector(user.platform);
			QString channel = c->Config().value("channel").toString().trimmed().toLower();
			QString login = user.login.isEmpty() ? user.displayName : user.login;
			QUrl url = user.platform == ChatPlatform::Twitch
					   ? QUrl(QStringLiteral("https://www.twitch.tv/popout/%1/viewercard/%2")
							  .arg(channel, login.toLower()))
					   : QUrl(QStringLiteral("https://kick.com/%1").arg(login.toLower()));
			QDesktopServices::openUrl(url);
		});
		header->addWidget(full);
	}
	layout->addLayout(header);

	/* Messages from this user, this session */
	QLabel *logTitle = new QLabel(tr("Messages this session"), this);
	logTitle->setStyleSheet("color:#9aa3b2");
	layout->addWidget(logTitle);
	log = new ChatView(this);
	layout->addWidget(log, 1);

	/* Moderation */
	reason = new QLineEdit(this);
	reason->setPlaceholderText(tr("Reason (optional)"));
	layout->addWidget(reason);

	QHBoxLayout *actions = new QHBoxLayout();
	timeoutButton = new QToolButton(this);
	timeoutButton->setText(tr("Timeout"));
	timeoutButton->setPopupMode(QToolButton::InstantPopup);
	timeoutButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
	timeoutButton->setMinimumHeight(28);
	QMenu *menu = new QMenu(timeoutButton);
	const QList<std::pair<QString, int>> durations{{tr("1 minute"), 60},
						       {tr("10 minutes"), 600},
						       {tr("1 hour"), 3600},
						       {tr("24 hours"), 86400},
						       {tr("7 days"), 604800}};
	for (const auto &d : durations) {
		int secs = d.second;
		menu->addAction(d.first, this, [this, secs] { Moderate(secs); });
	}
	timeoutButton->setMenu(menu);
	actions->addWidget(timeoutButton);

	banButton = new QPushButton(tr("Ban"), this);
	banButton->setStyleSheet("QPushButton:enabled { color: #ff8a80; }");
	connect(banButton, &QPushButton::clicked, this, [this] { Moderate(0); });
	actions->addWidget(banButton);

	unbanButton = new QPushButton(tr("Unban"), this);
	connect(unbanButton, &QPushButton::clicked, this, [this] {
		hub->Connector(user.platform)->Unban(user);
		status->setText(tr("Working…"));
	});
	actions->addWidget(unbanButton);
	actions->addStretch();
	layout->addLayout(actions);

	modHint = new QLabel(this);
	modHint->setWordWrap(true);
	modHint->setStyleSheet("color:#9aa3b2");
	layout->addWidget(modHint);

	status = new QLabel(this);
	status->setWordWrap(true);
	layout->addWidget(status);

	for (const ChatMessage &m : hub->History(user.Key())) {
		AddMessage(m);
	}
	count->setText(tr("%1 · %2 messages this session")
			       .arg(ChatPlatformName(user.platform))
			       .arg(hub->MessageCount(user.Key())));

	connect(hub, &ChatHub::StateChanged, this, &ChatUserCard::RefreshActions);
	RefreshActions();
}

void ChatUserCard::AddMessage(const ChatMessage &m)
{
	QString html = QStringLiteral("<span style=\"color:#7f8796\">%1</span> ").arg(m.time.toString("HH:mm:ss"));
	if (!m.highlight.isEmpty()) {
		html += QStringLiteral("<span style=\"background-color:#4a3b00;color:#ffd24a\">&nbsp;%1&nbsp;</span> ")
				.arg(m.highlight.toHtmlEscaped());
	}
	html += m.text.toHtmlEscaped();
	log->AppendHtml(html);
	count->setText(tr("%1 · %2 messages this session")
			       .arg(ChatPlatformName(user.platform))
			       .arg(hub->MessageCount(user.Key())));
}

void ChatUserCard::ShowNotice(const QString &text, bool error)
{
	status->setStyleSheet(error ? "color:#ff8a80" : "color:#8be28b");
	status->setText(text);
}

void ChatUserCard::RefreshActions()
{
	ChatConnector *c = hub->Connector(user.platform);
	bool can = c->CanModerate() && !user.id.isEmpty();
	timeoutButton->setEnabled(can);
	banButton->setEnabled(can);
	unbanButton->setEnabled(can);
	reason->setEnabled(can);

	if (user.platform == ChatPlatform::Rumble) {
		modHint->setText(tr("Rumble's API doesn't allow moderation from other apps. Moderate on rumble.com."));
	} else if (!c->LoggedIn()) {
		modHint->setText(
			tr("Log in to %1 in Chat Accounts (⚙) to moderate.").arg(ChatPlatformName(user.platform)));
	} else if (!can) {
		modHint->setText(
			tr("Moderation becomes available once you're live on %1.").arg(ChatPlatformName(user.platform)));
	} else {
		modHint->clear();
	}
	modHint->setVisible(!modHint->text().isEmpty());
}

void ChatUserCard::Moderate(int seconds)
{
	ChatConnector *c = hub->Connector(user.platform);
	if (seconds == 0) {
		QMessageBox::StandardButton res =
			QMessageBox::question(this, tr("Ban %1?").arg(user.displayName),
					      tr("Permanently ban %1 from your %2 chat?")
						      .arg(user.displayName, ChatPlatformName(user.platform)));
		if (res != QMessageBox::Yes) {
			return;
		}
		c->Ban(user, reason->text().trimmed());
	} else {
		c->Timeout(user, seconds, reason->text().trimmed());
	}
	status->setStyleSheet("");
	status->setText(tr("Working…"));
}

/* ------------------------------------------------------------------------ */
/* Accounts dialog                                                          */

ChatAccountsDialog::ChatAccountsDialog(ChatHub *hub_, QWidget *parent) : QDialog(parent), hub(hub_)
{
	setWindowTitle(tr("Chat Accounts"));
	setMinimumWidth(520);
	QVBoxLayout *layout = new QVBoxLayout(this);
	QTabWidget *tabWidget = new QTabWidget(this);

	tabWidget->addTab(BuildTab(ChatPlatform::Twitch, {{"channel", tr("Channel"), tr("your own channel"), false}},
				   {{"client_id", tr("Client ID"), tr("built in"), false}},
				   tr("Only needed if you want to use your own Twitch developer app."), true),
			  QIcon(ChatBadge(ChatPlatform::Twitch, 16)), "Twitch");

	tabWidget->addTab(BuildTab(ChatPlatform::Kick, {{"channel", tr("Channel"), tr("your own channel"), false}},
				   {{"chatroom_id", tr("Chatroom ID"), tr("found automatically"), false},
				    {"client_id", tr("Client ID"), tr("built in"), false},
				    {"client_secret", tr("Client Secret"), tr("built in"), true}},
				   tr("Chatroom ID: only if chat doesn't load. Open kick.com/api/v2/channels/<channel> "
				      "in a browser and copy \"chatroom\" → \"id\". Client ID/Secret: only for your "
				      "own Kick app (redirect http://localhost:%1/callback).")
					   .arg(KickChat::kRedirectPort),
				   true),
			  QIcon(ChatBadge(ChatPlatform::Kick, 16)), "Kick");

	tabWidget->addTab(BuildTab(ChatPlatform::YouTube, {},
				   {{"client_id", tr("Client ID"), tr("built in"), false},
				    {"client_secret", tr("Client Secret"), tr("built in"), true}},
				   tr("Only needed for your own Google OAuth client (type: Desktop app, with the "
				      "YouTube Data API v3 enabled)."),
				   true),
			  QIcon(ChatBadge(ChatPlatform::YouTube, 16)), "YouTube");

	tabWidget->addTab(BuildTab(ChatPlatform::Rumble,
				   {{"api_url", tr("Live Stream API URL"), tr("paste it here"), true}}, {}, {}, false),
			  QIcon(ChatBadge(ChatPlatform::Rumble, 16)), "Rumble");

	layout->addWidget(tabWidget);

	QHBoxLayout *buttons = new QHBoxLayout();
	buttons->addStretch();
	QPushButton *close = new QPushButton(tr("Save && Close"), this);
	close->setDefault(true);
	connect(close, &QPushButton::clicked, this, [this] {
		for (int i = 0; i < kChatPlatformCount; i++) {
			Apply(ChatPlatform(i));
		}
		hub->Save();
		accept();
	});
	buttons->addWidget(close);
	layout->addLayout(buttons);

	connect(hub, &ChatHub::StateChanged, this, &ChatAccountsDialog::RefreshStatus);
	connect(hub, &ChatHub::Notice, this, [this](ChatPlatform p, const QString &text, bool error) {
		auto it = tabs.find(int(p));
		if (it != tabs.end() && it->second.status) {
			it->second.status->setStyleSheet(error ? "color:#ff8a80" : "color:#8be28b");
			it->second.status->setText(text);
		}
	});
	for (ChatConnector *c : hub->Connectors()) {
		ChatPlatform p = c->Platform();
		connect(c, &ChatConnector::ConfigChanged, this, [this, p] { SyncFromConfig(p); });
	}
}

QWidget *ChatAccountsDialog::BuildTab(ChatPlatform p, const QList<Field> &main, const QList<Field> &advanced,
				      const QString &advancedHelp, bool hasLogin)
{
	ChatConnector *c = hub->Connector(p);
	Tab &tab = tabs[int(p)];
	QString name = ChatPlatformName(p);

	QWidget *page = new QWidget();
	QVBoxLayout *v = new QVBoxLayout(page);
	v->setSpacing(10);

	auto makeEdit = [&](const Field &f) {
		QLineEdit *e = new QLineEdit(c->Config().value(f.key).toString(), page);
		e->setPlaceholderText(f.placeholder);
		if (f.secret) {
			e->setEchoMode(QLineEdit::PasswordEchoOnEdit);
		}
		tab.edits[f.key] = e;
		return e;
	};

	if (hasLogin) {
		/* Account: who you're connected as, and one button */
		QHBoxLayout *row = new QHBoxLayout();
		QLabel *badge = new QLabel(page);
		badge->setPixmap(ChatBadge(p, 32));
		row->addWidget(badge);
		tab.account = new QLabel(page);
		QFont f = tab.account->font();
		f.setPointSizeF(f.pointSizeF() * 1.15);
		f.setBold(true);
		tab.account->setFont(f);
		row->addWidget(tab.account, 1);
		tab.connect = new QPushButton(page);
		tab.connect->setMinimumWidth(150);
		tab.connect->setMinimumHeight(32);
		connect(tab.connect, &QPushButton::clicked, this, [this, p] {
			ChatConnector *conn = hub->Connector(p);
			if (conn->LoggedIn()) {
				conn->Logout();
				tabs[int(p)].status->clear();
				RefreshStatus(p);
				return;
			}
			Apply(p);
			hub->Save();
			QString req = conn->LoginRequirement();
			if (!req.isEmpty()) {
				tabs[int(p)].status->setStyleSheet("color:#ff8a80");
				tabs[int(p)].status->setText(req);
				return;
			}
			tabs[int(p)].status->setStyleSheet("");
			tabs[int(p)].status->setText(tr("Finish connecting in your browser…"));
			conn->Login(this);
		});
		row->addWidget(tab.connect);
		v->addLayout(row);
	}

	tab.enabled = new QCheckBox(tr("Show %1 chat").arg(name), page);
	tab.enabled->setChecked(c->Enabled());
	v->addWidget(tab.enabled);

	if (!main.isEmpty()) {
		QFormLayout *form = new QFormLayout();
		for (const Field &f : main) {
			form->addRow(f.label, makeEdit(f));
		}
		v->addLayout(form);
	}

	if (p == ChatPlatform::Rumble) {
		QLabel *how =
			new QLabel(tr("<a href=\"https://rumble.com/account/livestream-api\">Get your Live Stream "
				      "API URL from Rumble</a>, then paste it above. Rumble only lets apps "
				      "read chat, so sending and moderation aren't available."),
				   page);
		how->setOpenExternalLinks(true);
		how->setWordWrap(true);
		how->setStyleSheet("color:#9aa3b2");
		v->addWidget(how);
	}

	tab.status = new QLabel(page);
	tab.status->setWordWrap(true);
	v->addWidget(tab.status);
	v->addStretch();

	if (!advanced.isEmpty()) {
		QToolButton *toggle = new QToolButton(page);
		toggle->setText(tr("Advanced"));
		toggle->setCheckable(true);
		toggle->setAutoRaise(true);
		toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		toggle->setArrowType(Qt::RightArrow);
		QWidget *box = new QWidget(page);
		QVBoxLayout *bl = new QVBoxLayout(box);
		bl->setContentsMargins(16, 0, 0, 0);
		QFormLayout *form = new QFormLayout();
		for (const Field &f : advanced) {
			form->addRow(f.label, makeEdit(f));
		}
		bl->addLayout(form);
		QLabel *help = new QLabel(advancedHelp, box);
		help->setWordWrap(true);
		help->setStyleSheet("color:#9aa3b2");
		bl->addWidget(help);
		/* Open Advanced only when this build can't connect on its own */
		bool open = hasLogin && !c->LoginRequirement().isEmpty() && !c->LoggedIn();
		box->setVisible(open);
		toggle->setChecked(open);
		toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
		connect(toggle, &QToolButton::toggled, box, [box, toggle](bool on) {
			box->setVisible(on);
			toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
		});
		v->addWidget(toggle);
		v->addWidget(box);
	}

	RefreshStatus(p);
	return page;
}

void ChatAccountsDialog::Apply(ChatPlatform p)
{
	ChatConnector *c = hub->Connector(p);
	Tab &tab = tabs[int(p)];
	QJsonObject &cfg = c->Config();
	QJsonObject before = cfg;

	cfg["enabled"] = tab.enabled->isChecked();
	for (auto &[key, edit] : tab.edits) {
		cfg[key] = edit->text().trimmed();
	}

	/* No channel typed: use the connected account's own channel */
	if (cfg.value("channel").toString().isEmpty() && !cfg.value("user_login").toString().isEmpty() &&
	    tab.edits.count("channel")) {
		cfg["channel"] = cfg.value("user_login");
	}

	/* A different channel invalidates cached lookups */
	if (before.value("channel").toString() != cfg.value("channel").toString()) {
		cfg.remove("chatroom_id_cached");
		if (cfg.value("channel").toString().toLower() != cfg.value("user_login").toString()) {
			cfg.remove("broadcaster_user_id");
		}
	}

	if (before != cfg) {
		c->Restart();
	}
}

void ChatAccountsDialog::SyncFromConfig(ChatPlatform p)
{
	/* A login just finished: show what it set (channel, chat switched on) */
	auto it = tabs.find(int(p));
	if (it == tabs.end()) {
		return;
	}
	Tab &tab = it->second;
	ChatConnector *c = hub->Connector(p);
	if (c->Enabled() && !tab.enabled->isChecked()) {
		tab.enabled->setChecked(true);
	}
	auto ch = tab.edits.find("channel");
	if (ch != tab.edits.end() && ch->second->text().trimmed().isEmpty()) {
		ch->second->setText(c->Config().value("channel").toString());
	}
	RefreshStatus(p);
}

void ChatAccountsDialog::RefreshStatus(ChatPlatform p)
{
	auto it = tabs.find(int(p));
	if (it == tabs.end()) {
		return;
	}
	Tab &tab = it->second;
	ChatConnector *c = hub->Connector(p);
	if (tab.account) {
		QString who = c->Config().value("user_name").toString();
		if (c->LoggedIn()) {
			tab.account->setText(who.isEmpty() ? tr("Connected") : tr("Connected as %1").arg(who));
			tab.connect->setText(tr("Disconnect"));
		} else {
			tab.account->setText(tr("Not connected"));
			tab.connect->setText(tr("Connect %1").arg(ChatPlatformName(p)));
		}
	}
	if (tab.status && tab.status->text().isEmpty() && !c->StateText().isEmpty()) {
		tab.status->setText(c->StateText());
	}
}

/* ------------------------------------------------------------------------ */
/* Dock                                                                     */

MultichatDock::MultichatDock(QWidget *parent, ChatHub *hub_) : QWidget(parent), hub(hub_)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	view = new ChatView(this);
	view->setStyleSheet("QTextBrowser { background-color: #0e1117; color: #e6e8ee; border: none; }");
	connect(view, &ChatView::UserRequested, this, &MultichatDock::OpenUserCard);
	layout->addWidget(view, 1);

	QHBoxLayout *bottom = new QHBoxLayout();
	input = new QLineEdit(this);
	input->setMaxLength(500);
	connect(input, &QLineEdit::returnPressed, this, &MultichatDock::SendClicked);
	bottom->addWidget(input, 1);
	sendButton = new QPushButton(tr("Chat"), this);
	connect(sendButton, &QPushButton::clicked, this, &MultichatDock::SendClicked);
	bottom->addWidget(sendButton);
	QToolButton *settings = new QToolButton(this);
	settings->setText(QStringLiteral("⚙"));
	settings->setToolTip(tr("Chat Accounts"));
	connect(settings, &QToolButton::clicked, this, &MultichatDock::OpenAccounts);
	bottom->addWidget(settings);
	layout->addLayout(bottom);

	connect(hub, &ChatHub::MessageReceived, this, &MultichatDock::OnMessage);
	connect(hub, &ChatHub::Notice, this, &MultichatDock::OnNotice);
	connect(hub, &ChatHub::StateChanged, this, &MultichatDock::RefreshPlatforms);

	RefreshPlatforms();

	bool any = false;
	for (ChatConnector *c : hub->Connectors()) {
		any |= c->Enabled();
	}
	if (!any) {
		view->AppendHtml(
			QStringLiteral("<i style=\"color:#9aa3b2\">%1</i>")
				.arg(tr("No chats connected yet. Click ⚙ to add Twitch, Kick, YouTube or Rumble. "
					"Right-click any username to see their messages and timeout or ban them.")
					     .toHtmlEscaped()));
	}
}

void MultichatDock::OnMessage(const ChatMessage &msg)
{
	view->AppendHtml(MessageHtml(msg, false));
	auto it = cards.find(msg.user.Key());
	if (it != cards.end() && it->second) {
		it->second->AddMessage(msg);
	}
}

void MultichatDock::OnNotice(ChatPlatform p, const QString &text, bool error)
{
	view->AppendHtml(NoticeHtml(p, text, error));
	for (auto &[key, card] : cards) {
		if (card && card->User().platform == p) {
			card->ShowNotice(text, error);
		}
	}
}

void MultichatDock::RefreshPlatforms()
{
	QStringList targets;
	for (ChatConnector *c : hub->Connectors()) {
		if (c->Enabled() && c->SendTarget() && c->CanSend()) {
			targets << ChatPlatformName(c->Platform());
		}
	}

	input->setEnabled(!targets.isEmpty());
	sendButton->setEnabled(!targets.isEmpty());
	input->setPlaceholderText(targets.isEmpty() ? tr("Connect a platform (⚙) and switch it on in Text to send")
						    : tr("Send to %1").arg(targets.join(QStringLiteral(" + "))));
}

void MultichatDock::SendClicked()
{
	QString text = input->text();
	if (text.trimmed().isEmpty()) {
		return;
	}
	if (!hub->Send(text).isEmpty()) {
		input->clear();
	}
}

void MultichatDock::OpenUserCard(const QString &userKey)
{
	auto it = cards.find(userKey);
	if (it != cards.end() && it->second) {
		it->second->show();
		it->second->raise();
		it->second->activateWindow();
		return;
	}
	ChatUser user = hub->KnownUser(userKey);
	if (user.id.isEmpty()) {
		return;
	}
	ChatUserCard *card = new ChatUserCard(hub, user, window());
	cards[userKey] = card;
	card->move(QCursor::pos() - QPoint(card->width() / 2, 40));
	card->show();
}

void MultichatDock::OpenAccounts()
{
	ShowAccounts(hub, this);
	RefreshPlatforms();
}

void MultichatDock::ShowAccounts(ChatHub *hub, QWidget *parent)
{
	ChatAccountsDialog dlg(hub, parent);
	dlg.exec();
}

/* ------------------------------------------------------------------------ */
/* Chat sending panel                                                       */

ChatTargetsPanel::ChatTargetsPanel(QWidget *parent, ChatHub *hub_) : QWidget(parent), hub(hub_)
{
	/* Deliberately tiny: an icon and a switch per platform, details on hover */
	setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
	setMinimumWidth(96); /* room for the dock title */

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(6);

	rowsLayout = new QVBoxLayout();
	rowsLayout->setSpacing(6);
	layout->addLayout(rowsLayout);

	empty = new QLabel(tr("No chats"), this);
	empty->setStyleSheet("color:#9aa3b2");
	layout->addWidget(empty);
	layout->addStretch();

	QToolButton *accounts = new QToolButton(this);
	accounts->setText(QStringLiteral("⚙"));
	accounts->setToolTip(tr("Chat Accounts"));
	accounts->setAutoRaise(true);
	connect(accounts, &QToolButton::clicked, this, [this] {
		MultichatDock::ShowAccounts(hub, this);
		Rebuild();
	});
	layout->addWidget(accounts, 0, Qt::AlignRight);

	connect(hub, &ChatHub::StateChanged, this, &ChatTargetsPanel::Refresh);
	Rebuild();
}

void ChatTargetsPanel::Rebuild()
{
	for (auto &[i, row] : rows) {
		row.widget->deleteLater();
	}
	rows.clear();
	enabledSignature.clear();

	for (ChatConnector *c : hub->Connectors()) {
		enabledSignature += c->Enabled() ? '1' : '0';
		if (!c->Enabled()) {
			continue;
		}
		ChatPlatform p = c->Platform();

		Row row;
		row.widget = new QWidget(this);
		QHBoxLayout *h = new QHBoxLayout(row.widget);
		h->setContentsMargins(0, 0, 0, 0);
		h->setSpacing(6);

		row.badge = new QLabel(row.widget);
		row.badge->setPixmap(ChatBadge(p, 18));
		h->addWidget(row.badge);

		row.toggle = new ToggleSwitch(row.widget);
		connect(row.toggle, &ToggleSwitch::toggled, this,
			[this, p](bool on) { hub->Connector(p)->SetSendTarget(on); });
		h->addWidget(row.toggle);

		rowsLayout->addWidget(row.widget);
		rows[int(p)] = row;
	}

	empty->setVisible(rows.empty());
	Refresh();
}

void ChatTargetsPanel::Refresh()
{
	/* Platforms switched on/off in Chat Accounts: rebuild the list */
	QString sig;
	for (ChatConnector *c : hub->Connectors()) {
		sig += c->Enabled() ? '1' : '0';
	}
	if (sig != enabledSignature) {
		Rebuild();
		return;
	}

	for (auto &[i, row] : rows) {
		ChatConnector *c = hub->Connector(ChatPlatform(i));
		QString name = ChatPlatformName(c->Platform());
		bool connected = c->State() == ChatState::Connected;
		row.badge->setPixmap(
			connected ? ChatBadge(c->Platform(), 18)
				  : QIcon(ChatBadge(c->Platform(), 18)).pixmap(QSize(18, 18), QIcon::Disabled));

		bool canSend = c->CanSend();
		QString status;
		if (c->Platform() == ChatPlatform::Rumble) {
			status = tr("%1: read-only, Rumble's API can't send messages").arg(name);
		} else if (!c->LoggedIn()) {
			status = tr("%1: log in (⚙) to send messages").arg(name);
		} else if (!canSend) {
			status = tr("%1: %2").arg(name, c->StateText().isEmpty() ? tr("offline") : c->StateText());
		} else {
			status = c->SendTarget() ? tr("%1: your messages are sent here").arg(name)
						 : tr("%1: not sending here").arg(name);
		}
		row.widget->setToolTip(status);
		row.toggle->setToolTip(status);

		row.toggle->setEnabled(canSend);
		bool want = canSend && c->SendTarget();
		if (row.toggle->isChecked() != want) { /* don't cut the user's own slide animation short */
			row.toggle->SetCheckedSilently(want);
		}
	}
}
