#include <string.h>

#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-user.h"
#include "slack-im.h"
#include "slack-blist.h"
#include "slack-message.h"
#include "slack-channel.h"
#include "slack-rtm.h"

struct _SlackRTMCall {
	SlackAccount *sa;
	SlackRTMCallback *callback;
	gpointer data;
};

static gboolean rtm_msg(SlackAccount *sa, const char *type, json_value *json) {
	if (!strcmp(type, "message")) {
		return slack_message(sa, json);
	}
	else if (!strcmp(type, "user_typing")) {
		slack_user_typing(sa, json);
	}
	else if (!strcmp(type, "presence_change") ||
	         !strcmp(type, "presence_change_batch")) {
		slack_presence_change(sa, json);
	}
	else if (!strcmp(type, "im_close")) {
		slack_im_close(sa, json);
	}
	else if (!strcmp(type, "im_open")) {
		slack_im_open(sa, json);
	}
	else if (!strcmp(type, "member_joined_channel")) {
		slack_member_joined_channel(sa, json, TRUE);
	}
	else if (!strcmp(type, "member_left_channel")) {
		slack_member_joined_channel(sa, json, FALSE);
	}
	else if (!strcmp(type, "user_change") ||
		 !strcmp(type, "team_join")) {
		slack_user_changed(sa, json);
	}
	else if (!strcmp(type, "im_created")) {
		/* not necessarily (and probably in reality never) open, but works as no-op in that case */
		slack_im_open(sa, json);
	}
	else if (!strcmp(type, "channel_joined")) {
		slack_channel_update(sa, json, SLACK_CHANNEL_MEMBER);
	}
	else if (!strcmp(type, "group_joined") ||
		 !strcmp(type, "group_unarchive")) {
		slack_channel_update(sa, json, SLACK_CHANNEL_GROUP);
	}
	else if (!strcmp(type, "channel_left") ||
	         !strcmp(type, "channel_created") ||
	         !strcmp(type, "channel_unarchive")) {
		slack_channel_update(sa, json, SLACK_CHANNEL_PUBLIC);
	}
	else if (!strcmp(type, "channel_rename") ||
		 !strcmp(type, "group_rename")) {
		slack_channel_update(sa, json, SLACK_CHANNEL_UNKNOWN);
	}
	else if (!strcmp(type, "channel_archive") ||
		 !strcmp(type, "channel_deleted") ||
		 !strcmp(type, "group_archive") ||
		 !strcmp(type, "group_left")) {
		slack_channel_update(sa, json, SLACK_CHANNEL_DELETED);
	}
	else if (!strcmp(type, "hello")) {
		slack_login_step(sa);
	}
	else {
		purple_debug_info("slack", "Unhandled RTM type %s\n", type);
	}
	return FALSE;
}

static void rtm_cb(PurpleWebsocket *ws, gpointer data, PurpleWebsocketOp op, const guchar *msg, size_t len) {
	SlackAccount *sa = data;

	purple_debug_misc("slack", "RTM %x: %.*s\n", op, (int)len, msg);
	switch (op) {
		case PURPLE_WEBSOCKET_TEXT:
			break;
		case PURPLE_WEBSOCKET_ERROR:
		case PURPLE_WEBSOCKET_CLOSE:
			purple_connection_error_reason(sa->gc,
					PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
					(const char *)msg ?: "RTM connection closed");
			sa->rtm = NULL;
			break;
		case PURPLE_WEBSOCKET_OPEN:
			slack_login_step(sa);
		default:
			return;
	}

	json_value *json = json_parse((const char *)msg, len);
	json_value *reply_to = json_get_prop_type(json, "reply_to", integer);
	const char *type = json_get_prop_strptr(json, "type");

	if (reply_to) {
		SlackRTMCall *call = g_hash_table_lookup(sa->rtm_call, GUINT_TO_POINTER((guint) reply_to->u.integer));
		if (call) {
			g_hash_table_steal(sa->rtm_call, GUINT_TO_POINTER((guint) reply_to->u.integer));
			if (!json_get_prop_boolean(json, "ok", FALSE)) {
				json_value *err = json_get_prop(json, "error");
				if (err->type == json_object)
					err = json_get_prop(err, "msg");
				err = json_get_type(err, string);
				call->callback(call->sa, call->data, json, err ? err->u.string.ptr : "Unknown error");
			} else
				call->callback(call->sa, call->data, json, NULL);
			g_free(call);
		}
	}
	else if (type) {
		if (rtm_msg(sa, type, json))
			json = NULL;
	}
	else {
		purple_debug_error("slack", "RTM: %.*s\n", (int)len, msg);
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
				"Could not parse RTM JSON");
	}

	if (json)
		json_value_free(json);
}

static gboolean ping_timer(gpointer data) {
	SlackAccount *sa = data;

	PurplePresence *pres = purple_account_get_presence(sa->account);
	if (pres && purple_presence_get_idle_time(pres) == 0)
		slack_rtm_send(sa, NULL, NULL, "tickle", NULL);
	else
		/* we don't care about the response (at this point) so just send a uni-directional PONG */
		purple_websocket_send(sa->rtm, PURPLE_WEBSOCKET_PONG, NULL, 0);
	return TRUE;
}

static gboolean rtm_connect_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	if (error) {
		purple_connection_error_reason(sa->gc, slack_api_connection_error(error), error);
		return FALSE;
	}

	if (sa->rtm) {
		purple_websocket_abort(sa->rtm);
		sa->rtm = NULL;
	}

	const char *url     = json_get_prop_strptr(json, "url");
	if (sa->self)
		g_object_unref(sa->self);
	sa->self = g_object_ref(slack_user_update(sa, json_get_prop_type(json, "self", object)));

	if (!url || !sa->self) {
		purple_connection_error_reason(sa->gc,
				slack_api_connection_error(error), error ?: "Missing RTM parameters");
		return FALSE;
	}

	purple_connection_set_display_name(sa->gc, sa->self->object.name);

#define SET_STR(FIELD, JSON, PROP) ({ \
		g_free(sa->FIELD); \
		sa->FIELD = g_strdup(json_get_prop_strptr(JSON, PROP)); \
	})

	json_value *team = json_get_prop_type(json, "team", object);
	SET_STR(team.id, team, "id");
	SET_STR(team.name, team, "name");
	SET_STR(team.domain, team, "domain");

#undef SET_STR

	/* now that we have team info... */
	slack_blist_init(sa);

	slack_login_step(sa);
	purple_debug_info("slack", "RTM URL: %s\n", url);
	sa->rtm = purple_websocket_connect(sa->account, url, NULL, rtm_cb, sa);

	sa->ping_timer = purple_timeout_add_seconds(60, ping_timer, sa);
	return FALSE;
}

void slack_rtm_cancel(SlackRTMCall *call) {
	/* Called from sa->rtm_call value destructor: perhaps should be more explicit */
	call->callback(call->sa, call->data, NULL, NULL);
	g_free(call);
}

void slack_rtm_send(SlackAccount *sa, SlackRTMCallback *callback, gpointer user_data, const char *type, ...) {
	g_return_if_fail(sa->rtm);
	guint id = ++sa->rtm_id;

	GString *json = g_string_new(NULL);
	g_string_printf(json, "{\"id\":%u,\"type\":\"%s\"", id, type);
	va_list qargs;
	va_start(qargs, type);
	const char *key;
	while ((key = va_arg(qargs, const char*))) {
		const char *val = va_arg(qargs, const char*);
		g_string_append_printf(json, ",\"%s\":%s", key, val);
	}
	va_end(qargs);
	g_string_append_c(json, '}');
	g_return_if_fail(json->len <= 16384);

	purple_debug_misc("slack", "RTM: %.*s\n", (int)json->len, json->str);

	if (callback) {
		SlackRTMCall *call = g_new0(SlackRTMCall, 1);
		call->sa = sa;
		call->callback = callback;
		call->data = user_data;
		g_hash_table_insert(sa->rtm_call, GUINT_TO_POINTER(id), call);
	}

	purple_websocket_send(sa->rtm, PURPLE_WEBSOCKET_TEXT, (guchar*)json->str, json->len);
	g_string_free(json, TRUE);
}

void slack_rtm_connect(SlackAccount *sa) {
	slack_api_call(sa, rtm_connect_cb, NULL, "rtm.connect", "batch_presence_aware", "1", "presence_sub", "true", NULL);
}
