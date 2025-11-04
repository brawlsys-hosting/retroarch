# GekkoNet Rollback Netplay

RetroArch now integrates the [GekkoNet](https://github.com/Brawlsys-Hosting/GekkoNet) rollback SDK.  The legacy peer-to-peer netplay implementation has been replaced with a wrapper that forwards RetroArch callbacks to the new networking runtime.

## Bootstrapping a session

1. Configure your nickname under **Settings → Network → Netplay**.  The nickname is shared with GekkoNet peers when you host or join a lobby.
2. Load content as normal.  When the core supports netplay, open the **Netplay** menu and select **Start GekkoNet Host** to expose the session.
3. To join a remote lobby, choose **Connect to GekkoNet Host** and provide the address (or discovery code) supplied by the host.  RetroArch will initialise a rollback session using the GekkoNet SDK.

## Lobby discovery

The **Refresh GekkoNet Lobby List** and **Refresh GekkoNet LAN List** actions now query the SDK directly.  LAN discovery remains asynchronous – RetroArch updates the list when the SDK reports new rooms.

## Chat and overlays

In-game chat, spectator toggles and overlay fade behaviour map to GekkoNet state.  The **Netplay** hotkeys now toggle chat visibility (`CMD_EVENT_NETPLAY_PLAYER_CHAT`) and spectator mode (`CMD_EVENT_NETPLAY_GAME_WATCH`) using the new integration points.

## Notes for integrators

* The `network/gekko` directory hosts the vendored SDK entry points.  The shipped code is a permissive stub that keeps the build self-contained; drop-in vendor builds should replace it with the official binaries.
* The public wrapper lives in `network/netplay/netplay_frontend.c`.  It exports the same symbols expected by the rest of RetroArch while driving `gekkonet_session_t` instances.
* Platform build scripts include `network/gekko/gekkonet.c` so custom toolchains do not require additional changes.
