#ifndef _PURPLE_SLACK_EMOJI_H
#define _PURPLE_SLACK_EMOJI_H
#include "json.h"

int get_unicode_from_emoji_short(const char *short_name);
void replace_emoji_short_names(GString *html, gchar * s);

#endif // _PURPLE_SLACK_EMOJI_H