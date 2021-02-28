#include <errno.h>

#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-blist.h"
#include "slack-rtm.h"
#include "slack-message.h"
#include "slack-user.h"
#include "slack-conversation.h"
#include "slack-channel.h"

G_DEFINE_TYPE(SlackChannel, slack_channel, SLACK_TYPE_OBJECT);

static void slack_channel_finalize(GObject *gobj) {
	// SlackChannel *chan = SLACK_CHANNEL(gobj);

	G_OBJECT_CLASS(slack_channel_parent_class)->finalize(gobj);
}

static void slack_channel_class_init(SlackChannelClass *klass) {
	GObjectClass *gobj = G_OBJECT_CLASS(klass);
	gobj->finalize = slack_channel_finalize;
}

static void slack_channel_init(SlackChannel *self) {
}

PurpleConvChat *slack_channel_get_conversation(SlackAccount *sa, SlackChannel *chan) {
	g_return_val_if_fail(chan, NULL);
	if (!chan->cid)
		return NULL;
	return PURPLE_CONV_CHAT(purple_find_chat(sa->gc, chan->cid));
}

static void channel_depart(SlackAccount *sa, SlackChannel *chan) {
	if (chan->cid) {
		serv_got_chat_left(sa->gc, chan->cid);
		g_hash_table_remove(sa->channel_cids, GUINT_TO_POINTER(chan->cid));
		chan->cid = 0;
	}
	if (chan->object.buddy) {
		slack_blist_uncache(sa, chan->object.buddy);
		purple_blist_remove_chat(channel_buddy(chan));
		chan->object.buddy = NULL;
	}
}

SlackChannel *slack_channel_set(SlackAccount *sa, json_value *json, SlackChannelType type) {
	const char *sid = json_get_strptr(json);
	if (sid)
		json = NULL;
	else
		sid = json_get_prop_strptr(json, "id");
	if (!sid)
		return NULL;
	slack_object_id id;
	slack_object_id_set(id, sid);

	SlackChannel *chan = g_hash_table_lookup(sa->channels, id);

	     if (json_get_prop_boolean(json, "is_archived", FALSE))
		type = SLACK_CHANNEL_DELETED;
	else if (json_get_prop_boolean(json, "is_mpim", FALSE))
		type = SLACK_CHANNEL_MPIM;
	else if (json_get_prop_boolean(json, "is_group", FALSE))
		type = SLACK_CHANNEL_GROUP;
	else if (json_get_prop_boolean(json, "is_member", FALSE) || json_get_prop_boolean(json, "is_general", FALSE))
		type = SLACK_CHANNEL_MEMBER;
	else if (json_get_prop_boolean(json, "is_channel", FALSE))
		type = SLACK_CHANNEL_PUBLIC;

	if (type == SLACK_CHANNEL_DELETED) {
		if (!chan)
			return NULL;
		channel_depart(sa, chan);
		if (chan->object.name)
			g_hash_table_remove(sa->channel_names, chan->object.name);
		g_hash_table_remove(sa->channels, id);
		return NULL;
	}

	g_return_val_if_fail(chan || json, NULL);

	if (!chan) {
		chan = g_object_new(SLACK_TYPE_CHANNEL, NULL);
		slack_object_id_copy(chan->object.id, id);
		g_hash_table_replace(sa->channels, chan->object.id, chan);
	}

	if (type > SLACK_CHANNEL_UNKNOWN)
		chan->type = type;

	const char *name = json_get_prop_strptr(json, "name");

	if (name && g_strcmp0(chan->object.name, name)) {
		purple_debug_misc("slack", "channel %s: %s %d\n", sid, name, type);
		
		if (chan->object.name)
			g_hash_table_remove(sa->channel_names, chan->object.name);
		g_free(chan->object.name);
		chan->object.name = g_strdup(name);
		g_hash_table_insert(sa->channel_names, chan->object.name, chan);
		if (chan->object.buddy)
			g_hash_table_insert(channel_buddy(chan)->components, "name", g_strdup(chan->object.name));
	}

	if (!chan->object.buddy && chan->type >= SLACK_CHANNEL_MEMBER) {
		chan->object.buddy = g_hash_table_lookup(sa->buddies, sid);
		if (chan->object.buddy && PURPLE_BLIST_NODE_IS_CHAT(chan->object.buddy)) {
			/* While the docs say to use NULL key_destructor, libpurple actually uses g_free when loading buddies, so we recreate components here */
			if (channel_buddy(chan)->components)
				g_hash_table_destroy(channel_buddy(chan)->components);
			channel_buddy(chan)->components = slack_chat_info_defaults(sa->gc, chan->object.name);
		} else {
			chan->object.buddy = PURPLE_BLIST_NODE(purple_chat_new(sa->account, chan->object.name,
					slack_chat_info_defaults(sa->gc, chan->object.name)));
			slack_blist_cache(sa, chan->object.buddy, sid);
			purple_blist_add_chat(channel_buddy(chan), sa->blist, NULL);
		}
	}
	else if (chan->type < SLACK_CHANNEL_MEMBER) {
		channel_depart(sa, chan);
	}

	return chan;
}

void slack_channel_update(SlackAccount *sa, json_value *json, SlackChannelType event) {
	slack_channel_set(sa, json_get_prop(json, "channel"), event);
}

struct join_channel {
	SlackChannel *chan;
	char *name;
};

static void join_channel_free(struct join_channel *join) {
	if (join->chan)
		g_object_unref(join->chan);
	g_free(join->name);
	g_free(join);
}

static gboolean channels_members_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackChannel *chan = data;
	purple_debug_misc("slack", "Adding members to %s\n", chan->object.id);
	json_value *members = json_get_prop_type(json, "members", array);

	if (!members || error) {
		purple_debug_error("slack", "Error adding members to channel: %s\n", error ?: "missing");
		return FALSE;
	}

	PurpleConvChat *conv = slack_channel_get_conversation(sa, chan);
	if (!conv) {
		purple_debug_error("slack", "Error finding conversation in channel: %s\n", chan->object.id);
		return FALSE;
	}

	if (members) {
		GList *users = NULL, *flags = NULL;
		for (unsigned i = members->u.array.length; i; i --) {
			SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, json_get_strptr(members->u.array.values[i-1]));
			if (!user)
				continue;
			users = g_list_prepend(users, user->object.name);
			PurpleConvChatBuddyFlags flag = PURPLE_CBFLAGS_VOICE;
			flags = g_list_prepend(flags, GINT_TO_POINTER(flag));
		}

		purple_conv_chat_add_users(conv, users, NULL, flags, FALSE);
		g_list_free(users);
		g_list_free(flags);
	}

	// check to see if we need to fetch more pages
	json_value *metadata = json_get_prop_type(json, "response_metadata", object);
	char *next_cursor = json_get_prop_strptr(metadata, "next_cursor");
	if (strcmp(next_cursor, "")) {
		slack_api_get(sa, channels_members_cb, chan, "conversations.members", "channel", chan->object.id, "cursor", next_cursor, NULL);
	}

	return FALSE;
}

static gboolean channels_info_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json = json_get_prop_type(json, "channel", object);

	if (!json || error) {
		purple_debug_error("slack", "Error loading channel info: %s\n", error ?: "missing");
		return FALSE;
	}

	SlackChannel *chan = slack_channel_set(sa, json, SLACK_CHANNEL_PUBLIC);

	PurpleConvChat *conv = slack_channel_get_conversation(sa, chan);
	if (!conv)
		return FALSE;

	json_value *topic = json_get_prop_type(json, "topic", object);
	if (topic) {
		SlackUser *topic_user = (SlackUser*)slack_object_hash_table_lookup(sa->users, json_get_prop_strptr(topic, "creator"));
		purple_conv_chat_set_topic(conv, topic_user ? topic_user->object.name : NULL, json_get_prop_strptr(json, "value"));
	}

	slack_api_get(sa, channels_members_cb, chan, "conversations.members", "channel", chan->object.id, NULL);

	if (purple_account_get_bool(sa->account, "get_history", FALSE)) {
		slack_get_history_unread(sa, &chan->object, json);
	}
	return FALSE;
}

void slack_chat_open(SlackAccount *sa, SlackChannel *chan) {
	g_warn_if_fail(chan->type >= SLACK_CHANNEL_MEMBER);

	if (chan->cid)
		return;

	chan->cid = ++sa->cid;
	g_hash_table_insert(sa->channel_cids, GUINT_TO_POINTER(chan->cid), chan);

	serv_got_joined_chat(sa->gc, chan->cid, chan->object.name);

	slack_api_get(sa, channels_info_cb, GINT_TO_POINTER(chan->type), "conversations.info", "channel", chan->object.id, NULL);
}

static gboolean channels_join_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct join_channel *join = data;

	SlackChannel *chan = json
		? slack_channel_set(sa, json_get_prop(json, "channel"), SLACK_CHANNEL_MEMBER)
		: join->chan;

	if (!chan || error) {
		purple_notify_error(sa->gc, "Invalid Channel", "Could not join channel", error ?: join->name);
		/* hacky: reconstruct info */
		GHashTable *info = slack_chat_info_defaults(sa->gc, join->name);
		purple_serv_got_join_chat_failed(sa->gc, info);
		join_channel_free(join);
		return FALSE;
	}

	slack_chat_open(sa, chan);

	join_channel_free(join);
	return FALSE;
}

void slack_join_chat(PurpleConnection *gc, GHashTable *info) {
	SlackAccount *sa = gc->proto_data;

	const char *name = g_hash_table_lookup(info, "name");
	g_return_if_fail(name);

	if (*name == '#')
		name++;
	SlackChannel *chan = g_hash_table_lookup(sa->channel_names, name);
	if (chan)
		g_object_ref(chan);
	struct join_channel *join = g_new0(struct join_channel, 1);
	if (chan)
		join->chan = g_object_ref(chan);
	join->name = g_strdup(name);

	if (chan && chan->type >= SLACK_CHANNEL_MEMBER)
		channels_join_cb(sa, join, NULL, NULL);
	else
		slack_api_post(sa, channels_join_cb, join, "converstations.join", "name", name, NULL);
}

void slack_chat_leave(PurpleConnection *gc, int cid) {
	SlackAccount *sa = gc->proto_data;
	SlackChannel *chan = g_hash_table_lookup(sa->channel_cids, GUINT_TO_POINTER(cid));
	if (!chan)
		return;
	g_hash_table_remove(sa->channel_cids, GUINT_TO_POINTER(cid));
	chan->cid = 0;
}

struct send_chat {
	SlackChannel *chan;
	int cid;
	PurpleMessageFlags flags;
};

static void send_chat_free(struct send_chat *send) {
	g_object_unref(send->chan);
	g_free(send);
}

static void send_chat_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct send_chat *send = data;

	/* XXX better way to present chat errors? */
	if (error) {
		purple_conv_present_error(send->chan->object.name, sa->account, error);
		send_chat_free(send);
		return;
	}

	json_value *ts = json_get_prop(json, "ts");
	const char *tss = json_get_strptr(ts);

	if (send->chan->object.last_sent)
		g_free(send->chan->object.last_sent);
	send->chan->object.last_sent = g_strdup(tss);

	/* if we've already received this sent message, don't re-display it (#79) */
	if (slack_ts_cmp(tss, send->chan->object.last_mesg) != 0) {
		GString *html = g_string_new(NULL);
		slack_json_to_html(html, sa, json, &send->flags);
		time_t mt = slack_parse_time(ts);
		serv_got_chat_in(sa->gc, send->cid, purple_connection_get_display_name(sa->gc), send->flags, html->str, mt);
		g_string_free(html, TRUE);
	}

	send_chat_free(send);
}

int slack_chat_send(PurpleConnection *gc, int cid, const char *msg, PurpleMessageFlags flags) {
	SlackAccount *sa = gc->proto_data;

	SlackChannel *chan = g_hash_table_lookup(sa->channel_cids, GUINT_TO_POINTER(cid));
	if (!chan)
		return -ENOENT;

	gchar *m = slack_html_to_message(sa, msg, flags);
	glong mlen = g_utf8_strlen(m, 16384);
	if (mlen > 4000) {
		g_free(m);
		return -E2BIG;
	}

	struct send_chat *send = g_new0(struct send_chat, 1);
	send->chan = g_object_ref(chan);
	send->cid = cid;
	send->flags = flags;

	GString *channel = append_json_string(g_string_new(NULL), chan->object.id);
	GString *text = append_json_string(g_string_new(NULL), m);
	slack_rtm_send(sa, send_chat_cb, send, "message", "channel", channel->str, "text", text->str, NULL);
	g_string_free(channel, TRUE);
	g_string_free(text, TRUE);
	g_free(m);

	return 1;
}

void slack_member_joined_channel(SlackAccount *sa, json_value *json, gboolean joined) {
	SlackChannel *chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, json_get_prop_strptr(json, "channel"));
	if (!chan)
		return;

	PurpleConvChat *conv = slack_channel_get_conversation(sa, chan);
	if (!conv)
		return;

	const char *user_id = json_get_prop_strptr(json, "user");
	SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);
	if (joined) {
		PurpleConvChatBuddyFlags flag = PURPLE_CBFLAGS_VOICE;
		/* TODO we don't know creator here */
		purple_conv_chat_add_user(conv, user ? user->object.name : user_id, NULL, flag, TRUE);
	} else
		purple_conv_chat_remove_user(conv, user ? user->object.name : user_id, NULL);
}

void slack_chat_invite(PurpleConnection *gc, int cid, const char *message, const char *who) {
	SlackAccount *sa = gc->proto_data;

	SlackChannel *chan = g_hash_table_lookup(sa->channel_cids, GUINT_TO_POINTER(cid));
	if (!chan)
		return;

	SlackUser *user = g_hash_table_lookup(sa->user_names, who);
	if (!user)
		return;

	slack_api_post(sa, NULL, NULL, "conversations.invite", "channel", chan->object.id, "user", user->object.id, NULL);
}

void slack_set_chat_topic(PurpleConnection *gc, int cid, const char *topic) {
	SlackAccount *sa = gc->proto_data;

	SlackChannel *chan = g_hash_table_lookup(sa->channel_cids, GUINT_TO_POINTER(cid));
	if (!chan)
		return;

	slack_api_post(sa, NULL, NULL, "conversations.setTopic", "channel", chan->object.id, "topic", topic, NULL);
}
