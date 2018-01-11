#ifndef _PURPLE_SLACK_MESSAGE_H
#define _PURPLE_SLACK_MESSAGE_H

#include "json.h"
#include "slack.h"
#include "slack-object.h"

gchar *slack_html_to_message(SlackAccount *sa, const char *s, PurpleMessageFlags flags);
void add_slack_attachments_to_buffer(GString *buffer, SlackAccount *sa, json_value *attachments, PurpleMessageFlags *flags);
gchar *slack_json_to_html(SlackAccount *sa, json_value *json, const char *subtype, PurpleMessageFlags *flags);
gchar *slack_message_to_html(SlackAccount *sa, gchar *s, const char *subtype, PurpleMessageFlags *flags);
gchar *slack_attachment_to_html(SlackAccount *sa, json_value *attachment, PurpleMessageFlags *flags);
gchar *get_color(char *c);
gchar *link(char *url, char *text, int insertBR);
SlackObject *slack_conversation_get_channel(SlackAccount *sa, PurpleConversation *conv);
void slack_get_history(SlackAccount *sa, SlackObject *obj, const char *since, unsigned count);
void slack_mark_conversation(SlackAccount *sa, PurpleConversation *conv);

/* RTM event handlers */
void slack_message(SlackAccount *sa, json_value *json);
void slack_user_typing(SlackAccount *sa, json_value *json);

/* Purple protocol handlers */
unsigned int slack_send_typing(PurpleConnection *gc, const char *who, PurpleTypingState state);
void debug(char *s);

#endif // _PURPLE_SLACK_MESSAGE_H
