#ifndef _PURPLE_WEBSOCKET_H_
#define _PURPLE_WEBSOCKET_H_

#include <glib.h>

typedef struct _PurpleWebsocket PurpleWebsocket;

typedef enum _PurpleWebsocketOp {
	PURPLE_WEBSOCKET_ERROR  = 0x00,
	PURPLE_WEBSOCKET_TEXT   = 0x01,
	PURPLE_WEBSOCKET_BINARY = 0x02,
	PURPLE_WEBSOCKET_CLOSE  = 0x08,
	PURPLE_WEBSOCKET_PING   = 0x09,
	PURPLE_WEBSOCKET_PONG   = 0x0A,
	PURPLE_WEBSOCKET_OPEN   = 0x10,
} PurpleWebsocketOp;

typedef void (*PurpleWebsocketCallback)(PurpleWebsocket *ws, gpointer user_data, PurpleWebsocketOp op, const guchar *msg, size_t len);

PurpleWebsocket *purple_websocket_connect(PurpleAccount *account, const char *url, const char *protocol, const char *cookies, PurpleWebsocketCallback callback, void *user_data);
void purple_websocket_send(PurpleWebsocket *ws, PurpleWebsocketOp op, const guchar *msg, size_t len);
void purple_websocket_abort(PurpleWebsocket *ws);

#endif
