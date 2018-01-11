#include <debug.h>
#include <version.h>

#include "slack-json.h"
#include "slack-rtm.h"
#include "slack-api.h"
#include "slack-user.h"
#include "slack-channel.h"
#include "slack-message.h"

gchar *slack_html_to_message(SlackAccount *sa, const char *s, PurpleMessageFlags flags) {
	
	if (flags & PURPLE_MESSAGE_RAW)
		return g_strdup(s);

	GString *msg = g_string_sized_new(strlen(s));
	while (*s) {
		const char *ent;
		int len;
		if ((*s == '@' || *s == '#') && !(flags & PURPLE_MESSAGE_NO_LINKIFY)) {
			const char *e = s+1;
			/* try to find the end of this command, but not very well -- not sure what characters are valid and eventually will need to deal with spaces */
			while (g_ascii_isalnum(*e) || *e == '-' || *e == '_' || (*e == '.' && g_ascii_isalnum(e[1]))) e++;
			if (*s == '@') {
#define COMMAND(CMD, CMDL) \
				if (e-(s+1) == CMDL && !strncmp(s+1, CMD, CMDL)) { \
					g_string_append_len(msg, "<!" CMD ">", CMDL+3); \
					s = e; \
					continue; \
				}
				COMMAND("here", 4)
				COMMAND("channel", 7)
				COMMAND("everyone", 8)
			}
#undef COMMAND
			char *t = g_strndup(s+1, e-(s+1));
			SlackObject *obj = g_hash_table_lookup(*s == '@' ? sa->user_names : sa->channel_names, t);
			g_free(t);
			if (obj) {
				g_string_append_c(msg, '<');
				g_string_append_c(msg, *s);
				g_string_append(msg, obj->id);
				g_string_append_c(msg, '|');
				g_string_append_len(msg, s+1, e-(s+1));
				g_string_append_c(msg, '>');
				s = e;
				continue;
			}
		}
		if ((ent = purple_markup_unescape_entity(s, &len))) {
			if (!strcmp(ent, "&"))
				g_string_append(msg, "&amp;");
			else if (!strcmp(ent, "<"))
				g_string_append(msg, "&lt;");
			else if (!strcmp(ent, ">"))
				g_string_append(msg, "&gt;");
			else
				g_string_append(msg, ent);
			s += len;
			continue;
		}
		if (!g_ascii_strncasecmp(s, "<br>", 4)) {
			g_string_append_c(msg, '\n');
			s += 4;
			continue;
		}
		/* what about other tags? urls (auto-detected server-side)? dates? */
		g_string_append_c(msg, *s++);
	}

	return g_string_free(msg, FALSE);
}

gchar *slack_message_to_html(SlackAccount *sa, gchar *s, const char *subtype, PurpleMessageFlags *flags) {
	g_return_val_if_fail(s, NULL);

	size_t l = strlen(s);
	char *end = &s[l];
	GString *html = g_string_sized_new(l);

	if (!g_strcmp0(subtype, "me_message")) {
		g_string_append(html, "/me ");
	} else if (subtype) {
		*flags |= PURPLE_MESSAGE_SYSTEM;
	}
	*flags |= PURPLE_MESSAGE_NO_LINKIFY;

	while (s < end) {
		char c = *s++;
		if (c == '\n') {
			g_string_append(html, "<BR>");
			continue;
		}
		if (c != '<') {
			g_string_append_c(html, c);
			continue;
		}

		/* found a <tag> */
		char *r = memchr(s, '>', end-s);
		if (!r)
			/* should really be error */
			r = end;
		else
			*r = 0;
		char *b = memchr(s, '|', r-s);
		if (b) {
			*b = 0;
			b++;
		}
		switch (*s) {
			case '#':
				s++;
				g_string_append_c(html, '#');
				if (!b) {
					SlackChannel *chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, s);
					if (chan)
						b = chan->name;
				}
				g_string_append(html, b ?: s);
				break;
			case '@':
				// We should only look at this case when sa (the slack account) is passed.
				// It will not be passed for text that appears inside of attachments.
				if (sa) {
					s++;
					g_string_append_c(html, '@');
					SlackUser *user = NULL;
					if (slack_object_id_is(sa->self->object.id, s)) {
						user = sa->self;
						*flags |= PURPLE_MESSAGE_NICK;
					}
					if (!b) {
						if (!user)
							user = (SlackUser*)slack_object_hash_table_lookup(sa->users, s);
						if (user)
							b = user->name;
					}
					g_string_append(html, b ?: s);
				}
				break;
			case '!':
				s++;
				if (!strcmp(s, "channel") || !strcmp(s, "group") || !strcmp(s, "here") || !strcmp(s, "everyone")) {
					*flags |= PURPLE_MESSAGE_NICK;
					g_string_append_c(html, '@');
					g_string_append(html, b ?: s);
				} else {
					g_string_append(html, "&lt;");
					g_string_append(html, b ?: s);
					g_string_append(html, "&gt;");
				}
				break;
			default:
				/* URL */
				g_string_append(html, "<A HREF=\"");
				g_string_append(html, s); /* XXX embedded quotes? */
				g_string_append(html, "\">");
				g_string_append(html, b ?: s);
				g_string_append(html, "</A>");
		}
		s = r+1;
	}

	return g_string_free(html, FALSE);
}

static void handle_message(SlackAccount *sa, SlackObject *obj, json_value *json, PurpleMessageFlags flags) {
	const char *user_id     = json_get_prop_strptr(json, "user");
	const char *subtype     = json_get_prop_strptr(json, "subtype");
	const gboolean isHidden = json_get_prop_boolean(json, "hidden", FALSE);
	gchar *html;
	json_value *ts          = json_get_prop(json, "ts");
	time_t mt = slack_parse_time(ts);

	if (isHidden) {
		flags |= PURPLE_MESSAGE_INVISIBLE;
	}

	SlackUser *user = NULL;
	if (slack_object_id_is(sa->self->object.id, user_id)) {
		user = sa->self;
#if PURPLE_VERSION_CHECK(2,12,0)
		flags |= PURPLE_MESSAGE_REMOTE_SEND;
#endif
	}

	html = slack_json_to_html(sa, json, subtype, &flags);
	
	PurpleConversation *conv = NULL;
	if (SLACK_IS_CHANNEL(obj)) {
		SlackChannel *chan = (SlackChannel*)obj;
		/* Channel */
		if (!chan->cid) {
			if (!purple_account_get_bool(sa->account, "open_chat", FALSE)) {
				g_free(html);
				return;
			}
			slack_chat_open(sa, chan);
		}

		if (!user)
			user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);

		PurpleConvChat *chat = slack_channel_get_conversation(sa, chan);
		if (chat) {
			conv = purple_conv_chat_get_conversation(chat);
			if (!subtype);
			else if (!strcmp(subtype, "channel_topic") ||
					!strcmp(subtype, "group_topic"))
				purple_conv_chat_set_topic(chat, user ? user->name : user_id, json_get_prop_strptr(json, "topic"));
		}
		
		serv_got_chat_in(sa->gc, chan->cid, user ? user->name : user_id ?: "", flags, html, mt);
	} else if (SLACK_IS_USER(obj)) {
		SlackUser *im = (SlackUser*)obj;
		/* IM */
		conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, im->name, sa->account);
		if (slack_object_id_is(im->object.id, user_id))
			serv_got_im(sa->gc, im->name, html, flags, mt);
		else {
			if (!conv)
				conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, sa->account, im->name);
			if (!user)
				/* is this necessary? shouldn't be anyone else in here */
				user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);
			purple_conversation_write(conv, user ? user->name : user_id, html, flags, mt);
		}
	}

	g_free(html);

	/* update most recent ts for later marking */
	if (conv && json_get_strptr(ts)) {
		g_free(purple_conversation_get_data(conv, "slack:ts"));
		purple_conversation_set_data(conv, "slack:ts", g_strdup(json_get_strptr(ts)));
	}
}

void add_slack_attachments_to_buffer(GString *buffer, SlackAccount *sa, json_value *attachments, PurpleMessageFlags *flags) {
	if (attachments) {

		int length = (attachments != NULL) ? attachments->u.array.length : 0;

		for (int i=0; i < length; i++) {
			json_value *attachment = attachments->u.array.values[i];

			g_string_append_printf(
				buffer,
				"%s",
				slack_attachment_to_html(sa, attachment, flags)
			);
		}
	}
}

gchar *slack_json_to_html(SlackAccount *sa, json_value *json, const char *subtype, PurpleMessageFlags *flags) {
	GString *html = g_string_sized_new(0);
	char *s;
	json_value *message = json_get_prop(json, "message");
	json_value *message_attachments = (message != NULL) ? json_get_prop(message, "attachments") : NULL;
	json_value *attachments = json_get_prop(json, "attachments");
	

	g_string_printf(html, "");

	// If there are attachements, show them.
	add_slack_attachments_to_buffer(html, sa, attachments, flags);
	add_slack_attachments_to_buffer(html, sa, message_attachments, flags);
	
	// Check to see if the message is a special subtype.
	if (subtype) {
		// If the message was editted, we show how the message was changed.
		// We do this by displayed the changed message and what it was originally,
		// since we can't do what the official slack plugin does and rewrite history.
		if (g_strcmp0(subtype, "message_changed") == 0) {
			json_value *message = json_get_prop(json, "message");
			s = json_get_prop_strptr(message, "text");

			json_value *previous_message = json_get_prop(json, "previous_message");
			char *previous_message_text = json_get_prop_strptr(previous_message, "text");

			if (g_strcmp0(s, previous_message_text) == 0) {
				// nothing visually changed, so don't print out the difference.
				g_string_append_printf(html, "");
			} else {
				g_string_append_printf(
					html,
					"%s <font color=\"#717274\"><i>(edited)</i> <br><br><i>(Old message was \"%s\")</i></font>", 
					slack_message_to_html(sa, s, subtype, flags),
					slack_message_to_html(sa, previous_message_text, subtype, flags)
				);
			}
		// If the message was deleted, we report that it was.
		} else if (g_strcmp0(subtype, "message_deleted") == 0) {
			json_value *previous_message = json_get_prop(json, "previous_message");
			char *previous_message_text = json_get_prop_strptr(previous_message, "text");

			g_string_append_printf(
				html,
				"<font color=\"#717274\"<i>(Deleted message: \"%s\")</i></font>", 
				slack_message_to_html(sa, previous_message_text, subtype, flags)
			);
		} else {
			s = json_get_prop_strptr(json, "text");
			g_string_append_printf(
				html,
				"%s",
				slack_message_to_html(sa, s, subtype, flags)
			);
		}
	// assume this is just a text message.
	} else {
		s = json_get_prop_strptr(json, "text");
		g_string_append_printf(
			html,
			"%s",
			slack_message_to_html(sa, s, subtype, flags)
		);
	}

	return g_string_free(html, FALSE);
}

/*
 *	Changes a "slack color" (i.e. "good", "warning", "danger") to the correct
 *  RGB color.  From https://api.slack.com/docs/message-attachments
 */
gchar *get_color(char *c) {
	if (c == NULL) {
		return (gchar * ) "#717274";
	} else if (!strcmp(c, "good")) {
		return (gchar * ) "#2fa44f";
	} else if (!strcmp(c, "warning")) {
		return (gchar * ) "#de9e31";
	} else if (!strcmp(c, "danger")) {
		return (gchar * ) "#d50200";
	} else {
		return (gchar * ) c;
	}
}

gchar *get_fields(json_value *fields) {
	if (fields == NULL) {
		return "";
	} else {
		GString *html = g_string_sized_new(0);

		g_string_printf(html, "");
		for (int i=0; i<fields->u.array.length; i++) {
				json_value *field = fields->u.array.values[i];
				char *title = json_get_prop_strptr(field, "title");
				char *value = json_get_prop_strptr(field, "value");

				g_string_append_printf(
					html,
					"<br /><br /><b>%s</b><br /><i>%s</i><br />",
					(title == NULL) ? "Unknown Field Title" : title,
					(value == NULL) ? "Unknown Field Value" : value
				);
		}

		return g_string_free(html, FALSE);
	}
}

void debug(char *s) {
	printf("DEBUG: %s\n", s);
	fflush(stdout);
}

/*
 * make a link if url is not NULL.  Otherwise, just give the text back.
 * 
 * if insertBR is true, it inserts a <br /> tag.
 */
gchar *link(char *url, char *text, int insertBR) {
	GString *html = g_string_sized_new(0);

	if (!text) {
		return (gchar *) "";
	} else if (url) {
		g_string_printf(
			html,
			"<a href=\"%s\">%s</a>%s",
			url,
			text,
			insertBR ? "<br />" : ""
		);
		return g_string_free(html, FALSE);
	} else {
		return (gchar *) text;
	}
}

/*
 * Converts a single attachment to HTML.  The shape of an attachment is
 * documented at https://api.slack.com/docs/message-attachments
 */
gchar *slack_attachment_to_html(SlackAccount *sa, json_value *attachment, PurpleMessageFlags *flags) {
	GString *html = g_string_sized_new(0);

	char *service_name = json_get_prop_strptr(attachment, "service_name");
	char *service_link = json_get_prop_strptr(attachment, "service_link");
	char *author_name = json_get_prop_strptr(attachment, "author_name");
	char *author_subname = json_get_prop_strptr(attachment, "author_subname");
	
	char *author_link = json_get_prop_strptr(attachment, "author_link");
	char *text = json_get_prop_strptr(attachment, "text");

	gchar *formatted_text = slack_message_to_html(sa, text, "", flags);

	char color[16];
	snprintf(color, sizeof(color) -1, "%s", get_color(json_get_prop_strptr(attachment, "color")));
	//char *fallback = json_get_prop_strptr(attachment, "fallback");
	char *pretext = json_get_prop_strptr(attachment, "pretext");
	
	char *title = json_get_prop_strptr(attachment, "title");
	char *title_link = json_get_prop_strptr(attachment, "title_link");
	char *fields = get_fields(json_get_prop(attachment, "fields"));
	char *footer = json_get_prop_strptr(attachment, "footer");
	time_t ts = slack_parse_time(json_get_prop(attachment, "ts"));

	GString *border = g_string_sized_new(0);
	
	g_string_printf(border, "</b><font color=\"%s\">————————————</font>", color);

	// Sometimes, the text of the attachment can be *really* large.  The official
	// Slack client will truncate the text at x-characters and have a "Read More"
	// link so the user can read the rest of the text.  I wasn't sure what the
	// right thing to do for the plugin, so I implemented both the truncated
	// version as well as the "just dump all the text naively" version (the
	// latter of which is what is uncommented .. I am leaving the truncated
	// version commented in in case it is decided that this is the right thing to
	// do.

	/* GString *truncated_text = g_string_sized_new(0);
	g_string_printf(
		truncated_text,
		"%.480s%s",
		(char *) formatted_text,
		(strlen(formatted_text) > 480) ? "…" : ""
	); */

	g_string_printf(
		html,
		"<br /><font color=\"#717274\">"

			// pretext
			"%s"
			"%s"

			// top border
			"%s%s"
			"<br />"

			// service name and author name
			"<b>%s%s%s %s</b>"
			"%s"

			// title
			"<b><i>%s</i></b>"
			"%s"

			// main text
			"<i>%s</i>"

			// fields
			"%s"
			"%s"

			// bottom border
			"%s<br />"

			// footer
			"%s%s%s"

		"</font>",

		// pretext
		pretext ? slack_message_to_html(sa, pretext, "attachment", flags) : "",
		pretext ? "<br /><br />" : "",

		// top border
		!pretext ? "<br />" : "",
		border->str,

		// service name and author name
		link(service_link, service_name, FALSE),
		(service_name && author_name) ? " - " : "",
		link(author_link, author_name, FALSE),
		author_subname ? author_subname : "",
		(service_name != NULL || author_name != NULL || author_subname != NULL) ? "<br />" : "",
		
		// title
		link(title_link, title, FALSE),
		title ? "<br /><br />" : "",
		
		// main text
		text ? formatted_text : "",

		// fields
		!strcmp(fields, "") ? "<br />" : "",
		fields,

		// bottom border
		border->str,

		// footer
		(footer) ? footer : "",
		(footer) ? "<br />" : "",
		(ts != (time_t) (0))  ? ctime(&ts) : ""
		
	);

	// Free up the GStrings that we don't need anymore.
	g_string_free(border, TRUE);
	//g_string_free(truncated_text, TRUE);

	return g_string_free(html, FALSE);
}

void slack_message(SlackAccount *sa, json_value *json) {
	const char *channel_id = json_get_prop_strptr(json, "channel");

	handle_message(sa, slack_object_hash_table_lookup(sa->channels, channel_id)
			?: slack_object_hash_table_lookup(sa->ims,      channel_id),
			json, PURPLE_MESSAGE_RECV);
}

void slack_user_typing(SlackAccount *sa, json_value *json) {
	const char *user_id    = json_get_prop_strptr(json, "user");
	const char *channel_id = json_get_prop_strptr(json, "channel");

	SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, user_id);
	SlackChannel *chan;
	if (user && slack_object_id_is(user->im, channel_id)) {
		/* IM */
		serv_got_typing(sa->gc, user->name, 3, PURPLE_TYPING);
	} else if ((chan = (SlackChannel*)slack_object_hash_table_lookup(sa->channels, channel_id))) {
		/* Channel */
		/* TODO: purple_conv_chat_user_set_flags (though nothing seems to use this) */
	} else {
		purple_debug_warning("slack", "Unhandled typing: %s@%s\n", user_id, channel_id);
	}
}

unsigned int slack_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state) {
	SlackAccount *sa = gc->proto_data;

	if (state != PURPLE_TYPING)
		return 0;

	SlackUser *user = g_hash_table_lookup(sa->user_names, who);
	if (!user || !*user->im)
		return 0;

	GString *channel = append_json_string(g_string_new(NULL), user->im);
	slack_rtm_send(sa, NULL, NULL, "typing", "channel", channel->str, NULL);
	g_string_free(channel, TRUE);

	return 3;
}

static void get_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	SlackObject *obj = data;
	json_value *list = json_get_prop_type(json, "messages", array);

	if (!list || error) {
		purple_debug_error("slack", "Error loading channel history: %s\n", error ?: "missing");
		g_object_unref(obj);
		return;
	}

	/* what order are these in? */
	for (unsigned i = list->u.array.length; i; i --) {
		json_value *msg = list->u.array.values[i-1];
		if (g_strcmp0(json_get_prop_strptr(msg, "type"), "message"))
			continue;

		handle_message(sa, obj, msg, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_DELAYED | PURPLE_MESSAGE_NO_LOG);
	}
	/* TODO: has_more? */

	g_object_unref(obj);
}

void slack_get_history(SlackAccount *sa, SlackObject *obj, const char *since, unsigned count) {
	if (SLACK_IS_CHANNEL(obj)) {
		SlackChannel *chan = (SlackChannel*)obj;
		if (!chan->cid)
			slack_chat_open(sa, chan);
	}

	char count_buf[6] = "";
	snprintf(count_buf, 5, "%u", count);
	slack_api_channel_call(sa, get_history_cb, g_object_ref(obj), obj, "history", "oldest", since ?: "0", "count", count_buf, NULL);
}

SlackObject *slack_conversation_get_channel(SlackAccount *sa, PurpleConversation *conv) {
	switch (conv->type) {
		case PURPLE_CONV_TYPE_IM:
			return g_hash_table_lookup(sa->user_names, purple_conversation_get_name(conv));
		case PURPLE_CONV_TYPE_CHAT:
			return g_hash_table_lookup(sa->channel_cids, GUINT_TO_POINTER(purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv))));
		default:
			return NULL;
	}
}

void slack_mark_conversation(SlackAccount *sa, PurpleConversation *conv) {
	int c = GPOINTER_TO_INT(purple_conversation_get_data(conv, "unseen-count"));
	if (c != 0)
		/* we could update read count to farther back, but best to only move it forward to latest */
		return;

	char *ts = purple_conversation_get_data(conv, "slack:ts");
	if (!ts)
		return;
	/* we don't need ts anymore */
	purple_conversation_set_data(conv, "slack:ts", NULL);

	SlackObject *obj = slack_conversation_get_channel(sa, conv);
	slack_api_channel_call(sa, NULL, NULL, obj, "mark", "ts", ts, NULL);
	g_free(ts);
}
