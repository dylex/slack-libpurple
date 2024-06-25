#include "slack-api.h"
#include "slack-channel.h"
#include "slack-conversation.h"
#include "slack-json.h"
#include "slack-message.h"
#include "slack-thread.h"

#include <debug.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

/**
 * Returns a deterministic color for a thread.
 *
 * Returned string must be g_string_free'd.
 *
 * @param ts "thread_ts" string value from Slack.
 */
static void slack_get_thread_color(char s[8], const char *ts) {
	// Produce a color that works against white background by the following
	// algorithm:
	//
	// 1. Pick a pseudo-random number by seeding it with ts, IOW it is
	// deterministic.
	//
	// 2. Pick a 24-bit RGB value, but throw away the the original highest
	// bit of each byte, resulting in a range from 000000 to 7f7f7f.
	//
	// 3. Pick exactly one base color that will receive its highest bit, or
	// possibly none of them.
	//
	// This gives quite a random color, but often with a dominant RGB
	// component, never white, and deterministic from ts.

	guint r = g_str_hash(ts);

	// Pick random RGB color.
	guint color = r & 0x7f7f7f;

	// Pick random RGB high bit by shifting 0x800000 down by 0 (B), 8 (G),
	// 16 (R), or 24 (0). 0x3000000 are the first bits of 'r' we didn't use
	// yet.
	guint pref_color = (0x800000 >> ((r & 0x3000000) >> 21));
	color |= pref_color;

	snprintf(s, 7, "%06x", color);
}

#ifdef _WIN32
static struct tm *localtime_r(time_t *_clock, struct tm *_result)
{
	struct tm *p = localtime(_clock);

	if (p)
		*(_result) = *p;

	return p;
}
#endif

static void slack_format_thread_time(SlackAccount *sa, char s[128], const char *ts, gboolean exact) {
	char *te;
	time_t tt = strtoul(ts, &te, 10); // slack_parse_time_str(ts)
	if (!tt) {
		strncpy(s, ts, 127);
		return;
	}
	time_t now = time(NULL);
	struct tm now_time, thread_time;
	localtime_r(&tt, &thread_time);
	localtime_r(&now, &now_time);

	const char *time_fmt;
	if (thread_time.tm_yday == now_time.tm_yday && thread_time.tm_year == now_time.tm_year)
		time_fmt = purple_account_get_string(sa->account, "thread_timestamp", "%X");
	else
		time_fmt = purple_account_get_string(sa->account, "thread_datestamp", "%x %X");

	size_t r = strftime(s, 128, time_fmt, &thread_time);
	if (!r) {
		/* fall back */
		r = snprintf(s, 128, "%ld", tt);
	}

	if (exact)
		strncpy(&s[r], te, 127-r);
}

static gboolean slack_parse_thread_time(SlackAccount *sa, const char *s, char start[20], char end[20], const char **rest) {
	time_t t = 0;
	char *e = NULL;

	if (rest)
		*rest = NULL;

	/* first try posix seconds */
	if (s[0] >= '0' && s[0] <= '9') {
		t = strtoul(s, &e, 10);
		if (t && e - s >= 10)
			goto sub;
	}

#ifndef _WIN32
	char *su = NULL;
	const char *formats[] = {
		purple_account_get_string(sa->account, "thread_datestamp", "%x %X"),
		purple_account_get_string(sa->account, "thread_timestamp", "%X"),
		NULL };
	const char **fmt;
	time(&t);
	for (fmt = formats; *fmt; fmt++) {
		struct tm tm;
		localtime_r(&t, &tm);
		e = strptime(s, *fmt, &tm);
		if (!e && su)
			e = strptime(su, *fmt, &tm);
		if (e) {
			t = mktime(&tm);
			break;
		}
	}
	g_free(su);
#endif

sub:
	if (!e)
		return FALSE;
	if (*e == '.') {
		e++;
		int i = 0;
		while (e[i] >= '0' && e[i] <= '9')
			i ++;
		if (i == 6) {
			snprintf(start, 19, "%lu.%s", t, e);
			*end = 0;
			e += i;
		}
	}
	else
	{
		snprintf(start, 19, "%lu.000000", t);
		snprintf(end, 19, "%lu.999999", t);
	}

	if (*e && !isspace(*e))
		// Don't accept other strings right next to the timestamp
		// (require at least one space).
		return FALSE;

	if (rest) {
		while (isspace(*e))
			e++;
		*rest = e;
	}
	return TRUE;
}

void slack_append_formatted_thread_timestamp(SlackAccount *sa, GString *str, const char *ts, gboolean exact) {
	char color[8] = "";
	slack_get_thread_color(color, ts);

	char time_str[128] = "";
	slack_format_thread_time(sa, time_str, ts, exact);

	g_string_append(str, "<font color=\"#");
	g_string_append(str, color);
	g_string_append(str, "\">");
	g_string_append(str, time_str);
	g_string_append(str, "</font>");
}

typedef void slack_thread_lookup_ts_cb(SlackAccount *sa, SlackObject *conv, gpointer data, const char *thread_ts, const char *rest);

struct thread_lookup_ts {
	SlackObject *conv;
	slack_thread_lookup_ts_cb *cb;
	void *data;
	char *timestr;
	char *rest;
};

static gboolean slack_thread_lookup_ts_history_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct thread_lookup_ts *lookup = data;
	const char *ts = NULL;

	json_value *list = json_get_prop_type(json, "messages", array);
	if (!list || error) {
		purple_debug_error("slack", "Error querying threads: %s\n", error ?: "missing");
	}
	else if (list->u.array.length <= 0) {
		slack_write_message(sa, lookup->conv, "Thread not found. If the thread start date is not today, make sure you specify the date in the thread timestamp.", PURPLE_MESSAGE_SYSTEM);

	}
	else if (list->u.array.length > 1) {
		GString *errmsg = g_string_new("Thread timestamp is ambiguous. Please use one of the following unambiguous thread IDs:<br>");
		for (unsigned i = 0; i < list->u.array.length; i++) {
			json_value *entry = list->u.array.values[i];
			/* matching slack_json_to_html */
			const char *ts = json_get_prop_strptr(entry, "ts");
			g_string_append(errmsg, purple_account_get_string(sa->account, "parent_indicator", "◈ "));
			if (ts)
				slack_append_formatted_thread_timestamp(sa, errmsg, ts, TRUE);
			g_string_append(errmsg, ": ");
			slack_message_to_html(errmsg, sa, json_get_prop_strptr(entry, "text"), 0, NULL);
			g_string_append(errmsg, "<br>");
		}
		slack_write_message(sa, lookup->conv, errmsg->str, PURPLE_MESSAGE_SYSTEM);
		g_string_free(errmsg, TRUE);
	}
	else {
		json_value *entry = list->u.array.values[0];
		ts = json_get_prop_strptr(entry, "ts");
	}

	if (ts != NULL) {
		g_free(lookup->conv->last_thread_timestr);
		g_free(lookup->conv->last_thread_ts);
		lookup->conv->last_thread_timestr = lookup->timestr;
		lookup->timestr = NULL; // Take ownership, avoid strdup.
		lookup->conv->last_thread_ts = g_strdup(ts);
	}

	lookup->cb(sa, lookup->conv, lookup->data, ts, lookup->rest);
	g_object_unref(lookup->conv);
	g_free(lookup->timestr);
	g_free(lookup->rest);
	g_free(lookup);
	return FALSE;
}

static void slack_thread_lookup_ts(SlackAccount *sa, slack_thread_lookup_ts_cb *cb, SlackObject *conv, gpointer data, const char *timestr) {
	char start[20] = "";
	char end[20] = "";
	const char *rest;
	if (!slack_parse_thread_time(sa, timestr, start, end, &rest)) {
		slack_write_message(sa, conv, "Could not parse thread timestamp.", PURPLE_MESSAGE_SYSTEM);
		return cb(sa, conv, data, NULL, rest);
	}
	if (!*end)
		return cb(sa, conv, data, start, rest);

	if (conv->last_thread_timestr && conv->last_thread_ts && strncmp(timestr, conv->last_thread_timestr, rest - timestr) == 0)
		return cb(sa, conv, data, conv->last_thread_ts, rest);

	struct thread_lookup_ts *lookup = g_new(struct thread_lookup_ts, 1);
	lookup->conv = g_object_ref(conv);
	lookup->cb = cb;
	lookup->data = data;
	lookup->timestr = g_strndup(timestr, rest - timestr);
	lookup->rest = g_strdup(rest);

	const char *id = slack_conversation_id(conv);
	slack_api_post(sa, slack_thread_lookup_ts_history_cb, lookup, "conversations.history", "channel", id, "oldest", start, "latest", end, "inclusive", "1", NULL);
}

static void slack_thread_post_lookup_cb(SlackAccount *sa, SlackObject *conv, gpointer data, const char *thread_ts, const char *msg) {
	if (!msg || !*msg) {
		slack_write_message(sa, conv, "Please supply a message.", PURPLE_MESSAGE_SYSTEM);
		return;
	}

	if (thread_ts) {
		int r = slack_conversation_send(sa, conv, msg, 0, thread_ts);
		if (r < 0)
			purple_debug_error("slack", "Not able to send message \"%s\": %s\n", msg, strerror(-r));
	}
}

void slack_thread_post_to_timestamp(SlackAccount *sa, SlackObject *obj, const char *timestr_and_msg) {
	slack_thread_lookup_ts(sa, slack_thread_post_lookup_cb, obj, NULL, timestr_and_msg);
}

static void slack_thread_get_replies_lookup_cb(SlackAccount *sa, SlackObject *conv, gpointer data, const char *thread_ts, const char *rest) {
	if (rest && *rest) {
		slack_write_message(sa, conv, "Too many arguments.", PURPLE_MESSAGE_SYSTEM);
		return;
	}

	if (thread_ts)
		slack_get_history(sa, conv, NULL, SLACK_HISTORY_LIMIT_COUNT, thread_ts, TRUE);
}

void slack_thread_get_replies(SlackAccount *sa, SlackObject *obj, const char *timestr) {
	slack_thread_lookup_ts(sa, slack_thread_get_replies_lookup_cb, obj, NULL, timestr);
}
