#include <debug.h>

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

static void api_cb(PurpleUtilFetchUrlData *fetch, gpointer data, const gchar *buf, gsize len, const gchar *error) {
	SlackAccount *sa = data;
	SlackAPICall *call = g_queue_pop_head(&sa->api_calls);
	g_return_if_fail(call == NULL || (call->fetch == NULL && error) || call->fetch == fetch);
	call->fetch = NULL;

	purple_debug_misc("slack", "api response: %s\n", error ?: buf);
	if (error) {
		api_error(call, error);
		api_run(sa);
		return;
	}

	json_value *json = json_parse(buf, len);
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
	call->timeout = 0;
	purple_debug_misc("slack", "api call: %s\n%s\n", call->url, call->request ?: "");
	PurpleUtilFetchUrlData *fetch =
		purple_util_fetch_url_request_len_with_account(call->sa->account,
			call->url, FALSE, NULL, TRUE, call->request, FALSE, 4096*1024,
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

static GString *slack_api_encode_url(SlackAccount *sa, const char *pfx, const char *endpoint, va_list qargs) {
	GString *url = g_string_new(NULL);
	g_string_printf(url, "%s/%s%s?token=%s", sa->api_url, pfx, endpoint, sa->token);

	const char *param;
	while ((param = va_arg(qargs, const char*))) {
		const char *val = va_arg(qargs, const char*);
		g_string_append_printf(url, "&%s=%s", param, purple_url_encode(val));
	}

	return url;
}

static char *slack_api_encode_post_request(SlackAccount *sa, const char *url, va_list qargs) {
	GString *request;
	gchar *host = NULL, *path = NULL;
	purple_url_parse(url, &host, NULL, &path, NULL, NULL);

	GString *postdata;
	const char *param;
	gboolean sep = FALSE;

	postdata = g_string_new("{");
	while ((param = va_arg(qargs, const char*))) {
		const char *val = va_arg(qargs, const char*);
		if (sep)
			g_string_append_c(postdata, ',');
		append_json_string(postdata, param);
		g_string_append_c(postdata, ':');
		append_json_string(postdata, val);
		sep = TRUE;
	}
	g_string_append_c(postdata, '}');

	request = g_string_new(NULL);
	g_string_append_printf(request, "\
POST /%s HTTP/1.0\r\n\
Host: %s\r\n\
Authorization: Bearer %s\r\n\
Content-Type: application/json;charset=utf-8\r\n\
Content-Length: %" G_GSIZE_FORMAT "\r\n\
\r\n",
		path, host, sa->token, postdata->len);
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

void slack_api_get(SlackAccount *sa, SlackAPICallback callback, gpointer user_data, const char *endpoint, ...)
{
	va_list qargs;
	va_start(qargs, endpoint);
	GString *url = slack_api_encode_url(sa, "", endpoint, qargs);
	va_end(qargs);

	slack_api_call_url(sa, callback, user_data, url->str, NULL);

	g_string_free(url, TRUE);
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
