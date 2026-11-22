/******************************************************************************
    Siegfried: Activity panel. See ActivityDock.hpp.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ActivityDock.hpp"
#include "ChatHub.hpp"
#include "MultichatDock.hpp"

#include <OBSApp.hpp>

#include <util/config-file.h>

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

#include "moc_ActivityDock.cpp"

namespace {

constexpr const char *kSection = "Activity";

QString TypeColor(StreamEventType t)
{
	switch (t) {
	case StreamEventType::Follow:
		return QStringLiteral("#9146ff");
	case StreamEventType::Subscribe:
	case StreamEventType::Resubscribe:
	case StreamEventType::Membership:
		return QStringLiteral("#e0a400");
	case StreamEventType::GiftSub:
		return QStringLiteral("#e05aa8");
	case StreamEventType::Bits:
		return QStringLiteral("#3a8dde");
	case StreamEventType::Donation:
	case StreamEventType::SuperChat:
	case StreamEventType::Rant:
		return QStringLiteral("#2f9e57");
	case StreamEventType::Raid:
	case StreamEventType::Host:
		return QStringLiteral("#e0662f");
	}
	return QStringLiteral("#666");
}

QStringList HiddenTypes()
{
	const char *v = config_get_string(App()->GetUserConfig(), kSection, "HiddenTypes");
	return v ? QString::fromUtf8(v).split(',', Qt::SkipEmptyParts) : QStringList();
}

void SetHiddenTypes(const QStringList &l)
{
	config_set_string(App()->GetUserConfig(), kSection, "HiddenTypes", l.join(',').toUtf8().constData());
}

} // namespace

QString ActivityTimeAgo(const QDateTime &t)
{
	qint64 s = std::max<qint64>(0, t.secsTo(QDateTime::currentDateTime()));
	if (s < 60) {
		return QStringLiteral("now");
	}
	if (s < 3600) {
		return QStringLiteral("%1m").arg(s / 60);
	}
	if (s < 86400) {
		return QStringLiteral("%1h").arg(s / 3600);
	}
	if (s < 86400 * 30) {
		return QStringLiteral("%1d").arg(s / 86400);
	}
	if (s < 86400 * 365) {
		return QStringLiteral("%1mo").arg(s / (86400 * 30));
	}
	return QStringLiteral("%1y").arg(s / (86400 * 365));
}

ActivityDock::ActivityDock(QWidget *parent, ChatHub *hub_) : QWidget(parent), hub(hub_)
{
	/* Platform filter: All + one per platform, like a segmented control */
	QHBoxLayout *top = new QHBoxLayout();
	top->setContentsMargins(0, 0, 0, 0);
	top->setSpacing(0);
	filters = new QButtonGroup(this);
	filters->setExclusive(true);
	auto addFilter = [&](int id, const QString &text, const QIcon &icon, const QString &tip) {
		QToolButton *b = new QToolButton(this);
		b->setCheckable(true);
		b->setText(text);
		b->setIcon(icon);
		b->setToolTip(tip);
		b->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextOnly);
		b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		b->setMinimumHeight(28);
		filters->addButton(b, id);
		top->addWidget(b);
		return b;
	};
	addFilter(-1, QTStr("Activity.All"), QIcon(), QTStr("Activity.All"))->setChecked(true);
	for (int i = 0; i < kChatPlatformCount; i++) {
		ChatPlatform p = ChatPlatform(i);
		addFilter(i, QString(), QIcon(ChatBadge(p, 16)), ChatPlatformName(p));
	}
	top->addSpacing(6);

	silence = new QToolButton(this);
	silence->setCheckable(true);
	silence->setMinimumHeight(28);
	silence->setToolTip(QTStr("Activity.SilenceTip"));
	top->addWidget(silence);

	gear = new QToolButton(this);
	gear->setText(QStringLiteral("⚙"));
	gear->setMinimumHeight(28);
	gear->setToolTip(QTStr("Activity.Options"));
	top->addWidget(gear);

	list = new QListWidget(this);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	list->setContextMenuPolicy(Qt::CustomContextMenu);
	list->setUniformItemSizes(false);
	list->setSpacing(2);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);
	layout->addLayout(top);
	layout->addWidget(list, 1);

	connect(filters, &QButtonGroup::idClicked, this, [this](int id) {
		platformFilter = id < 0 ? std::nullopt : std::optional<ChatPlatform>(ChatPlatform(id));
		Rebuild();
	});
	connect(silence, &QToolButton::toggled, hub, &ChatHub::SetAlertsPaused);
	connect(hub, &ChatHub::AlertsPausedChanged, this, &ActivityDock::UpdateSilence);
	connect(gear, &QToolButton::clicked, this, &ActivityDock::ShowMenu);
	connect(list, &QListWidget::customContextMenuRequested, this, &ActivityDock::ShowRowMenu);
	connect(hub, &ChatHub::EventReceived, this, [this](const StreamEvent &e) {
		if (Visible(e)) {
			AddRow(e, true);
		}
	});

	timesTimer.setInterval(30000);
	connect(&timesTimer, &QTimer::timeout, this, &ActivityDock::RefreshTimes);
	timesTimer.start();

	UpdateSilence(hub->AlertsPaused());
	Rebuild();
}

bool ActivityDock::Visible(const StreamEvent &e) const
{
	if (platformFilter && *platformFilter != e.platform) {
		return false;
	}
	return !HiddenTypes().contains(QString::fromLatin1(StreamEventKey(e.type)));
}

void ActivityDock::Rebuild()
{
	list->clear();
	const QList<StreamEvent> &events = hub->Events();
	for (auto it = events.crbegin(); it != events.crend(); ++it) {
		if (Visible(*it)) {
			AddRow(*it, false);
		}
	}
}

void ActivityDock::AddRow(const StreamEvent &e, bool atTop)
{
	QWidget *row = new QWidget();
	row->setObjectName(QStringLiteral("activityRow"));
	QString accent = TypeColor(e.type);
	row->setStyleSheet(QStringLiteral("#activityRow { background: rgba(255,255,255,10); border: 1px solid %1;"
					  " border-radius: 4px; }")
				   .arg(accent));
	QHBoxLayout *h = new QHBoxLayout(row);
	h->setContentsMargins(6, 4, 6, 4);
	h->setSpacing(6);

	QLabel *badge = new QLabel(row);
	badge->setPixmap(ChatBadge(e.platform, 18));
	badge->setToolTip(ChatPlatformName(e.platform));
	h->addWidget(badge);

	QString name = e.user.displayName.isEmpty() ? e.user.login : e.user.displayName;
	QLabel *who = new QLabel(name, row);
	QFont f = who->font();
	f.setBold(true);
	who->setFont(f);
	who->setTextInteractionFlags(Qt::NoTextInteraction);
	who->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	h->addWidget(who, 1);

	QString amount = e.AmountText();
	if (!amount.isEmpty()) {
		QLabel *a = new QLabel(amount, row);
		a->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(accent));
		h->addWidget(a);
	}

	QLabel *type = new QLabel(StreamEventName(e.type).toUpper(), row);
	type->setStyleSheet(QStringLiteral("background: %1; color: white; font-weight: bold; font-size: 11px;"
					   " padding: 1px 6px; border-radius: 3px;")
				    .arg(accent));
	h->addWidget(type);

	QLabel *when = new QLabel(e.test ? QTStr("Activity.Test") : ActivityTimeAgo(e.time), row);
	when->setObjectName(QStringLiteral("when"));
	when->setMinimumWidth(34);
	when->setAlignment(Qt::AlignCenter);
	when->setStyleSheet("color: palette(placeholder-text);");
	when->setProperty("time", e.time);
	when->setProperty("test", e.test);
	h->addWidget(when);

	QStringList tip;
	tip << QStringLiteral("%1 · %2").arg(ChatPlatformName(e.platform), StreamEventName(e.type));
	if (!e.message.isEmpty()) {
		tip << e.message;
	}
	tip << e.time.toString(QStringLiteral("d MMM yyyy, HH:mm"));
	row->setToolTip(tip.join('\n'));

	QListWidgetItem *item = new QListWidgetItem();
	item->setSizeHint(row->sizeHint());
	/* Right-click finds the event again by id to replay it */
	item->setData(Qt::UserRole + 1, e.id);
	if (atTop) {
		list->insertItem(0, item);
	} else {
		list->addItem(item);
	}
	list->setItemWidget(item, row);
	while (list->count() > 500) {
		delete list->takeItem(list->count() - 1);
	}
}

void ActivityDock::RefreshTimes()
{
	for (int i = 0; i < list->count(); i++) {
		QWidget *row = list->itemWidget(list->item(i));
		QLabel *when = row ? row->findChild<QLabel *>(QStringLiteral("when")) : nullptr;
		if (when && !when->property("test").toBool()) {
			when->setText(ActivityTimeAgo(when->property("time").toDateTime()));
		}
	}
}

void ActivityDock::UpdateSilence(bool paused)
{
	QSignalBlocker block(silence);
	silence->setChecked(paused);
	silence->setText(paused ? QTStr("Activity.Silenced") : QTStr("Activity.Silence"));
	silence->setStyleSheet(paused ? QStringLiteral("QToolButton { background: #b3261e; color: white; "
						       "font-weight: bold; border-radius: 4px; padding: 0 8px; }")
				      : QStringLiteral("QToolButton { padding: 0 8px; }"));
}

void ActivityDock::ShowMenu()
{
	QMenu menu(this);
	QMenu *test = menu.addMenu(QTStr("Activity.TestAlert"));
	for (int i = 0; i < kStreamEventTypeCount; i++) {
		StreamEventType t = StreamEventType(i);
		test->addAction(StreamEventName(t), this, [this, t] {
			/* Show it on the platform being filtered, if any */
			hub->Test(t, platformFilter.value_or(ChatPlatform::Twitch));
		});
	}
	menu.addAction(QTStr("Activity.Skip"), hub, &ChatHub::SkipAlert);
	menu.addSeparator();
	QMenu *show = menu.addMenu(QTStr("Activity.ShowTypes"));
	QStringList hidden = HiddenTypes();
	for (int i = 0; i < kStreamEventTypeCount; i++) {
		QString key = QString::fromLatin1(StreamEventKey(StreamEventType(i)));
		QAction *a = show->addAction(StreamEventName(StreamEventType(i)));
		a->setCheckable(true);
		a->setChecked(!hidden.contains(key));
		connect(a, &QAction::toggled, this, [this, key](bool on) {
			QStringList h = HiddenTypes();
			h.removeAll(key);
			if (!on) {
				h.push_back(key);
			}
			SetHiddenTypes(h);
			Rebuild();
		});
	}
	menu.exec(gear->mapToGlobal(QPoint(0, gear->height())));
}

void ActivityDock::ShowRowMenu(const QPoint &pos)
{
	QListWidgetItem *item = list->itemAt(pos);
	if (!item) {
		return;
	}
	QString id = item->data(Qt::UserRole + 1).toString();
	const StreamEvent *found = nullptr;
	for (const StreamEvent &e : hub->Events()) {
		if (e.id == id) {
			found = &e;
		}
	}
	if (!found) {
		return;
	}
	StreamEvent e = *found;
	QMenu menu(this);
	menu.addAction(QTStr("Activity.Replay"), this, [this, e] { hub->Replay(e); });
	menu.addAction(QTStr("Activity.CopyName"), this, [e] {
		QApplication::clipboard()->setText(e.user.displayName.isEmpty() ? e.user.login : e.user.displayName);
	});
	menu.exec(list->viewport()->mapToGlobal(pos));
}
