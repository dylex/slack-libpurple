#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-channel.h"
#include "slack-message.h"
#include "slack-im.h"
#include "slack-message.h"
#include "slack-conversation.h"

static SlackObject *conversation_update(SlackAccount *sa, json_value *json) {
	if (json_get_prop_boolean(json, "is_im", FALSE))
		return (SlackObject*)slack_im_set(sa, json, NULL, TRUE, FALSE);
	else
		return (SlackObject*)slack_channel_set(sa, json, SLACK_CHANNEL_UNKNOWN);
}

#define CONVERSATIONS_LIST_CALL(sa, ARGS...) \
	slack_api_post(sa, conversations_list_cb, NULL, "conversations.list", "types", "public_channel,private_channel,mpim,im", "exclude_archived", "true", SLACK_PAGINATE_LIMIT_ARG, ##ARGS, NULL)

static gboolean conversations_list_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json_value *chans = json_get_prop_type(json, "channels", array);
	if (!chans) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error ?: "Missing conversation list");
		return FALSE;
	}

	for (unsigned i = 0; i < chans->u.array.length; i++)
		conversation_update(sa, chans->u.array.values[i]);

	char *cursor = json_get_prop_strptr(json_get_prop(json, "response_metadata"), "next_cursor");
	if (cursor && *cursor)
		CONVERSATIONS_LIST_CALL(sa, "cursor", cursor);
	else
		slack_login_step(sa);
	return FALSE;
}

void slack_conversations_load(SlackAccount *sa) {
	// g_hash_table_remove_all(sa->channels);
	// g_hash_table_remove_all(sa->ims);
	CONVERSATIONS_LIST_CALL(sa);
}

static inline void conversation_counts_check_unread(SlackAccount *sa, SlackObject *conv, json_value *json, gboolean load_history) {
	if (!conv || !load_history)
		return;
	gboolean has_unreads = FALSE;
	if (json_get_prop_val(json, "has_unreads", boolean, FALSE) ||
			json_get_prop_val(json, "unread_count", integer, 0) > 0)
		has_unreads = TRUE;
	if (!has_unreads || json_get_prop_val(json, "is_muted", boolean, FALSE))
		return;
	const char *since = json_get_prop_strptr(json, "last_read");
	if (!since)
		return;
	slack_get_history(sa, conv, since, /* TODO pagination */ SLACK_HISTORY_LIMIT_COUNT, NULL, FALSE);
}

static inline void conversation_counts_channels(SlackAccount *sa, json_value *json, const char *prop, SlackChannelType type, gboolean load_history) {
	json_value *chans = json_get_prop_type(json, prop, array);
	if (!chans)
		return;
	for (unsigned i = 0; i < chans->u.array.length; i++) {
		json_value *j = chans->u.array.values[i];
		SlackChannel *chan = slack_channel_set(sa, j, type);
		conversation_counts_check_unread(sa, (SlackObject *)chan, j, load_history);
	}
}

static gboolean conversation_counts_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	if (error) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error);
		return FALSE;
	}

	gboolean load_history = purple_account_get_bool(sa->account, "connect_history", FALSE);

	json_value *ims = json_get_prop_type(json, "ims", array);
	for (unsigned i = 0; i < ims->u.array.length; i++) {
		json_value *im = ims->u.array.values[i];
		const char *user_id = json_get_prop_strptr(im, "user_id");
		if (!user_id)
			continue;
		/* hopefully this is the right name? */
		SlackUser *user = slack_user_set(sa, user_id, json_get_prop_strptr(im, "name"));
		slack_im_set(sa, im, user, TRUE, FALSE);
		conversation_counts_check_unread(sa, (SlackObject *)user, im, load_history);
	}

	load_history = load_history && purple_account_get_bool(sa->account, "open_history", FALSE);
	conversation_counts_channels(sa, json, "channels", SLACK_CHANNEL_PUBLIC, load_history);
	conversation_counts_channels(sa, json, "groups", SLACK_CHANNEL_GROUP, load_history);
	conversation_counts_channels(sa, json, "mpims", SLACK_CHANNEL_MPIM, load_history);

	slack_login_step(sa);
	return FALSE;
}

void slack_conversation_counts(SlackAccount *sa) {
	/* Private API, not documented. Found by EionRobb (Github). */
	slack_api_post(sa, conversation_counts_cb, NULL, "users.counts", "mpim_aware", "true", "only_relevant_ims", "true", "simple_unreads", "true", NULL);
}

SlackObject *slack_conversation_get_conversation(SlackAccount *sa, PurpleConversation *conv) {
	switch (conv->type) {
		case PURPLE_CONV_TYPE_IM:
			return g_hash_table_lookup(sa->user_names, purple_conversation_get_name(conv));
		case PURPLE_CONV_TYPE_CHAT:
			return g_hash_table_lookup(sa->channel_cids, GUINT_TO_POINTER(purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv))));
		default:
			return NULL;
	}
}

struct conversation_retrieve {
	SlackConversationCallback *cb;
	gpointer data;
	json_value *json;
};

static void conversation_retrieve_user_cb(SlackAccount *sa, gpointer data, SlackUser *user) {
	struct conversation_retrieve *lookup = data;
	json_value *chan = json_get_prop_type(lookup->json, "channel", object);
	lookup->cb(sa, lookup->data, conversation_update(sa, chan));
	json_value_free(lookup->json);
	g_free(lookup);
}

static gboolean conversation_retrieve_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct conversation_retrieve *lookup = data;
	json_value *chan = json_get_prop_type(json, "channel", object);
	if (!chan || error) {
		purple_debug_error("slack", "Error retrieving conversation: %s\n", error ?: "missing");
		lookup->cb(sa, lookup->data, NULL);
		g_free(lookup);
		return FALSE;
	}
	lookup->json = json;
	if (json_get_prop_boolean(json, "is_im", FALSE)) {
		/* Make sure we know the user, too */
		const char *uid = json_get_prop_strptr(json, "user");
		if (uid) {
			slack_user_retrieve(sa, uid, conversation_retrieve_user_cb, lookup);
			return TRUE;
		}
	}
	conversation_retrieve_user_cb(sa, lookup, NULL);
	return TRUE;
}

void slack_conversation_retrieve(SlackAccount *sa, const char *sid, SlackConversationCallback *cb, gpointer data) {
	SlackObject *obj = slack_conversation_lookup_sid(sa, sid);
	if (obj)
		return cb(sa, data, obj);
	struct conversation_retrieve *lookup = g_new(struct conversation_retrieve, 1);
	lookup->cb = cb;
	lookup->data = data;
	slack_api_post(sa, conversation_retrieve_cb, lookup, "conversations.info", "channel", sid, NULL);
}

static gboolean mark_conversation_timer(gpointer data) {
	SlackAccount *sa = data;
	sa->mark_timer = 0; /* always return FALSE */

	/* we just send them all at once -- maybe would be better to chain? */
	SlackObject *next = sa->mark_list;
	sa->mark_list = MARK_LIST_END;
	while (next != MARK_LIST_END) {
		SlackObject *obj = next;
		next = obj->mark_next;
		obj->mark_next = NULL;
		g_free(obj->last_mark);
		obj->last_mark = g_strdup(obj->last_read);
		slack_api_post(sa, NULL, NULL, "conversations.mark", "channel", slack_conversation_id(obj), "ts", obj->last_mark, NULL);
	}

	return FALSE;
}

void slack_mark_conversation(SlackAccount *sa, PurpleConversation *conv) {
	SlackObject *obj = slack_conversation_get_conversation(sa, conv);
	if (!obj)
		return;

	int c = GPOINTER_TO_INT(purple_conversation_get_data(conv, "unseen-count"));
	if (c != 0)
		/* we could update read count to farther back, but best to only move it forward to latest */
		return;

	if (slack_ts_cmp(obj->last_mesg, obj->last_mark) <= 0)
		return; /* already marked newer */
	g_free(obj->last_read);
	obj->last_read = g_strdup(obj->last_mesg);

	if (obj->mark_next)
		return; /* already on list */

	/* add to list */
	obj->mark_next = sa->mark_list;
	sa->mark_list = obj;

	if (sa->mark_timer)
		return; /* already running */

	/* start */
	sa->mark_timer = purple_timeout_add_seconds(5, mark_conversation_timer, sa);
}

struct get_history {
	SlackObject *conv;
	char *since;
	gboolean thread;
	gboolean force_threads;
};

void slack_get_history_free(struct get_history *h) {
	g_object_unref(h->conv);
	g_free(h->since);
	g_free(h);
}

static gboolean get_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct get_history *h = data;
	json_value *list = json_get_prop_type(json, "messages", array);

	if (!list || error) {
		purple_debug_error("slack", "Error loading channel history: %s\n", error ?: "missing");
	} else {
		gboolean display_threads = !h->thread && purple_account_get_bool(sa->account, "display_threads", TRUE);

		// Annoying. Conversations are listed in reverse order,
		// whereas threads are listed in correct order.
		for (int i = h->thread ? 0 : list->u.array.length-1;
				h->thread ? i < list->u.array.length : i >= 0;
				h->thread ? i++ : i--) {

			json_value *msg = list->u.array.values[i];
			if (g_strcmp0(json_get_prop_strptr(msg, "type"), "message"))
				continue;

			const char *ts = json_get_prop_strptr(msg, "ts");
			const char *thread_ts = json_get_prop_strptr(msg, "thread_ts");
			if (thread_ts && !slack_ts_cmp(ts, thread_ts)) {
				if (h->thread && !h->force_threads)
					// When we are fetching threads, don't display
					// the parent message, because it has already
					// been displayed when fetching the non-thread
					// messages.
					continue;
				if (display_threads) {
					const char *latest_reply = json_get_prop_strptr(msg, "latest_reply");
					if (!latest_reply || !h->since || slack_ts_cmp(latest_reply, h->since) > 0)
						slack_get_history(sa, h->conv, h->since, SLACK_HISTORY_LIMIT_COUNT, thread_ts, FALSE);
				}
			}

			if (!ts || !h->since || slack_ts_cmp(ts, h->since) > 0)
				slack_handle_message(sa, h->conv, msg, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_DELAYED, h->force_threads);
		}
		/* TODO: pagination has_more? */
	}

	slack_get_history_free(h);
	return FALSE;
}

void slack_get_history(SlackAccount *sa, SlackObject *conv, const char *since, unsigned count, const char *thread_ts, gboolean force_threads) {
	purple_debug_misc("slack", "get_history %s %u\n", since, count);

	if (count == 0)
		return;

	if (since && !slack_ts_cmp(since, "0000000000.000000"))
		/* even though it gives this as a last_read, it doesn't like it in since */
		since = NULL;

	if (SLACK_IS_CHANNEL(conv)) {
		SlackChannel *chan = (SlackChannel*)conv;
		if (!chan->cid) {
			if (purple_account_get_bool(sa->account, "open_history", FALSE)) {
				/* this will call back into get_history */
				slack_chat_open(sa, chan);
			}
			return;
		}
	}
	const char *id = slack_conversation_id(conv);
	g_return_if_fail(id);

	struct get_history *h = g_new(struct get_history, 1);
	h->conv = g_object_ref(conv);
	h->since = g_strdup(since);
	h->thread = (thread_ts != NULL);
	h->force_threads = force_threads;

	if (!thread_ts && since && purple_account_get_bool(sa->account, "thread_history", FALSE)) {
		/*
		  To get thread replies we have to get around some serious deficiences
		  in the public API: There no way to query replies by date. And we have
		  to do that in order to get automatic history when you log in. The only
		  way I have found is to query as many channel messages as possible,
		  check what their "latest_reply" field is, and use those to decide what
		  threads to query. Checking all messages this way is completely
		  unrealistic, but fortunately, threads tend to be short-lived, so we
		  can use one single API call to get the last 1000 messages (maximum
		  that Slack allows), and check that without much risk of being rate
		  limited. This means the mechanics for querying by time is (mostly)
		  moved to the client side, and when limiting by time, we still fetch
		  all the last 1000 messages, but we only display those that match the
		  time range.

		  Yes, this is horrible, and it will not work for any thread older than
		  1000 main-channel messages ago. If anyone knows how to query threads
		  in a more efficient way, I'm very interested in hearing it.
		*/
		since = NULL;
		count = SLACK_HISTORY_LIMIT_COUNT;
	}

	char count_buf[6] = "";
	snprintf(count_buf, 5, "%u", MIN(count, SLACK_HISTORY_LIMIT_COUNT));
	if (thread_ts)
		slack_api_post(sa, get_history_cb, h, "conversations.replies", "channel", id, "oldest", since ?: "0", "limit", count_buf, "ts", thread_ts, NULL);
	else
		slack_api_post(sa, get_history_cb, h, "conversations.history", "channel", id, "oldest", since ?: "0", "limit", count_buf, NULL);
}

void slack_get_history_unread(SlackAccount *sa, SlackObject *conv, json_value *json) {
	slack_get_history(sa, conv,
			json_get_prop_strptr(json, "last_read"),
			json_get_prop_val(json, "unread_count", integer, -1),
			NULL,
			FALSE);
}

static gboolean get_conversation_unread_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackObject *conv = data;
	json = json_get_prop_type(json, "channel", object);

	if (!json || error) {
		purple_debug_error("slack", "Error getting conversation unread info: %s\n", error ?: "missing");
	}
	else {
		slack_get_history_unread(sa, conv, json);
	}
	g_object_unref(conv);
	return FALSE;
}

void slack_get_conversation_unread(SlackAccount *sa, SlackObject *conv) {
	const char *id = slack_conversation_id(conv);
	g_return_if_fail(id);
	slack_api_post(sa, get_conversation_unread_cb, g_object_ref(conv), "conversations.info", "channel", id, NULL);
}
