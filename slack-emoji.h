#ifndef _PURPLE_SLACK_EMOJI_H
#define _PURPLE_SLACK_EMOJI_H

#include <glib.h>

typedef struct SlackEmojiContext {
	GHashTable *slackcode_to_unicode; /* char *slackcode -> char *unicode */
	GHashTable *unicode_to_slackcode; /* char *unicode -> char *slackcode */
	GRegex *slackcode_re;
	GRegex *unicode_re;
} SlackEmojiContext;

SlackEmojiContext *slack_emoji_new();
void slack_emoji_free(SlackEmojiContext *ctx);
char *slack_emoji_slackcode_to_unicode(SlackEmojiContext *ctx, const char *msg);
char *slack_emoji_unicode_to_slackcode(SlackEmojiContext *ctx, const char *msg);

#endif
