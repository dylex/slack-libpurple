#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <cipher.h>
#include <debug.h>
#include <sslconn.h>

#include "purple-websocket.h"

static const char WS_SALT[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
#define WS_FIN  0x80
#define WS_RSV1 0x40
#define WS_RSV2 0x20
#define WS_RSV3 0x10
#define WS_OP_MASK 0x0F
#define WS_OP_CONT 0x00
#define WS_OP_TEXT 0x01
#define WS_OP_BIN  0x02
#define WS_OP_CLOS 0x08
#define WS_OP_PING 0x09
#define WS_OP_PONG 0x0A
#define WS_MASK	0x80
#define MAX_FRAG 64

struct buffer {
	guchar *buf;
	gsize off; /* next byte to read/write to */
	gsize len; /* (expected) size of data in buffer */
	gsize siz; /* allocated size of buffer */
};

struct _PurpleWebsocket {
	PurpleWebsocketCallback callback;
	void *user_data;

	char *key;

	PurpleProxyConnectData *connection;
	PurpleSslConnection *ssl_connection;

	int fd;
	guint inpa;

	struct buffer input, output;

	gboolean connected;
	PurpleInputCondition closed;
};

static void buffer_set_len(struct buffer *b, size_t n) {
	if (n > b->siz) {
		b->buf = g_realloc(b->buf, n);
		b->siz = n;
	}
	b->len = n;
}

static inline guchar *buffer_incr(struct buffer *b, size_t n) {
	gsize l = b->len;
	buffer_set_len(b, l + n);
	return &b->buf[l];
}

void purple_websocket_abort(PurpleWebsocket *ws) {
	if (ws->ssl_connection != NULL)
		purple_ssl_close(ws->ssl_connection);

	if (ws->connection != NULL)
		purple_proxy_connect_cancel(ws->connection);

	purple_debug_misc("websocket", "removing input %d\n", ws->inpa);
	if (ws->inpa > 0)
		purple_input_remove(ws->inpa);

	if (ws->fd >= 0)
		close(ws->fd);

	g_free(ws->key);
	g_free(ws->output.buf);
	g_free(ws->input.buf);

	g_free(ws);
}

static void ws_error(PurpleWebsocket *ws, const char *error) {
	ws->callback(ws, ws->user_data, PURPLE_WEBSOCKET_ERROR, (const guchar *)error, strlen(error));
	purple_websocket_abort(ws);
}

static const char *skip_lws(const char *s) {
	while (s) {
		switch (*s) {
			case ' ':
			case '\t':
				s++;
				break;
			case '\r':
				if (s[1] == '\n' && (s[2] == ' ' || s[2] == '\t')) {
					s += 3;
					break;
				}
			case '\n':
			case '\0':
				return NULL;
			default:
				return s;
		}
	}
	return s;
}

static const char *find_header_content(const char *data, const char *name) {
	int nlen = strlen(name);
	const char *p = data;

	while ((p = strstr(p, "\r\n"))) {
		p += 2;
		if (!g_ascii_strncasecmp(p, name, nlen) && p[nlen] == ':')
			return &p[nlen+1];
	}
	return NULL;
}

static gboolean ws_read_headers(PurpleWebsocket *ws, const char *headers) {
	const char *upgrade = skip_lws(find_header_content(headers, "Upgrade"));
	if (upgrade && (g_ascii_strncasecmp(upgrade, "websocket", 9) || skip_lws(upgrade+9)))
		upgrade = NULL;

	const char *connection = find_header_content(headers, "Connection");
	while ((connection = skip_lws(connection)) && g_ascii_strncasecmp(connection, "Upgrade", 7))
		while (*connection++ != ',' && (connection = skip_lws(connection)));
	if (connection) {
		const char *e = skip_lws(connection+7);
		if (e && *e != ',')
			connection = NULL;
	}

	const char *accept = skip_lws(find_header_content(headers, "Sec-WebSocket-Accept"));
	if (accept) {
		char *k = g_strjoin(NULL, ws->key, WS_SALT, NULL);
		size_t l = 20;
		guchar s[l];
		g_warn_if_fail(purple_cipher_digest_region("sha1", (guchar *)k, strlen(k), l, s, &l));
		g_free(k);
		gchar *b = g_base64_encode(s, l);
		l = strlen(b);
		if (strncmp(accept, b, l) || skip_lws(accept+l))
			accept = NULL;
		g_free(b);
	}

	/* TODO: Sec-WebSocket-Extensions, Sec-WebSocket-Protocol */

	if (strncmp(headers, "HTTP/1.1 101 ", 13) || !upgrade || !connection || !accept) {
		ws_error(ws, headers);
		return FALSE;
	}

	ws->connected = TRUE;
	ws->callback(ws, ws->user_data, PURPLE_WEBSOCKET_OPEN, NULL, 0);
	return TRUE;
}

static size_t ws_read_message(PurpleWebsocket *ws) {
	uint8_t *input = ws->input.buf;
	size_t len = ws->input.off;
	size_t off = 0;
	struct {
		guchar *p;
		size_t l;
	} frag[MAX_FRAG];
	unsigned fi;

	for (fi = 0; fi < MAX_FRAG; fi ++) {
#define GETN(N) ({ \
		if (len-off < (N)) \
			return off+N; \
		uint8_t *_p = &input[off]; \
		off += N; \
		_p; \
	})
#define GETB(T) (*(uint8_t*)GETN(1))
#define GET(V) memcpy(&(V), GETN(sizeof(V)), sizeof(V))

		if (len-off < 2)
			return off+2;
		uint8_t header = GETB(uint8_t);
		if (header & ~(WS_OP_MASK|WS_FIN)) {
			ws_error(ws, "Unsupported RSV flag");
			return 0;
		}
		uint8_t mlen = GETB(uint8_t);
		if (mlen & WS_MASK) {
			ws_error(ws, "Masked frame");
			return 0;
		}
		uint64_t plen = mlen & ~WS_MASK;
		uint16_t tlen;
		switch (plen) {
			case 127:
				GET(plen);
				plen = GUINT64_FROM_BE(plen);
				break;
			case 126:
				GET(tlen);
				plen = GUINT16_FROM_BE(tlen);
				break;
		}
		frag[fi].l = plen;
		frag[fi].p = GETN(plen);
	
#undef GET
#undef GETB
#undef GETN

		if (header & WS_FIN) {
			/* consolidate all the fragments after the first */
			unsigned i;
			for (i = 1; i <= fi; i++) {
				memmove(&frag[0].p[frag[0].l], frag[i].p, frag[i].l);
				frag[0].l += frag[i].l;
			}

			purple_debug_misc("websocket", "message %x len %lu\n", input[0], (unsigned long) frag[0].l);
			uint8_t op = input[0] & WS_OP_MASK;
			switch (op) {
				case WS_OP_TEXT:
				case WS_OP_BIN:
				case WS_OP_PONG:
				case WS_OP_CLOS:
					ws->callback(ws, ws->user_data, (PurpleWebsocketOp)op, frag[0].p, frag[0].l);
					if (op == WS_OP_CLOS) {
						ws->closed |= PURPLE_INPUT_READ;
						if (ws->closed & PURPLE_INPUT_WRITE) {
							purple_websocket_abort(ws);
							return 0;
						} else
							purple_websocket_send(ws, PURPLE_WEBSOCKET_CLOSE, NULL, 0);
					}
					break;
				case WS_OP_PING:
					purple_websocket_send(ws, PURPLE_WEBSOCKET_PONG, frag[0].p, frag[0].l);
					break;
				default:
					ws_error(ws, "Unknown frame op");
			}
			return off;
		}
	}

	ws_error(ws, "Maximum fragment count exceeded");
	return 0;
}

static void ws_input_cb(gpointer data, gint source, PurpleInputCondition cond);

static gboolean ws_input(PurpleWebsocket *ws) {
	if (ws->inpa) {
		purple_input_remove(ws->inpa);
		ws->inpa = 0;
	}

	if (ws->output.off) {
		/* always none left in practice: */
		memmove(ws->output.buf, ws->output.buf + ws->output.off, ws->output.len -= ws->output.off);
		ws->output.off = 0;
	}

	PurpleInputCondition cond = (ws->ssl_connection ? 0 : PURPLE_INPUT_READ); /* permanent purple_ssl_input_add for ssl */

	if (ws->output.len)
		cond |= PURPLE_INPUT_WRITE;
	else if (ws->closed & PURPLE_INPUT_READ) {
		purple_websocket_abort(ws);
		return FALSE;
	}

	if (cond)
		ws->inpa = purple_input_add(ws->fd, cond, ws_input_cb, ws);
	return TRUE;
}

static void ws_input_cb(gpointer data, G_GNUC_UNUSED gint source, PurpleInputCondition cond) {
	PurpleWebsocket *ws = data;

	if (cond & PURPLE_INPUT_WRITE) {
		ssize_t len = ws->output.off >= ws->output.len ? 0 :
			ws->ssl_connection
			? (ssize_t)purple_ssl_write(ws->ssl_connection, ws->output.buf + ws->output.off, ws->output.len - ws->output.off)
			: write(ws->fd, ws->output.buf + ws->output.off, ws->output.len - ws->output.off);

		if (len < 0) {
			if (errno != EAGAIN) {
				ws_error(ws, g_strerror(errno));
				return;
			}
		} else if ((ws->output.off += len) >= ws->output.len) {
			g_assert(ws->output.off == ws->output.len);
			if (!ws_input(ws))
				return;
		}

		/*
		gchar *enc = purple_base16_encode(ws->output.buf + ws->output.off, len);
		purple_debug_misc("websocket", "send: %s\n", enc);
		g_free(enc);
		*/
	}

	while (cond & PURPLE_INPUT_READ) {
		g_return_if_fail(ws->input.off < ws->input.len);
		ssize_t len = ws->ssl_connection
			? (ssize_t)purple_ssl_read(ws->ssl_connection, ws->input.buf + ws->input.off, ws->input.siz - ws->input.off)
			: read(ws->fd, ws->input.buf + ws->input.off, ws->input.siz - ws->input.off);

		if (len < 0) {
			if (errno != EAGAIN) {
				ws_error(ws, g_strerror(errno));
				return;
			}
			cond &= ~PURPLE_INPUT_READ;
		}
		else if (len == 0) {
			ws_error(ws, "Connection closed");
			return;
		} else {
			/*
			gchar *enc = purple_base16_encode(ws->input.buf + ws->input.off, len);
			purple_debug_misc("websocket", "recv %zu/%zu: %s\n", ws->input.off+len, ws->input.len, enc);
			g_free(enc);
			*/

			ws->input.off += len;

			if (!ws->connected) {
				/* search for the end of headers in the new block (backing up 4-1) */
				char *resp = (char *)ws->input.buf;
				int backup = len + 3;
				if (backup > ws->input.off)
					backup = ws->input.off;
				char *eoh = g_strstr_len(resp + ws->input.off - backup, backup, "\r\n\r\n");

				if (eoh) {
					/* got all the headers now */
					*eoh = '\0';
					eoh += 4;
					if (!ws_read_headers(ws, resp))
						return;

					memmove(ws->input.buf, eoh, ws->input.off -= eoh - resp);
					ws->input.len = 2;
				}
				else if (ws->input.off >= ws->input.len) {
					ws_error(ws, "Response headers too long");
					return;
				}
				else
					return;
			}
			
			while (ws->input.off >= ws->input.len) {
				size_t r = ws_read_message(ws);
				if (!r) /* error */
					return;
				else if (r > ws->input.off) {
					/* need more */
					buffer_set_len(&ws->input, r);
				} else {
					/* consumed some */
					memmove(ws->input.buf, ws->input.buf + r, ws->input.off -= r);
					ws->input.len = 2;
				}
			}
		}
	}
}

void purple_websocket_send(PurpleWebsocket *ws, PurpleWebsocketOp op, const guchar *msg, size_t len) {
	g_return_if_fail(ws->connected && !(ws->closed & PURPLE_INPUT_WRITE));
	g_return_if_fail(!(op & ~WS_OP_MASK));
	gboolean buf = ws->output.len;

#define ADDB(V) (*(uint8_t*)buffer_incr(&ws->output, 1) = (V))
#define ADD(T, V) ({ \
		T _v = (V); \
		memcpy(buffer_incr(&ws->output, sizeof(T)), &_v, sizeof(T)); \
	})

	ADDB(WS_FIN | op);
	if (len > UINT16_MAX) {
		ADDB(WS_MASK | 127);
		ADD(uint64_t, GUINT64_TO_BE(len));
	} else if (len >= 126) {
		ADDB(WS_MASK | 126);
		ADD(uint16_t, GUINT16_TO_BE(len));
	} else {
		ADDB(WS_MASK | len);
	}

	uint32_t mask = g_random_int();
	ADD(uint32_t, mask);

#undef ADD
#undef ADDB

	guchar *p = buffer_incr(&ws->output, len);
	size_t i;
	for (i = 0; i+3 < len; i+=4) {
		uint32_t m = *(uint32_t*)&msg[i] ^ mask;
		memcpy(&p[i], &m, 4);
	}
	for (; i < len; i++)
		p[i] = msg[i] ^ ((uint8_t*)&mask)[i&3];

	if (op == PURPLE_WEBSOCKET_CLOSE)
		ws->closed |= PURPLE_INPUT_WRITE;

	if (!buf)
		ws_input(ws);
}

static void wss_input_cb(gpointer data, G_GNUC_UNUSED PurpleSslConnection *ssl_connection, PurpleInputCondition cond)
{
	PurpleWebsocket *ws = data;
	ws_input_cb(data, ws->fd, cond);
}

static void wss_connect_cb(gpointer data, PurpleSslConnection *ssl_connection, G_GNUC_UNUSED PurpleInputCondition cond) {
	PurpleWebsocket *ws = data;

	ws->fd = ssl_connection->fd;
	purple_ssl_input_add(ws->ssl_connection, wss_input_cb, ws);

	if (ws_input(ws))
		ws_input_cb(ws, ws->fd, PURPLE_INPUT_WRITE);
}

static void wss_error_cb(G_GNUC_UNUSED PurpleSslConnection *ssl_connection, PurpleSslErrorType error, gpointer data) {
	PurpleWebsocket *ws = data;
	ws->ssl_connection = NULL;
	ws_error(ws, purple_ssl_strerror(error));
}

static void ws_connect_cb(gpointer data, gint source, const gchar *error_message) {
	PurpleWebsocket *ws = data;
	ws->connection = NULL;

	if (source == -1) {
		ws_error(ws, error_message ?: "Unable to connect to websocket");
		return;
	}

	ws->fd = source;

	if (ws_input(ws))
		ws_input_cb(ws, ws->fd, PURPLE_INPUT_WRITE);
}

PurpleWebsocket *purple_websocket_connect(PurpleAccount *account,
		const char *url, const char *protocol, const char *cookies,
		PurpleWebsocketCallback callback, void *user_data) {
	gboolean ssl = FALSE;

	if (!g_ascii_strncasecmp(url, "ws://", 5)) {
		ssl = FALSE;
		url += 5;
	}
	else if (!g_ascii_strncasecmp(url, "wss://", 6)) {
		ssl = TRUE;
		url += 6;
	}
	else if (!g_ascii_strncasecmp(url, "http://", 7)) {
		ssl = FALSE;
		url += 7;
	}
	else if (!g_ascii_strncasecmp(url, "https://", 8)) {
		ssl = TRUE;
		url += 8;
	}

	PurpleWebsocket *ws = g_new0(PurpleWebsocket, 1);
	ws->callback = callback;
	ws->user_data = user_data;
	ws->fd = -1;

	char *host, *path;
	int port;
	if (!purple_url_parse(url, &host, &port, &path, NULL, NULL));
	else {
		/* hack to fix default port */
		if (ssl && port == 80)
			port = 443;

		guint32 key[4] = {
			g_random_int(),
			g_random_int(),
			g_random_int(),
			g_random_int()
		};
		ws->key = g_base64_encode((guchar*)key, 16);

		GString *request = g_string_new(NULL);
		g_string_printf(request, "\
GET /%s HTTP/1.1\r\n\
Host: %s\r\n\
Connection: Upgrade\r\n\
Upgrade: websocket\r\n\
Sec-WebSocket-Key: %s\r\n\
Sec-WebSocket-Version: 13\r\n", path, host, ws->key);
		if (protocol)
			g_string_append_printf(request, "Sec-WebSocket-Protocol: %s\r\n", protocol);
		if (cookies)
			g_string_append_printf(request, "Cookie: %s\r\n", cookies);
		g_string_append(request, "\r\n");

		ws->output.len = request->len;
		ws->output.siz = request->allocated_len;
		ws->output.buf = (guchar *)g_string_free(request, FALSE);

		/* allocate space for responses (headers) */
		buffer_set_len(&ws->input, 4096);

		if (ssl)
			ws->ssl_connection = purple_ssl_connect(account, host, port,
					wss_connect_cb, wss_error_cb, ws);
		else
			ws->connection = purple_proxy_connect(NULL, account, host, port,
					ws_connect_cb, ws);

		g_free(host);
		g_free(path);
	}

	if (!(ws->ssl_connection || ws->connection)) {
		ws_error(ws, "Unable to connect to websocket");
		return NULL;
	}

	return ws;
}
