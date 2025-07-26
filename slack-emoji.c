// SPDX-License-Identifier: GPL-2.0-or-later

#include <gio/gio.h>
#include <glib.h>
#include <stdio.h>

#include "slack-emoji.h"
#include "slack-json.h"

static void slack_emoji_load_json(SlackEmojiContext *ctx, json_value *emoji, gboolean skins, GString *unicode_re);

static void slack_emoji_load_json_one(SlackEmojiContext *ctx, json_value *emoji, gboolean skins, GString *unicode_re)
{
	gchar *slackcode;
	gchar *unicode;

	if (emoji->type != json_object) {
		return;
	}

	slackcode = json_get_prop_strptr(emoji, "name");
	unicode = json_get_prop_strptr(emoji, "unicode");

	g_hash_table_insert(ctx->slackcode_to_unicode,
			    g_strdup(slackcode),
			    g_strdup(unicode));
	g_hash_table_insert(ctx->unicode_to_slackcode,
			    g_strdup(unicode),
			    g_strdup(slackcode));

	if (unicode_re->len != 1) {
		g_string_append(unicode_re, "|");
	}
	while (*unicode) {
		gunichar c = g_utf8_get_char(unicode);
		g_string_append_printf(unicode_re, "\\N{U+%04x}", c);
		unicode = g_utf8_next_char(unicode);
	}

	if (!skins) {
		json_value *skins = json_get_prop(emoji, "skinVariations");
		if (skins) {
			slack_emoji_load_json(ctx, skins, TRUE, unicode_re);
		}
	}
}

static void slack_emoji_load_json(SlackEmojiContext *ctx, json_value *emoji, gboolean skins, GString *unicode_re)
{
	gsize i;
	if (emoji->type != json_object) {
		return;
	}

	for (i = 0; i < emoji->u.object.length; i++) {
		slack_emoji_load_json_one(ctx,
					  emoji->u.object.values[i].value,
					  skins,
					  unicode_re);
	}
}

SlackEmojiContext *slack_emoji_new(void)
{
	GError *err = NULL;
	GBytes *emojidata;
	gsize emojilen;
	gconstpointer emojibytes;
	json_value *emoji;
	SlackEmojiContext *ctx;
	GString *unicode_re_match = NULL;

	ctx = g_new0(SlackEmojiContext, 1);
	ctx->slackcode_to_unicode = g_hash_table_new_full(g_str_hash,
							  g_str_equal,
							  g_free,
							  g_free);
	ctx->unicode_to_slackcode = g_hash_table_new_full(g_str_hash,
							  g_str_equal,
							  g_free,
							  g_free);

	emojidata = g_resources_lookup_data("/im/pidgin/plugins/slack-libpurple/weemoji.json",
					    G_RESOURCE_LOOKUP_FLAGS_NONE,
					    &err);
	if (err) {
		g_warning("Unable to load weemoji.json resource: %s", err->message);
		g_error_free(err);
		goto error;
	}

	emojibytes = g_bytes_get_data(emojidata, &emojilen);
	emoji = json_parse(emojibytes, emojilen);
	g_bytes_unref(emojidata);


	unicode_re_match = g_string_new("(");
	slack_emoji_load_json(ctx, emoji, FALSE, unicode_re_match);
	g_string_append(unicode_re_match, ")");


	ctx->slackcode_re = g_regex_new(":([a-z0-9_+-]+):",
					G_REGEX_DEFAULT,
					G_REGEX_MATCH_DEFAULT,
					&err);
	if (err) {
		g_warning("Unable to compile emoji unicode regex: %s", err->message);
		g_error_free(err);
		goto error;
	}

	ctx->unicode_re = g_regex_new(unicode_re_match->str,
				      G_REGEX_DEFAULT,
				      G_REGEX_MATCH_DEFAULT,
				      &err);
	if (err) {
		fprintf(stderr, "Unable to compile emoji unicode regex: %s", err->message);
		g_error_free(err);
		goto error;
	}

	json_value_free(emoji);
	g_string_free(unicode_re_match, TRUE);

	return ctx;

 error:
	g_string_free(unicode_re_match, TRUE);
	slack_emoji_free(ctx);
	return NULL;
}

void slack_emoji_free(SlackEmojiContext *ctx)
{
	if (!ctx)
		return;
	if (ctx->slackcode_to_unicode)
		g_hash_table_destroy(ctx->slackcode_to_unicode);
	if (ctx->unicode_to_slackcode)
		g_hash_table_destroy(ctx->unicode_to_slackcode);
	if (ctx->slackcode_re)
		g_regex_unref(ctx->slackcode_re);
	if (ctx->unicode_re)
		g_regex_unref(ctx->unicode_re);
	g_free(ctx);
}

static gboolean slack_emoji_replace(const GMatchInfo *info,
				    GString *res,
				    GHashTable *mapping,
				    gboolean slackcode)
{
	gchar *match;
	gchar *r;

	match = g_match_info_fetch(info, 1);
	r = g_hash_table_lookup(mapping, match);

	if (r != NULL) {
		if (!slackcode)
			g_string_append(res, ":");
		g_string_append(res, r);
		if (!slackcode)
			g_string_append(res, ":");
	} else {
		if (slackcode)
			g_string_append(res, ":");
		g_string_append(res, match);
		if (slackcode)
			g_string_append(res, ":");
	}
	g_free(match);

	return FALSE;
}

static gboolean slack_emoji_replace_slackcode(const GMatchInfo *info,
					      GString *res,
					      gpointer data)
{
	return slack_emoji_replace(info, res, data, TRUE);
}

static gboolean slack_emoji_replace_unicode(const GMatchInfo *info,
					    GString *res,
					    gpointer data)
{
	return slack_emoji_replace(info, res, data, FALSE);
}

char *slack_emoji_slackcode_to_unicode(SlackEmojiContext *ctx, const char *msg)
{
	return g_regex_replace_eval(ctx->slackcode_re,
				    msg,
				    -1,
				    0,
				    0,
				    slack_emoji_replace_slackcode,
				    ctx->slackcode_to_unicode,
				    NULL);
}

char *slack_emoji_unicode_to_slackcode(SlackEmojiContext *ctx, const char *msg)
{
	return g_regex_replace_eval(ctx->unicode_re,
				    msg,
				    -1,
				    0,
				    0,
				    slack_emoji_replace_unicode,
				    ctx->unicode_to_slackcode,
				    NULL);
}
