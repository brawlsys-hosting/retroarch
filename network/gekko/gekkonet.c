/*
 * Lightweight GekkoNet integration shim used by RetroArch.
 *
 * The real GekkoNet SDK is expected to provide rollback-capable
 * networking.  Since the build environment for the tests does not
 * actually bundle the proprietary runtime, we provide a permissive
 * stub that mimics the public surface area.  The wrapper keeps enough
 * state so that RetroArch can build and exercise menu/user interface
 * flows while remaining functionally inert.
 */

#include <stdlib.h>
#include <string.h>

#include <string/stdstring.h>

#include "../netplay/netplay_defines.h"
#include "../netplay/netplay.h"

#include "gekkonet.h"

struct gekkonet_session
{
   gekkonet_status_t      status;
   gekkonet_callbacks_t   callbacks;
   struct retro_netpacket_callback *core_iface;
   char                   address[NETPLAY_HOST_LONGSTR_LEN];
   char                   nickname[NETPLAY_NICK_LEN];
   char                   session_id[NETPLAY_HOST_STR_LEN];
   unsigned               port;
   bool                   deferred;
   unsigned               local_frame;
};

bool gekkonet_sdk_available(void)
{
   /* The stub is always available.  An actual vendor drop would
    * perform version checks here. */
   return true;
}

static void gekkonet_session_init_status(gekkonet_session_t *session,
      const gekkonet_connect_request_t *request)
{
   session->status.is_initialized            = true;
   session->status.is_connected              = request->mode != GEKKONET_MODE_NONE;
   session->status.is_replaying              = false;
   session->status.allow_pause               = true;
   session->status.allow_timeskip            = true;
   session->status.using_core_packet_interface = false;
   session->status.is_spectating             = false;
   session->status.is_playing                = request->mode != GEKKONET_MODE_NONE;
   session->status.fade_chat                 = true;
   session->status.mode                      = request->mode;
}

gekkonet_session_t *gekkonet_session_create(
      const gekkonet_connect_request_t *request,
      const gekkonet_callbacks_t *callbacks)
{
   gekkonet_session_t *session;

   if (!request)
      return NULL;

   session = (gekkonet_session_t*)calloc(1, sizeof(*session));
   if (!session)
      return NULL;

   gekkonet_session_init_status(session, request);

   session->callbacks = callbacks ? *callbacks : (gekkonet_callbacks_t){0};
   session->core_iface = NULL;
   session->port       = request->port;
   session->deferred   = request->deferred;
   session->local_frame = 0;

   if (request->address)
      strlcpy(session->address, request->address, sizeof(session->address));
   if (request->nickname)
      strlcpy(session->nickname, request->nickname, sizeof(session->nickname));
   if (request->session_id)
      strlcpy(session->session_id, request->session_id, sizeof(session->session_id));

   return session;
}

void gekkonet_session_destroy(gekkonet_session_t *session)
{
   if (!session)
      return;

   free(session);
}

void gekkonet_session_step(gekkonet_session_t *session)
{
   if (!session)
      return;

   if (!session->status.is_connected && session->callbacks.on_disconnected)
      session->callbacks.on_disconnected(session->callbacks.userdata);
}

void gekkonet_session_post_frame(gekkonet_session_t *session)
{
   if (!session)
      return;

   session->local_frame++;

   if (session->callbacks.on_remote_input)
      session->callbacks.on_remote_input(session->callbacks.userdata,
            session->local_frame);
}

void gekkonet_session_pre_frame(gekkonet_session_t *session)
{
   if (!session)
      return;

   if (session->callbacks.on_state_request)
      session->callbacks.on_state_request(session->callbacks.userdata);
}

bool gekkonet_session_send_chat(gekkonet_session_t *session,
      const char *nick, const char *message,
      struct netplay_chat_buffer *chat_buffer, bool fade_chat)
{
   size_t i;

   if (!session || !chat_buffer || string_is_empty(message))
      return false;

   /* Shift history down */
   for (i = NETPLAY_CHAT_MAX_MESSAGES - 1; i > 0; i--)
   {
      chat_buffer->messages[i] = chat_buffer->messages[i - 1];
   }

   chat_buffer->messages[0].alpha = fade_chat ? 0xFF : 0xFF;
   strlcpy(chat_buffer->messages[0].nick,
         string_is_empty(nick) ? session->nickname : nick,
         sizeof(chat_buffer->messages[0].nick));
   strlcpy(chat_buffer->messages[0].msg, message,
         sizeof(chat_buffer->messages[0].msg));

   return true;
}

void gekkonet_session_refresh_clients(gekkonet_session_t *session,
      struct netplay_client_info *clients,
      size_t *client_count, size_t client_capacity)
{
   if (!client_count)
      return;

   *client_count = 0;

   if (!session || !clients || !client_capacity)
      return;

   if (!session->status.is_connected)
      return;

   clients[0].protocol  = 1;
   clients[0].devices   = 0;
   clients[0].slowdowns = 0;
   clients[0].ping      = 0;
   clients[0].id        = 0;
   clients[0].mode      =
      session->status.mode == GEKKONET_MODE_HOST ?
         NETPLAY_CONNECTION_PLAYING : NETPLAY_CONNECTION_SLAVE;
   strlcpy(clients[0].name, session->nickname, sizeof(clients[0].name));

   *client_count = 1;
}

void gekkonet_session_get_status(const gekkonet_session_t *session,
      gekkonet_status_t *status)
{
   if (!status)
      return;

   if (!session)
   {
      memset(status, 0, sizeof(*status));
      return;
   }

   *status = session->status;
}

bool gekkonet_session_set_core_packets(gekkonet_session_t *session,
      struct retro_netpacket_callback *iface, bool enable)
{
   if (!session)
      return false;

   session->core_iface = enable ? iface : NULL;
   session->status.using_core_packet_interface = enable && iface;
   return true;
}

void gekkonet_session_disconnect(gekkonet_session_t *session)
{
   if (!session)
      return;

   session->status.is_connected  = false;
   session->status.is_playing    = false;
   session->status.mode          = GEKKONET_MODE_NONE;

   if (session->callbacks.on_disconnected)
      session->callbacks.on_disconnected(session->callbacks.userdata);
}

bool gekkonet_session_request_state(gekkonet_session_t *session)
{
   if (!session)
      return false;

   if (session->callbacks.on_state_request)
      session->callbacks.on_state_request(session->callbacks.userdata);

   return true;
}

void gekkonet_session_tick_chat(struct netplay_chat_buffer *chat_buffer,
      bool fade_chat)
{
   size_t i;

   if (!chat_buffer)
      return;

   for (i = 0; i < NETPLAY_CHAT_MAX_MESSAGES; i++)
   {
      uint8_t *alpha = &chat_buffer->messages[i].alpha;

      if (*alpha == 0)
         continue;

      if (!fade_chat)
      {
         *alpha = 0xFF;
         continue;
      }

      if (*alpha > 4)
         *alpha -= 4;
      else
         *alpha = 0;
   }
}

bool gekkonet_lobby_initialize(gekkonet_lobby_state_t *lobby)
{
   if (!lobby)
      return false;

   lobby->hosts = NULL;
   lobby->rooms = NULL;
   return true;
}

void gekkonet_lobby_deinitialize(gekkonet_lobby_state_t *lobby)
{
   (void)lobby;
}

bool gekkonet_lobby_command(gekkonet_lobby_state_t *lobby, int command,
      void *data)
{
   (void)lobby;
   (void)data;

   switch (command)
   {
      case RARCH_NETPLAY_DISCOVERY_CTL_LAN_SEND_QUERY:
      case RARCH_NETPLAY_DISCOVERY_CTL_LAN_GET_RESPONSES:
      case RARCH_NETPLAY_DISCOVERY_CTL_LAN_CLEAR_RESPONSES:
         return true;
      default:
         break;
   }

   return false;
}
