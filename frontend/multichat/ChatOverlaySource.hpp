/******************************************************************************
    Siegfried: "Chat (all platforms)" source.

    A normal source the streamer adds to any scene (Sources > +). It shows
    the same merged chat as the Chat dock, with each platform's badge, so
    viewers on Twitch see what people on Kick or YouTube say, and so on.

    Drawn natively with Qt into a texture: no browser source, no web page,
    nothing online.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

class ChatHub;

namespace ChatOverlay {

/* Call once after modules are loaded (before a scene collection loads) */
void Register();

/* Start feeding chat into every chat source */
void AttachHub(ChatHub *hub);

} // namespace ChatOverlay
