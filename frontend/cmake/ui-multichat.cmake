# Built-in app registrations, so viewers of this build only click "Connect".
# Client IDs are public. Secrets are read from the environment at configure
# time (GitHub Actions secrets in CI) or passed with -D for local builds.
set(MULTICHAT_TWITCH_CLIENT_ID "3qps5jsj3mds8ebjrqiu4ttz4x1l5m")
set(MULTICHAT_KICK_CLIENT_ID "01M7XMZP0FR2M19QCE93ACSFMX")
foreach(_secret IN ITEMS KICK_CLIENT_SECRET YOUTUBE_CLIENT_ID YOUTUBE_CLIENT_SECRET)
  if(NOT DEFINED MULTICHAT_${_secret})
    set(MULTICHAT_${_secret} "$ENV{${_secret}}")
  endif()
endforeach()
configure_file(multichat/multichat-config.h.in "${CMAKE_CURRENT_BINARY_DIR}/multichat/multichat-config.h" @ONLY)
target_include_directories(obs-studio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/multichat")

target_sources(
  obs-studio
  PRIVATE
    multichat/ChatConnector.cpp
    multichat/ChatConnector.hpp
    multichat/ChatSocket.cpp
    multichat/ChatSocket.hpp
    multichat/ChatHub.cpp
    multichat/ChatHub.hpp
    multichat/ChatOverlaySource.cpp
    multichat/ChatOverlaySource.hpp
    multichat/KickChat.cpp
    multichat/KickChat.hpp
    multichat/MultichatDock.cpp
    multichat/MultichatDock.hpp
    multichat/ToggleSwitch.cpp
    multichat/ToggleSwitch.hpp
    multichat/RumbleChat.cpp
    multichat/RumbleChat.hpp
    multichat/TwitchChat.cpp
    multichat/TwitchChat.hpp
    multichat/YouTubeChat.cpp
    multichat/YouTubeChat.hpp
)
