/******************************************************************************
    Siegfried: "Alerts" source.

    One source shows every alert (follow, sub, raid, ...). Add it to a
    scene and place it where alerts should appear. Alerts play one after
    another, never on top of each other, and can be silenced from the
    Activity panel.

    Each alert is: an animation (the bundle's webm, a gif or an image),
    an optional sound, and the viewer's name drawn on top with a template
    like "{name} gifted {count} subs!", shown between two times so it
    lands after the animation's intro.

    All alert sources share one queue, so the same alert plays on the
    main and vertical canvases at the same time, with sound mixed once.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <multichat/ChatConnector.hpp>

#include <obs.hpp>

#include <QRectF>
#include <QSize>
#include <QString>

class ChatHub;

/* How one event type looks and sounds */
struct AlertStyle {
	bool enabled = true;
	QString media;      /* webm/mp4/gif/png... */
	QString sound;      /* optional mp3/ogg/wav... */
	int volume = 80;    /* % */
	int durationMs = 0; /* 0 = length of the animation (images: 5 s) */
	QString textTemplate;
	bool showMessage = true; /* second line: the viewer's message */
	QString fontFace;
	int fontSize = 44;
	bool bold = true;
	uint32_t color = 0xFFFFFFFF;   /* OBS ABGR */
	uint32_t outline = 0xFF000000; /* OBS ABGR, alpha 0 = none */
	QRectF textBox{0.08, 0.62, 0.84, 0.2}; /* fraction of the alert area */
	int showAtMs = 500;
	int hideAtMs = 0; /* 0 = until 300 ms before the end */
	double minAmount = 0; /* bits/donations below this: no alert */

	static AlertStyle Default(StreamEventType type);
	static AlertStyle FromData(obs_data_t *d, StreamEventType type);
	void ToData(obs_data_t *d) const;
};

namespace Alerts {

void Register();
void AttachHub(ChatHub *hub);

/* Text for an event: "{name} gifted {count} subs!" -> "Ana gifted 5 subs!" */
QString Expand(const QString &tmpl, const StreamEvent &e);

/* Editor preview: show this type frozen at timeMs (on every Alerts
 * source), or stop previewing with type = nullopt */
void Preview(obs_source_t *alertsSource, const StreamEvent *event, int timeMs);
int MediaDurationMs(const QString &path); /* -1 if not known yet */
QSize MediaSize(const QString &path);     /* empty if not known yet */

/* The editor's preview draws only inside the editor, never on stream:
 * set while the editor's display renders (graphics thread) */
void SetEditorRendering(bool on);

/* Blocked words (names/messages containing them never show) */
QStringList BlockedWords();
void SetBlockedWords(const QStringList &words);

} // namespace Alerts
