#include <debug.h>
#include <zlib.h>

#include "slack-api.h"
#include "slack-json.h"
#include "slack-channel.h"
#include "slack-user.h"

PurpleConnectionError slack_api_connection_error(const gchar *error) {
	if (!g_strcmp0(error, "not_authed"))
		return PURPLE_CONNECTION_ERROR_INVALID_USERNAME;
	if (!g_strcmp0(error, "invalid_auth") ||
			!g_strcmp0(error, "account_inactive"))
		return PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED;
	return PURPLE_CONNECTION_ERROR_NETWORK_ERROR;
}

struct _SlackAPICall {
	SlackAccount *sa;
	char *url;
	char *request;
	PurpleUtilFetchUrlData *fetch;
	guint timeout;
	SlackAPICallback *callback;
	gpointer data;
};

static void api_free(SlackAPICall *call) {
	g_free(call->request);
	g_free(call->url);
	g_free(call);
}

static void api_error(SlackAPICall *call, const char *error) {
	if (call->fetch)
		purple_util_fetch_url_cancel(call->fetch);
	if (call->timeout)
		purple_timeout_remove(call->timeout);
	if (call->callback)
		call->callback(call->sa, call->data, NULL, error);
	api_free(call);
};

static gboolean api_retry(SlackAPICall *call);
static void api_run(SlackAccount *sa);
gchar *api_gunzip(const guchar *gzip_data, gsize *len_ptr);

static void api_cb(PurpleUtilFetchUrlData *fetch, gpointer data, const gchar *buf_h, gsize len_h, const gchar *error) {
	gboolean free_buf = FALSE;
	SlackAccount *sa = data;
	SlackAPICall *call = g_queue_pop_head(&sa->api_calls);
	g_return_if_fail(call && (call->fetch == fetch || (call->fetch == NULL && error)));
	call->fetch = NULL;

	gsize len = len_h;
	const gchar *buf = g_strstr_len(buf_h, len_h, "\r\n\r\n");
	if (buf) {
		buf += 4; // skip the headers
		len = len_h - (buf - buf_h);
	} else {
		buf = buf_h;
		len = len_h;
	}

	if (g_strstr_len(buf_h, len_h - len, "Content-Encoding: gzip") != NULL ||
			g_strstr_len(buf_h, len_h - len, "content-encoding: gzip") != NULL) {
		gchar *gunzip = api_gunzip((const guchar *)buf, &len);
		if (!gunzip) {
			api_error(call, "Failed to gunzip response");
			api_run(sa);
			return;
		}
		free_buf = TRUE;
		buf = gunzip;
	}

	purple_debug_misc("slack", "api response: %s\n", error ?: buf);
	if (error) {
		if (free_buf) g_free((gchar *)buf);
		api_error(call, error);
		api_run(sa);
		return;
	}

	json_value *json = json_parse(buf, len);
	if (free_buf) g_free((gchar *)buf);
	if (!json) {
		api_error(call, "Invalid JSON response");
		api_run(sa);
		return;
	}

	if (!json_get_prop_boolean(json, "ok", FALSE)) {
		const char *err = json_get_prop_strptr(json, "error");
		if (!g_strcmp0(err, "ratelimited")) {
			/* #27: correct thing to do on 429 status is parse the "Retry-After" header and wait that many seconds,
			 * but getting access to the headers here requires more work, so we just heuristically make up a number... */
			g_queue_push_head(&sa->api_calls, call);
			call->timeout = purple_timeout_add_seconds(purple_account_get_int(sa->account, "ratelimit_delay", 15), (GSourceFunc)api_retry, call);
			json_value_free(json);
			return;
		}
		api_error(call, err ?: "Unknown error");
		json_value_free(json);
		api_run(sa);
		return;
	}

	if (call->callback)
		if (call->callback(call->sa, call->data, json, NULL))
			json = NULL;
	if (json)
		json_value_free(json);
	api_free(call);
	api_run(sa);
}

static gboolean api_retry(SlackAPICall *call) {
	g_return_val_if_fail(call == g_queue_peek_head(&call->sa->api_calls), FALSE);
	call->timeout = 0;
	purple_debug_misc("slack", "api call: %s\n%s\n", call->url, call->request ?: "");
	PurpleUtilFetchUrlData *fetch =
		purple_util_fetch_url_request_len_with_account(call->sa->account,
			call->url, TRUE, NULL, TRUE, call->request, TRUE, 4096*1024,
			api_cb, call->sa);
	if (fetch)
		call->fetch = fetch;
	return FALSE;
}

static void api_run(SlackAccount *sa) {
	SlackAPICall *call = g_queue_peek_head(&sa->api_calls);
	if (!call || call->fetch || call->timeout)
		return;
	api_retry(call);
}

static char *slack_api_encode_post_request(SlackAccount *sa, const char *url, va_list qargs) {
	GString *request;
	gchar *host = NULL, *path = NULL;
	purple_url_parse(url, &host, NULL, &path, NULL, NULL);

	GString *postdata;
	const char *param;

	// Just a long random number.
	guint64 delim = ((guint64)g_random_int() << 32) | (guint64)g_random_int();

	postdata = g_string_new("");
	g_string_printf(postdata, "-----------------------------%" G_GUINT64_FORMAT "\r\n", delim);
	while ((param = va_arg(qargs, const char*))) {
		const char *val = va_arg(qargs, const char*);
		g_string_append_printf(postdata, "\
Content-Disposition: form-data; name=\"%s\"\r\n\
\r\n\
%s\r\n\
-----------------------------%" G_GUINT64_FORMAT "\r\n",
			param, val, delim);
	}

	g_string_append_printf(postdata,
"Content-Disposition: form-data; name=\"token\"\r\n\
\r\n\
%s\r\n\
-----------------------------%" G_GUINT64_FORMAT "\r\n",
		sa->token, delim);

	request = g_string_new(NULL);
	g_string_append_printf(request, "\
POST /%s HTTP/1.0\r\n\
Host: %s\r\n\
Content-Type: multipart/form-data; boundary=---------------------------%" G_GUINT64_FORMAT "\r\n\
Content-Length: %" G_GSIZE_FORMAT "\r\n",
		path, host, delim, postdata->len);

	if (sa->d_cookie) {
		g_string_append_printf(request, "Cookie: d=%s\r\n", sa->d_cookie);
	}
	g_string_append(request, "Accept-Encoding: gzip\r\n");
	g_string_append(request, "\r\n");
	g_string_append(request, postdata->str);

	g_free(host);
	g_free(path);
	g_string_free(postdata, TRUE);

	return g_string_free(request, FALSE);
}

static void slack_api_call_url(SlackAccount *sa, SlackAPICallback callback, gpointer user_data, const char *url, const char *request) {
	SlackAPICall *call = g_new0(SlackAPICall, 1);
	call->sa = sa;
	call->callback = callback;
	call->url = g_strdup(url);
	call->request = g_strdup(request);
	call->data = user_data;

	gboolean empty = g_queue_is_empty(&sa->api_calls);
	g_queue_push_tail(&sa->api_calls, call);
	if (empty)
		api_retry(call);
}

void slack_api_post(SlackAccount *sa, SlackAPICallback callback, gpointer user_data, const gchar *endpoint, ...)
{
	GString *url = g_string_new(NULL);
	g_string_printf(url, "%s/%s", sa->api_url, endpoint);

	va_list qargs;
	va_start(qargs, endpoint);
	char *request = slack_api_encode_post_request(sa, url->str, qargs);
	va_end(qargs);

	slack_api_call_url(sa, callback, user_data, url->str, request);

	g_string_free(url, TRUE);
  	g_free(request);
}

void slack_api_disconnect(SlackAccount *sa) {
	SlackAPICall *call;
	while ((call = g_queue_pop_head(&sa->api_calls)))
		api_error(call, "disconnected");
}


#include <zlib.h>

gchar *
api_gunzip(const guchar *gzip_data, gsize *len_ptr)
{
	gsize gzip_data_len	= *len_ptr;
	z_stream zstr;
	int gzip_err = 0;
	gchar *data_buffer;
	gulong gzip_len = G_MAXUINT16;
	GString *output_string = NULL;

	data_buffer = g_new0(gchar, gzip_len);

	zstr.next_in = NULL;
	zstr.avail_in = 0;
	zstr.zalloc = Z_NULL;
	zstr.zfree = Z_NULL;
	zstr.opaque = 0;
	gzip_err = inflateInit2(&zstr, MAX_WBITS+32);
	if (gzip_err != Z_OK)
	{
		g_free(data_buffer);
		purple_debug_error("slack", "no built-in gzip support in zlib\n");
		return NULL;
	}

	zstr.next_in = (Bytef *)gzip_data;
	zstr.avail_in = gzip_data_len;

	zstr.next_out = (Bytef *)data_buffer;
	zstr.avail_out = gzip_len;

	gzip_err = inflate(&zstr, Z_SYNC_FLUSH);

	if (gzip_err == Z_DATA_ERROR)
	{
		inflateEnd(&zstr);
		gzip_err = inflateInit2(&zstr, -MAX_WBITS);
		if (gzip_err != Z_OK)
		{
			g_free(data_buffer);
			purple_debug_error("slack", "Cannot decode gzip header\n");
			return NULL;
		}
		zstr.next_in = (Bytef *)gzip_data;
		zstr.avail_in = gzip_data_len;
		zstr.next_out = (Bytef *)data_buffer;
		zstr.avail_out = gzip_len;
		gzip_err = inflate(&zstr, Z_SYNC_FLUSH);
	}
	output_string = g_string_new("");
	while (gzip_err == Z_OK)
	{
		//append data to buffer
		output_string = g_string_append_len(output_string, data_buffer, gzip_len - zstr.avail_out);
		//reset buffer pointer
		zstr.next_out = (Bytef *)data_buffer;
		zstr.avail_out = gzip_len;
		gzip_err = inflate(&zstr, Z_SYNC_FLUSH);
	}
	if (gzip_err == Z_STREAM_END)
	{
		output_string = g_string_append_len(output_string, data_buffer, gzip_len - zstr.avail_out);
	} else {
		purple_debug_error("slack", "gzip inflate error\n");
	}
	inflateEnd(&zstr);

	g_free(data_buffer);

	if (len_ptr)
		*len_ptr = output_string->len;

	return g_string_free(output_string, FALSE);
}
