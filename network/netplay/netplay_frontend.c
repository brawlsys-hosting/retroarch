/*  RetroArch - GekkoNet frontend wrapper
 *
 *  This module replaces the legacy peer-to-peer netplay stack with a
 *  minimalist bridge that exposes the same control surface to the rest
 *  of RetroArch while delegating runtime semantics to the GekkoNet
 *  rollback SDK.  The build used by these tests bundles a stubbed
 *  implementation of the SDK that keeps enough state for the menu and
 *  command queues to operate.  Integrators can drop the actual vendor
 *  runtime in network/gekko without touching the surrounding code.
 */

#include <stdlib.h>
#include <string.h>

#include <retro_miscellaneous.h>
#include <string/stdstring.h>
#include <lists/string_list.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../configuration.h"
#include "../../verbosity.h"

#include "../../audio/audio_driver.h"
#include "../../gfx/gfx_widgets.h"
#include "../../gfx/video_driver.h"
#include "../../input/input_driver.h"

#include "netplay.h"
#include "netplay_defines.h"

#include "../gekko/gekkonet.h"

#define GEKKONET_MAX_CLIENTS 8

#include "netplay.h"
#include "netplay_defines.h"

#include "../gekko/gekkonet.h"

#define GEKKONET_MAX_CLIENTS 8

static net_driver_state_t networking_driver_st = {0};

typedef struct netplay_gekkonet_frontend
{
   gekkonet_session_t   *session;
   gekkonet_status_t     status;
   struct netplay_client_info clients[GEKKONET_MAX_CLIENTS];
   bool                  chat_visible;
   bool                  playing;
   bool                  spectating;
   bool                  allow_pause;
   bool                  allow_timeskip;
   bool                  fade_chat;
   bool                  using_core_packets;
   enum gekkonet_mode    configured_mode;
   gekkonet_lobby_state_t lobby;
} netplay_gekkonet_frontend_t;

static netplay_gekkonet_frontend_t g_gekkonet = {0};

static void netplay_gekkonet_reset_chat_buffer(struct netplay_chat_buffer *buffer)
{
   if (!buffer)
      return;

   memset(buffer->messages, 0, sizeof(buffer->messages));
}

static settings_t *netplay_get_settings(void)
{
   return config_get_ptr();
}

static void netplay_gekkonet_refresh_status(void)
{
   if (g_gekkonet.session)
      gekkonet_session_get_status(g_gekkonet.session, &g_gekkonet.status);
   else
      memset(&g_gekkonet.status, 0, sizeof(g_gekkonet.status));

   g_gekkonet.allow_pause    = g_gekkonet.status.allow_pause;
   g_gekkonet.allow_timeskip = g_gekkonet.status.allow_timeskip;
   g_gekkonet.fade_chat      = g_gekkonet.status.fade_chat;

   if (!g_gekkonet.session)
   {
      g_gekkonet.playing    = false;
      g_gekkonet.spectating = false;
      return;
   }

   if (!g_gekkonet.spectating)
      g_gekkonet.playing = g_gekkonet.status.mode != GEKKONET_MODE_NONE;
}

static void netplay_gekkonet_apply_settings(net_driver_state_t *net_st)
{
   settings_t *settings = netplay_get_settings();

   if (!settings)
      return;

   net_st->chat_buffer.color_name =
      (uint32_t)settings->uints.netplay_chat_color_name << 8;
   net_st->chat_buffer.color_msg  =
      (uint32_t)settings->uints.netplay_chat_color_msg << 8;
   g_gekkonet.fade_chat           = settings->bools.netplay_fade_chat;
}

static bool netplay_gekkonet_start_session(
      enum gekkonet_mode mode,
      const char *address,
      unsigned port,
      const char *session,
      bool deferred)
{
   gekkonet_connect_request_t request = {0};
   net_driver_state_t *net_st        = &networking_driver_st;
   settings_t *settings              = netplay_get_settings();

   if (!gekkonet_sdk_available())
   {
      RARCH_ERR("[GekkoNet] SDK unavailable.\n");
      return false;
   }

   if (g_gekkonet.session)
      deinit_netplay();

   request.address    = address;
   request.port       = port;
   request.session_id = session;
   request.nickname   = settings ? settings->paths.username : NULL;
   request.mode       = mode;
   request.deferred   = deferred;

   g_gekkonet.session = gekkonet_session_create(&request, NULL);
   if (!g_gekkonet.session)
   {
      RARCH_ERR("[GekkoNet] Failed to create session.\n");
      return false;
   }

   g_gekkonet.configured_mode = mode;
   g_gekkonet.chat_visible    = false;
   g_gekkonet.spectating      = false;
   g_gekkonet.playing         = mode != GEKKONET_MODE_NONE;
   g_gekkonet.using_core_packets = false;

   net_st->data               = (netplay_t*)g_gekkonet.session;
   net_st->client_info        = g_gekkonet.clients;
   net_st->client_info_count  = 0;
   net_st->core_netpacket_interface = NULL;
   net_st->latest_ping        = 0;
   net_st->flags             |= NET_DRIVER_ST_FLAG_NETPLAY_ENABLED;

   if (mode == GEKKONET_MODE_CLIENT)
      net_st->flags |= NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT;
   else
      net_st->flags &= ~NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT;

   if (deferred)
      net_st->flags |= NET_DRIVER_ST_FLAG_NETPLAY_CLIENT_DEFERRED;
   else
      net_st->flags &= ~NET_DRIVER_ST_FLAG_NETPLAY_CLIENT_DEFERRED;

   netplay_gekkonet_apply_settings(net_st);
   netplay_gekkonet_reset_chat_buffer(&net_st->chat_buffer);
   netplay_gekkonet_refresh_status();

   return true;
}

static void netplay_gekkonet_finalize_stop(void)
{
   net_driver_state_t *net_st = &networking_driver_st;

   net_st->data                = NULL;
   net_st->client_info_count   = 0;
   net_st->core_netpacket_interface = NULL;
   net_st->flags              &= ~(NET_DRIVER_ST_FLAG_NETPLAY_ENABLED |
         NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT |
         NET_DRIVER_ST_FLAG_NETPLAY_CLIENT_DEFERRED);

   memset(&g_gekkonet, 0, sizeof(g_gekkonet));
}

static void netplay_gekkonet_disconnect_internal(bool full_shutdown)
{
   if (g_gekkonet.session)
   {
      if (full_shutdown)
         gekkonet_session_destroy(g_gekkonet.session);
      else
         gekkonet_session_disconnect(g_gekkonet.session);
   }

   if (full_shutdown)
      netplay_gekkonet_finalize_stop();
   else
   {
      g_gekkonet.status.is_connected = false;
      g_gekkonet.playing             = false;
   }
}

net_driver_state_t *networking_state_get_ptr(void)
{
   return &networking_driver_st;
}

bool netplay_compatible_version(const char *version)
{
   (void)version;
   /* The GekkoNet bridge handles compatibility internally. */
   return true;
}

bool netplay_decode_hostname(const char *hostname,
      char *address, unsigned *port, char *session, size_t len)
{
   struct string_list hostname_data;

   if (string_is_empty(hostname))
      return false;

   if (!string_list_initialize(&hostname_data))
      return false;

   if (!string_split_noalloc(&hostname_data, hostname, "|"))
   {
      string_list_deinitialize(&hostname_data);
      return false;
   }

   if (hostname_data.size >= 1 &&
       !string_is_empty(hostname_data.elems[0].data))
   {
      if (address)
         strlcpy(address, hostname_data.elems[0].data, len);
   }

   if (hostname_data.size >= 2 &&
       !string_is_empty(hostname_data.elems[1].data))
   {
      if (port)
      {
         unsigned long tmp_port = strtoul(hostname_data.elems[1].data,
               NULL, 10);
         if (tmp_port && tmp_port <= 65535)
            *port = (unsigned)tmp_port;
      }
   }

   if (hostname_data.size >= 3 &&
       !string_is_empty(hostname_data.elems[2].data))
   {
      if (session)
         strlcpy(session, hostname_data.elems[2].data, len);
   }

   string_list_deinitialize(&hostname_data);

   return true;
}

bool netplay_gekkonet_start_client(const char *server,
      unsigned port, const char *session, bool deferred)
{
   if (string_is_empty(server))
      server = "";

   RARCH_LOG("[GekkoNet] Starting client session (%s:%u).\n",
         string_is_empty(server) ? "local" : server, port);


   RARCH_LOG("[GekkoNet] Starting client session (%s:%u).\n",
         string_is_empty(server) ? "local" : server, port);

   return netplay_gekkonet_start_session(GEKKONET_MODE_CLIENT,
         server, port, session, deferred);
}

bool netplay_gekkonet_start_host(unsigned port)
{
   RARCH_LOG("[GekkoNet] Hosting on port %u.\n", port);
   return netplay_gekkonet_start_session(GEKKONET_MODE_HOST,
         NULL, port, NULL, false);
}

void netplay_gekkonet_disconnect(void)
{
   RARCH_LOG("[GekkoNet] Disconnect requested.\n");
   netplay_gekkonet_disconnect_internal(false);
   netplay_gekkonet_refresh_status();
}

void netplay_gekkonet_shutdown(void)
{
   RARCH_LOG("[GekkoNet] Shutting down session.\n");
   netplay_gekkonet_disconnect_internal(true);
}

void netplay_gekkonet_toggle_game_watch(void)
{
   g_gekkonet.spectating = !g_gekkonet.spectating;
   g_gekkonet.playing    = !g_gekkonet.spectating;
}

void netplay_gekkonet_toggle_chat_overlay(void)
{
   g_gekkonet.chat_visible = !g_gekkonet.chat_visible;
}

bool init_netplay(const char *server,
      unsigned port, const char *session)
{
   return netplay_gekkonet_start_client(server, port, session, false);
}

bool init_netplay_deferred(const char *server,
      unsigned port, const char *session)
{
   return netplay_gekkonet_start_client(server, port, session, true);
}

void deinit_netplay(void)
{
   netplay_gekkonet_shutdown();
}

bool netplay_driver_ctl(enum rarch_netplay_ctl_state state, void *data)
{
   net_driver_state_t *net_st = &networking_driver_st;

   switch (state)
   {
      case RARCH_NETPLAY_CTL_GAME_WATCH:
         netplay_gekkonet_toggle_game_watch();
         return true;
      case RARCH_NETPLAY_CTL_PLAYER_CHAT:
         netplay_gekkonet_toggle_chat_overlay();
         return true;
         return true;
      case RARCH_NETPLAY_CTL_PLAYER_CHAT:
         netplay_gekkonet_toggle_chat_overlay();
         return true;
      case RARCH_NETPLAY_CTL_POST_FRAME:
         if (g_gekkonet.session)
            gekkonet_session_post_frame(g_gekkonet.session);
         gekkonet_session_tick_chat(&net_st->chat_buffer,
               g_gekkonet.fade_chat);
         return true;
      case RARCH_NETPLAY_CTL_PRE_FRAME:
         if (g_gekkonet.session)
            gekkonet_session_pre_frame(g_gekkonet.session);
         return true;
      case RARCH_NETPLAY_CTL_ENABLE_SERVER:
         g_gekkonet.configured_mode = GEKKONET_MODE_HOST;
         net_st->flags &= ~NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT;
         return true;
      case RARCH_NETPLAY_CTL_ENABLE_CLIENT:
         g_gekkonet.configured_mode = GEKKONET_MODE_CLIENT;
         net_st->flags |= NET_DRIVER_ST_FLAG_NETPLAY_IS_CLIENT;
         return true;
      case RARCH_NETPLAY_CTL_DISABLE:
         netplay_gekkonet_shutdown();
         return true;
#ifndef HAVE_DYNAMIC
      case RARCH_NETPLAY_CTL_ADD_FORK_ARG:
      case RARCH_NETPLAY_CTL_GET_FORK_ARGS:
      case RARCH_NETPLAY_CTL_CLEAR_FORK_ARGS:
         return true;
#endif
      case RARCH_NETPLAY_CTL_REFRESH_CLIENT_INFO:
         if (g_gekkonet.session)
         {
            size_t capacity = ARRAY_SIZE(g_gekkonet.clients);
            size_t count    = 0;
            gekkonet_session_refresh_clients(g_gekkonet.session,
                  g_gekkonet.clients, &count, capacity);
            net_st->client_info_count = count;
            return true;
         }
         return false;
      case RARCH_NETPLAY_CTL_IS_ENABLED:
         return (net_st->flags & NET_DRIVER_ST_FLAG_NETPLAY_ENABLED) != 0;
      case RARCH_NETPLAY_CTL_IS_REPLAYING:
         return g_gekkonet.status.is_replaying;
      case RARCH_NETPLAY_CTL_IS_SERVER:
         return g_gekkonet.status.mode == GEKKONET_MODE_HOST;
      case RARCH_NETPLAY_CTL_IS_CONNECTED:
         return g_gekkonet.status.is_connected;
      case RARCH_NETPLAY_CTL_IS_PLAYING:
         return g_gekkonet.playing;
      case RARCH_NETPLAY_CTL_IS_SPECTATING:
         return g_gekkonet.spectating;
      case RARCH_NETPLAY_CTL_IS_DATA_INITED:
         return g_gekkonet.session != NULL;
      case RARCH_NETPLAY_CTL_ALLOW_PAUSE:
         return g_gekkonet.allow_pause;
      case RARCH_NETPLAY_CTL_PAUSE:
      case RARCH_NETPLAY_CTL_UNPAUSE:
         return g_gekkonet.allow_pause;
      case RARCH_NETPLAY_CTL_LOAD_SAVESTATE:
      case RARCH_NETPLAY_CTL_RESET:
         if (g_gekkonet.session)
            return gekkonet_session_request_state(g_gekkonet.session);
         return false;
      case RARCH_NETPLAY_CTL_DISCONNECT:
         netplay_gekkonet_disconnect();
         return true;
      case RARCH_NETPLAY_CTL_FINISHED_NAT_TRAVERSAL:
         return true;
      case RARCH_NETPLAY_CTL_DESYNC_PUSH:
      case RARCH_NETPLAY_CTL_DESYNC_POP:
         return true;
      case RARCH_NETPLAY_CTL_KICK_CLIENT:
      case RARCH_NETPLAY_CTL_BAN_CLIENT:
         return false;
      case RARCH_NETPLAY_CTL_SET_CORE_PACKET_INTERFACE:
         g_gekkonet.using_core_packets = data != NULL;
         if (g_gekkonet.session)
            return gekkonet_session_set_core_packets(
                  g_gekkonet.session,
                  (struct retro_netpacket_callback*)data,
                  data != NULL);
         return false;
      case RARCH_NETPLAY_CTL_USE_CORE_PACKET_INTERFACE:
         return g_gekkonet.using_core_packets;
      case RARCH_NETPLAY_CTL_ALLOW_TIMESKIP:
         return g_gekkonet.allow_timeskip;
      default:
         break;
   }

   return false;
}

bool netplay_reinit_serialization(void)
{
   if (!g_gekkonet.session)
      return false;
   return gekkonet_session_request_state(g_gekkonet.session);
}

bool netplay_is_spectating(void)
{
   return g_gekkonet.spectating;
}

void netplay_force_send_savestate(void)
{
   if (g_gekkonet.session)
      gekkonet_session_request_state(g_gekkonet.session);
}

#ifdef HAVE_NETPLAYDISCOVERY
bool init_netplay_discovery(void)
{
   return gekkonet_lobby_initialize(&g_gekkonet.lobby);
}

void deinit_netplay_discovery(void)
{
   gekkonet_lobby_deinitialize(&g_gekkonet.lobby);
}

bool netplay_discovery_driver_ctl(
      enum rarch_netplay_discovery_ctl_state state, void *data)
{
   return gekkonet_lobby_command(&g_gekkonet.lobby, state, data);
}
#endif

const mitm_server_t netplay_mitm_server_list[NETPLAY_MITM_SERVERS] = {0};

void video_frame_net(const void *data,
      unsigned width, unsigned height, size_t pitch)
{
   video_driver_frame(data, width, height, pitch);
}

void audio_sample_net(int16_t left, int16_t right)
{
   audio_driver_sample(left, right);
}

size_t audio_sample_batch_net(const int16_t *data, size_t frames)
{
   return audio_driver_sample_batch(data, frames);
}

int16_t input_state_net(unsigned port, unsigned device,
      unsigned idx, unsigned id)
{
   return input_driver_state_wrapper(port, device, idx, id);
}

const gfx_widget_t gfx_widget_netplay_chat = {0};
const gfx_widget_t gfx_widget_netplay_ping = {0};
}

bool netplay_reinit_serialization(void)
{
   if (!g_gekkonet.session)
      return false;
   return gekkonet_session_request_state(g_gekkonet.session);
}

bool netplay_is_spectating(void)
{
   return g_gekkonet.spectating;
}

void netplay_force_send_savestate(void)
{
   if (g_gekkonet.session)
      gekkonet_session_request_state(g_gekkonet.session);
}

#ifdef HAVE_NETPLAYDISCOVERY
bool init_netplay_discovery(void)
{
   return gekkonet_lobby_initialize(&g_gekkonet.lobby);
}

void deinit_netplay_discovery(void)
{
   gekkonet_lobby_deinitialize(&g_gekkonet.lobby);
}

bool netplay_discovery_driver_ctl(
      enum rarch_netplay_discovery_ctl_state state, void *data)
{
   return gekkonet_lobby_command(&g_gekkonet.lobby, state, data);
}
#endif

const mitm_server_t netplay_mitm_server_list[NETPLAY_MITM_SERVERS] = {0};
