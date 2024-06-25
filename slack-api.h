#ifndef _PURPLE_SLACK_API_H
#define _PURPLE_SLACK_API_H

#include "json.h"
#include "slack.h"
#include "slack-object.h"

PurpleConnectionError slack_api_connection_error(const gchar *error);

typedef struct _SlackAPICall SlackAPICall;
typedef gboolean SlackAPICallback(SlackAccount *sa, gpointer user_data, json_value *json, const char *error);

void slack_api_post(SlackAccount *sa, SlackAPICallback *callback, gpointer user_data, const char *endpoint, /* const char *query_param1, const char *query_value1, */ ...) G_GNUC_NULL_TERMINATED;
void slack_api_disconnect(SlackAccount *sa);

#define SLACK_LIMIT_ARG(COUNT)		"limit", G_STRINGIFY(COUNT)

#define SLACK_PAGINATE_LIMIT_COUNT	500
#define SLACK_PAGINATE_LIMIT_ARG	SLACK_LIMIT_ARG(SLACK_PAGINATE_LIMIT_COUNT)

// Documented as maximum in API (2020-07-20).
#define SLACK_HISTORY_LIMIT_COUNT	1000
#define SLACK_HISTORY_LIMIT_ARG		SLACK_LIMIT_ARG(SLACK_HISTORY_LIMIT_COUNT)

#endif
