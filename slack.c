#ifndef PURPLE_PLUGINS
#define PURPLE_PLUGINS
#endif

#include <string.h>

#include <accountopt.h>
#include <debug.h>
#include <plugin.h>
#include <version.h>

#include "slack.h"
#include "slack-api.h"
#include "slack-auth.h"
#include "slack-rtm.h"
#include "slack-json.h"
#include "slack-user.h"
#include "slack-im.h"
#include "slack-channel.h"
#include "slack-conversation.h"
#include "slack-blist.h"
#include "slack-message.h"
#include "slack-cmd.h"

static const char *slack_list_icon(G_GNUC_UNUSED PurpleAccount * account, G_GNUC_UNUSED PurpleBuddy * buddy) {
	return "slack";
}

static GList *slack_status_types(G_GNUC_UNUSED PurpleAccount *acct) {
	GList *types = NULL;

	types = g_list_append(types,
		purple_status_type_new_with_attrs(PURPLE_STATUS_AVAILABLE, "active", "active",
		TRUE, TRUE, FALSE,
		"message", "Message", purple_value_new(PURPLE_TYPE_STRING),
		NULL));

	types = g_list_append(types,
		purple_status_type_new_with_attrs(PURPLE_STATUS_AWAY, "away", "away",
		TRUE, TRUE, FALSE,
		"message", "Message", purple_value_new(PURPLE_TYPE_STRING),
		NULL));

	/* Even though slack never says anyone is offline, we need this status.
	 * (Maybe could treat deleted users as this?)
	 */
	types = g_list_append(types, purple_status_type_new_with_attrs(
		PURPLE_STATUS_OFFLINE, NULL, NULL,
		TRUE, TRUE, FALSE, "message", "Message",
		purple_value_new(PURPLE_TYPE_STRING), NULL));

	return types;
}

static gboolean slack_set_profile(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	GString *profile_json = data;
	slack_api_post(sa, NULL, NULL, "users.profile.set", "profile", profile_json->str, NULL);
	g_string_free(profile_json, TRUE);
	return FALSE;
}

static void slack_set_status(PurpleAccount *account, PurpleStatus *status) {
	PurpleConnection *gc = account->gc;
	if (!gc)
		return;
	SlackAccount *sa = gc->proto_data;
	g_return_if_fail(sa);

	/* Set status */
	sa->away = !purple_status_is_active(status) ||
			purple_status_type_get_primitive(purple_status_get_type(status)) == PURPLE_STATUS_AWAY;

	/* Set message */
	const char *message = purple_status_get_attr_string(status, "message");
	GString *profile_json = g_string_new("{\"status_text\":");
	if (message)
		append_json_string(profile_json, message);
	else
		g_string_append(profile_json, "\"\"");
	g_string_append(profile_json, ",\"status_emoji\":\"\"}");

	slack_api_post(sa, slack_set_profile, profile_json, "users.setPresence", "presence", sa->away ? "away" : "auto", NULL);
}

static void slack_set_idle(PurpleConnection *gc, int idle) {
	if (idle > 0)
		return;

	SlackAccount *sa = gc->proto_data;
	g_return_if_fail(sa && sa->rtm);

	if (sa->away)
		return;

	/* poke slack to maintain unidle status (also done in ping_timer) */
	slack_rtm_send(sa, NULL, NULL, "tickle", NULL);
}

static GList *slack_chat_info(PurpleConnection *gc) {
	GList *l = NULL;

	struct proto_chat_entry *e;
	e = g_new0(struct proto_chat_entry, 1);
	e->label = "_Channel:";
	e->identifier = "name";
	e->required = TRUE;
	l = g_list_append(l, e);

	return l;
}

GHashTable *slack_chat_info_defaults(PurpleConnection *gc, const char *name) {
	GHashTable *info = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	if (name)
		g_hash_table_insert(info, "name", g_strdup(name));
	/* we could look up the channel here and add more... */

	return info;
}

static char *slack_get_chat_name(GHashTable *info) {
	return g_strdup(g_hash_table_lookup(info, "name"));
}

static void slack_conversation_created(PurpleConversation *conv, void *data) {
	/* need to handle get_history for IMs (other conversations handled in slack_join_chat) */
	if (conv->type != PURPLE_CONV_TYPE_IM)
		return;
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return;
	if (!purple_account_get_bool(sa->account, "open_history", FALSE))
		return;

	SlackUser *user = g_hash_table_lookup(sa->user_names, purple_conversation_get_name(conv));
	if (!user)
		return;

	slack_get_conversation_unread(sa, &user->object);
}

static guint slack_conversation_send_typing(PurpleConversation *conv, PurpleTypingState state, gpointer userdata)
{
	PurpleConnection *gc = purple_conversation_get_gc(conv);
	
	if (!gc || !PURPLE_CONNECTION_IS_CONNECTED(gc)) {
		return 0;
	}
	if (!purple_strequal(purple_plugin_get_id(purple_connection_get_prpl(gc)), SLACK_PLUGIN_ID)) {
		return 0;
	}
	
	SlackAccount *sa = gc->proto_data;

	if (state != PURPLE_TYPING)
		return 0;

	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj)
		return 0;

	GString *channel = append_json_string(g_string_new(NULL), slack_conversation_id(obj));
	/* if ((SLACK_CHANNEL(obj)->object.thread_ts)
		slack_rtm_send(sa, NULL, NULL, "typing", "channel", channel->str, "thread_ts", chan->object.thread_ts, NULL);
	else */
		slack_rtm_send(sa, NULL, NULL, "typing", "channel", channel->str, NULL);
	g_string_free(channel, TRUE);
	
	return 3;
}

static void slack_conversation_updated(PurpleConversation *conv, PurpleConvUpdateType type, void *data) {
	/* TODO: channel TYPING? */
	if (type != PURPLE_CONV_UPDATE_UNSEEN)
		return;
	if (conv->type != PURPLE_CONV_TYPE_IM && conv->type != PURPLE_CONV_TYPE_CHAT)
		return;
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return;

	slack_mark_conversation(sa, conv);
}

static void slack_login(PurpleAccount *account) {
	PurpleConnection *gc = purple_account_get_connection(account);
	gboolean legacy_token = FALSE;
	const gchar *token = purple_account_get_string(account, "api_token", NULL);

	if(token != NULL && g_str_has_prefix(token, "xoxp-")) {
		legacy_token = TRUE;
	}

	static gboolean signals_connected = FALSE;
	if (!signals_connected) {
		signals_connected = TRUE;
		purple_signal_connect(purple_conversations_get_handle(), "conversation-created",
				gc->prpl, PURPLE_CALLBACK(slack_conversation_created), NULL);
		purple_signal_connect(purple_conversations_get_handle(), "conversation-updated",
				gc->prpl, PURPLE_CALLBACK(slack_conversation_updated), NULL);
		purple_signal_connect(purple_conversations_get_handle(), "chat-conversation-typing", 
				gc->prpl, PURPLE_CALLBACK(slack_conversation_send_typing), NULL);
	}

	const gchar *username = purple_account_get_username(account);
	const gchar *host = NULL;

	if(legacy_token) {
		/* the legacy token split on @ which we can't use because of email
		 * addresses, so we manually split it here.
		 */
		host = g_strstr_len(username, -1, "@");
		if (host) {
			gchar *percent = g_strrstr(host, "%");
			if (percent) {
				*percent = '\0';
			}
		}
	}

	/* if we had a legacy token and it failed to find a host, try the % as well
	 * since this could be an account created with the version of the plugin
	 * since mobile auth was added but a user is trying to provide a legacy
	 * api token.
	 */
	if(host == NULL) {
		host = g_strrstr(username, "%");
	}

	if (!host || !*host) {
		purple_connection_error_reason(gc,
			PURPLE_CONNECTION_ERROR_INVALID_SETTINGS, "Host setting is required");
		return;
	}
	/* move the host pointer forward one character passed the delimiter */
	host++;

	SlackAccount *sa = g_new0(SlackAccount, 1);
	gc->proto_data = sa;
	sa->account = account;
	sa->gc = gc;
	sa->host = g_strdup(host);

	/* check if we have a token and set it as the password if we do */
	if (token && *token) {
		purple_account_set_password(sa->account, token);
	}

	if(!legacy_token) {
		/* if we're not using the legacy token, grab the email out of the user
		 * split and throw a null terminator on the end.
		 */
		sa->email = g_strdup(purple_account_get_username(account));
		gchar *percent = g_strrstr(sa->email, "%");
		if (percent) {
			*percent = '\0';
		}
	}

	g_queue_init(&sa->api_calls);

	sa->rtm_call = g_hash_table_new_full(g_direct_hash,        g_direct_equal,        NULL, (GDestroyNotify)slack_rtm_cancel);

	sa->users    = g_hash_table_new_full(slack_object_id_hash, slack_object_id_equal, NULL, g_object_unref);
	sa->user_names = g_hash_table_new_full(g_str_hash,         g_str_equal,           NULL, NULL);
	sa->ims      = g_hash_table_new_full(slack_object_id_hash, slack_object_id_equal, NULL, NULL);

	sa->channels = g_hash_table_new_full(slack_object_id_hash, slack_object_id_equal, NULL, g_object_unref);
	sa->channel_names = g_hash_table_new_full(g_str_hash,      g_str_equal,           NULL, NULL);
	sa->channel_cids = g_hash_table_new_full(g_direct_hash,    g_direct_equal,        NULL, NULL);

	g_queue_init(&sa->avatar_queue);

	sa->buddies = g_hash_table_new_full(/* slack_object_id_hash, slack_object_id_equal, */ g_str_hash, g_str_equal, NULL, NULL);

	sa->emoji_ctx = slack_emoji_new();

	sa->mark_list = MARK_LIST_END;

	purple_connection_set_display_name(gc, account->alias ?: account->username);
	purple_connection_set_state(gc, PURPLE_CONNECTING);

	/* check if a token has been stored in the password field. */
	const char *password = purple_account_get_password(sa->account);
	if(g_regex_match_simple("^xox.-.+", password, 0, 0)) {
		/* the password is a token, so copy it to the token field */
		sa->token = g_strdup(password);

		/* set the api url to the property host */
		sa->api_url = g_strdup_printf("https://%s/api", sa->host);

		/* finally skip the mobile login as we already have a token */
		sa->login_step = 3;
	} else {
		if(!password || !*password) {
			purple_connection_error_reason(
				purple_account_get_connection(sa->account),
				PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
				"No password provided");

			return;
		}

		/* we do not have a token, so we have to do the mobile login.
		 * the mobile auth needs some different defaults than the rest of the
		 * prpl, so we set those here and set them to the proper values at the
		 * end of authentication.
		 */
		sa->token = g_strdup("");
		sa->api_url = g_strdup_printf("https://slack.com/api");
	}

	slack_login_step(sa);
}

void slack_login_step(SlackAccount *sa) {
	gboolean lazy = FALSE;
#define MSG(msg) \
	purple_connection_update_progress(sa->gc, msg, ++sa->login_step, 10)
	switch (sa->login_step) {
		case 0:
			MSG("Looking up team");
			slack_auth_login(sa);
			break;
		case 1:
			MSG("Finding user");
			break;
		case 2:
			MSG("Logging in");
			break;
		case 3:
			MSG("Requesting RTM");
			slack_rtm_connect(sa);
			break;
		case 4: /* slack_connect_cb */
			MSG("Connecting to RTM");
			/* purple_websocket_connect */
			break;
		case 5: /* rtm_cb */
			MSG("RTM Connected");
			break;
		case 6: /* rtm_msg("hello") */
			lazy = purple_account_get_bool(sa->account, "lazy_load", FALSE);
			MSG("Loading Users");
			if (!lazy) {
				slack_users_load(sa);
				break;
			}
		case 7:
			MSG("Loading conversations");
			if (!lazy) {
				slack_conversations_load(sa);
				break;
			}
		case 8:
			MSG("Loading active conversations");
			slack_conversation_counts(sa);
			break;
		case 9:
			slack_presence_sub(sa);
			purple_connection_set_state(sa->gc, PURPLE_CONNECTED);
	}
#undef MSG
}

static void slack_close(PurpleConnection *gc) {
	SlackAccount *sa = gc->proto_data;

	if (!sa)
		return;

	if (sa->mark_timer) {
		/* really should send final marks if we can... */
		purple_timeout_remove(sa->mark_timer);
		sa->mark_timer = 0;
	}

	if (sa->ping_timer) {
		purple_timeout_remove(sa->ping_timer);
		sa->ping_timer = 0;
	}

	if (sa->rtm) {
		purple_websocket_abort(sa->rtm);
		sa->rtm = NULL;
	}
	g_hash_table_destroy(sa->rtm_call);

	slack_api_disconnect(sa);

	g_hash_table_destroy(sa->buddies);

	g_hash_table_destroy(sa->channel_cids);
	g_hash_table_destroy(sa->channel_names);
	g_hash_table_destroy(sa->channels);

	g_hash_table_destroy(sa->ims);
	g_hash_table_destroy(sa->user_names);
	g_hash_table_destroy(sa->users);

	slack_emoji_free(sa->emoji_ctx);

#if GLIB_CHECK_VERSION(2,60,0)
	g_queue_clear_full(&sa->avatar_queue, g_object_unref);
#else
	g_queue_foreach(&sa->avatar_queue, (GFunc)g_object_unref, NULL);
#endif

	g_free(sa->team.id);
	g_free(sa->team.name);
	g_free(sa->team.domain);
	g_object_unref(sa->self);

	g_free(sa->api_url);
	g_free(sa->token);
	g_free(sa->email);
	g_free(sa->host);
	g_free(sa);
	gc->proto_data = NULL;
}

static gboolean slack_load(PurplePlugin *plugin) {
	slack_cmd_register();

	return TRUE;
}

static gboolean slack_unload(PurplePlugin *plugin) {
	slack_cmd_unregister();

	return TRUE;
}

static PurplePluginProtocolInfo prpl_info = {
	/* options */
	OPT_PROTO_CHAT_TOPIC
		| OPT_PROTO_PASSWORD_OPTIONAL
		/* TODO, requires redirecting / commands to hidden API: | OPT_PROTO_SLASH_COMMANDS_NATIVE */,
	NULL,			/* user_splits */
	NULL,			/* protocol_options */
	NO_BUDDY_ICONS,
	slack_list_icon,	/* list_icon */
	NULL,			/* list_emblems */
	slack_status_text,	/* status_text */
	NULL,			/* tooltip_text */
	slack_status_types,	/* status_types */
	slack_blist_node_menu,	/* blist_node_menu */
	slack_chat_info,	/* chat_info */
	slack_chat_info_defaults, /* chat_info_defaults */
	slack_login,		/* login */
	slack_close,		/* close */
	slack_send_im,		/* send_im */
	slack_set_info,		/* set_info */
	slack_send_typing,	/* send_typing */
	slack_get_info,		/* get_info */
	slack_set_status,	/* set_status */
	slack_set_idle,		/* set_idle */
	NULL,			/* change_passwd */
	NULL,			/* add_buddy */
	NULL,			/* add_buddies */
	NULL,			/* remove_buddy */
	NULL,			/* remove_buddies */
	NULL,			/* add_permit */
	NULL,			/* add_deny */
	NULL,			/* rem_permit */
	NULL,			/* rem_deny */
	NULL,			/* set_permit_deny */
	slack_join_chat,	/* join_chat */	
	NULL,			/* reject chat invite */
	slack_get_chat_name,	/* get_chat_name */
	slack_chat_invite,	/* chat_invite */
	slack_chat_leave,	/* chat_leave */
	NULL,			/* chat_whisper */
	slack_chat_send,	/* chat_send */
	NULL,			/* keepalive */
	NULL,			/* register_user */
	NULL,			/* get_cb_info */
	NULL,			/* get_cb_away */
	NULL,			/* alias_buddy */
	NULL,			/* group_buddy */
	NULL,			/* rename_group */
	slack_buddy_free,	/* buddy_free */
	NULL,			/* convo_closed */
	NULL,			/* normalize */
	NULL,			/* set_buddy_icon */
	NULL,			/* remove_group */
	NULL,			/* get_cb_real_name */
	slack_set_chat_topic,	/* set_chat_topic */
	NULL, // slack_find_blist_chat,	/* find_blist_chat */
	slack_roomlist_get_list,/* roomlist_get_list */
	slack_roomlist_cancel,	/* roomlist_cancel */
	slack_roomlist_expand_category,	/* roomlist_expand_category */
	NULL,			/* can_receive_file */
	NULL,			/* send_file */
	NULL,			/* new_xfer */
	NULL,			/* offline_message */
	NULL,			/* whiteboard_prpl_ops */
	NULL,			/* send_raw */
	NULL,			/* roomlist_room_serialize */
	NULL,			/* unregister_user */
	NULL,			/* send_attention */
	NULL,			/* attention_types */
	sizeof(PurplePluginProtocolInfo),	/* struct_size */
	NULL,			/* get_account_text_table */
	NULL,			/* initiate_media */
	NULL,			/* get_media_caps */
	NULL,			/* get_moods */
	NULL,			/* set_public_alias */
	NULL,			/* get_public_alias */
	NULL,			/* add_buddy_with_invite */
	NULL,			/* add_buddies_with_invite */
};

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_PROTOCOL,
	NULL,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,
	SLACK_PLUGIN_ID,
	"Slack",
	"0.2",
	"Slack protocol plugin",
	"Slack protocol support for libpurple.",
	"Dylan Simon <dylan@dylex.net>",
	"http://github.com/dylex/slack-libpurple",
	slack_load,
	slack_unload,
	NULL,
	NULL,
	&prpl_info,	/* extra info */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static void init_plugin(G_GNUC_UNUSED PurplePlugin *plugin)
{
	prpl_info.user_splits = g_list_append(prpl_info.user_splits,
		purple_account_user_split_new("Host", "slack.com", '%'));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_string_new("API token", "api_token", ""));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Open chat on channel message", "open_chat", FALSE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Display thread replies", "display_threads", TRUE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Re-display parent with indicator when thread is opened", "display_parent_indicator", TRUE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_string_new("Prepend thread replies with this string", "thread_indicator", "⤷ "));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_string_new("Prepend parent messages with this string", "parent_indicator", "◈ "));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_string_new("Thread timestamp format for the current day (time only)", "thread_timestamp", "%X"));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_string_new("Thread timestamp format for previous days (date and time)", "thread_datestamp", "%x %X"));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Retrieve unread IM (*and channel) history on connect", "connect_history", FALSE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Retrieve unread history on conversation open (*and connect)", "open_history", FALSE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Retrieve unread thread history too (slow!)", "thread_history", FALSE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Download user avatars", "enable_avatar_download", FALSE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Show members in channels (disabling may break channel features)", "channel_members", TRUE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_string_new("Prepend attachment lines with this string", "attachment_prefix", "▎ "));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Expand URLs", "expand_urls", TRUE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_bool_new("Lazy loading: only request objects on demand (EXPERIMENTAL!)", "lazy_load", FALSE));

	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options,
		purple_account_option_int_new("Seconds to delay when ratelimited", "ratelimit_delay", 15));
}

PURPLE_INIT_PLUGIN(slack, init_plugin, info);
