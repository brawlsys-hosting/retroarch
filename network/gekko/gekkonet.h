#ifndef RARCH_GEKKONET_H
#define RARCH_GEKKONET_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

struct retro_netpacket_callback;
struct netplay_chat_buffer;
struct netplay_client_info;
struct netplay_host_list;
struct netplay_rooms;

enum gekkonet_mode
{
   GEKKONET_MODE_NONE = 0,
   GEKKONET_MODE_HOST,
   GEKKONET_MODE_CLIENT
};

typedef struct gekkonet_session gekkonet_session_t;

typedef struct gekkonet_connect_request
{
   const char *address;
   unsigned    port;
   const char *session_id;
   const char *nickname;
   enum gekkonet_mode mode;
   bool        deferred;
} gekkonet_connect_request_t;

typedef struct gekkonet_callbacks
{
   void (*on_remote_input)(void *userdata, unsigned frame);
   void (*on_state_request)(void *userdata);
   void (*on_disconnected)(void *userdata);
   void *userdata;
} gekkonet_callbacks_t;

typedef struct gekkonet_status
{
   bool is_initialized;
   bool is_connected;
   bool is_replaying;
   bool allow_pause;
   bool allow_timeskip;
   bool using_core_packet_interface;
   bool is_spectating;
   bool is_playing;
   bool fade_chat;
   enum gekkonet_mode mode;
} gekkonet_status_t;

typedef struct gekkonet_lobby_state
{
   struct netplay_host_list *hosts;
   struct netplay_rooms     *rooms;
} gekkonet_lobby_state_t;

bool gekkonet_sdk_available(void);

gekkonet_session_t *gekkonet_session_create(
      const gekkonet_connect_request_t *request,
      const gekkonet_callbacks_t *callbacks);
void gekkonet_session_destroy(gekkonet_session_t *session);

void gekkonet_session_step(gekkonet_session_t *session);
void gekkonet_session_post_frame(gekkonet_session_t *session);
void gekkonet_session_pre_frame(gekkonet_session_t *session);

bool gekkonet_session_send_chat(gekkonet_session_t *session,
      const char *nick, const char *message,
      struct netplay_chat_buffer *chat_buffer, bool fade_chat);

void gekkonet_session_refresh_clients(gekkonet_session_t *session,
      struct netplay_client_info *clients,
      size_t *client_count, size_t client_capacity);

void gekkonet_session_get_status(const gekkonet_session_t *session,
      gekkonet_status_t *status);

bool gekkonet_session_set_core_packets(gekkonet_session_t *session,
      struct retro_netpacket_callback *iface, bool enable);

void gekkonet_session_disconnect(gekkonet_session_t *session);

bool gekkonet_session_request_state(gekkonet_session_t *session);

void gekkonet_session_tick_chat(struct netplay_chat_buffer *chat_buffer,
      bool fade_chat);

bool gekkonet_lobby_initialize(gekkonet_lobby_state_t *lobby);
void gekkonet_lobby_deinitialize(gekkonet_lobby_state_t *lobby);
bool gekkonet_lobby_command(gekkonet_lobby_state_t *lobby, int command,
      void *data);

#ifdef __cplusplus
}
#endif

#endif /* RARCH_GEKKONET_H */
