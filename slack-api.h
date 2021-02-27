#ifndef _PURPLE_SLACK_API_H
#define _PURPLE_SLACK_API_H

#include "json.h"
#include "slack.h"
#include "slack-object.h"

PurpleConnectionError slack_api_connection_error(const gchar *error);

typedef struct _SlackAPICall SlackAPICall;
typedef gboolean SlackAPICallback(SlackAccount *sa, gpointer user_data, json_value *json, const char *error);

void slack_api_get(SlackAccount *sa, SlackAPICallback *callback, gpointer user_data, const char *endpoint, /* const char *query_param1, const char *query_value1, */ ...) G_GNUC_NULL_TERMINATED;
void slack_api_post(SlackAccount *sa, SlackAPICallback *callback, gpointer user_data, const char *endpoint, /* const char *query_param1, const char *query_value1, */ ...) G_GNUC_NULL_TERMINATED;
void slack_api_disconnect(SlackAccount *sa);

#define SLACK_PAGINATE_LIMIT	"limit", "500"

#endif
