#ifndef _PURPLE_SLACK_H
#define _PURPLE_SLACK_H

#include <string.h>

#include <account.h>

#include "glibcompat.h"
#include "purple-websocket.h"
#include "slack-object.h"

#define SLACK_PLUGIN_ID "prpl-slack"

#define MARK_LIST_END ((SlackObject *)1)

typedef struct _SlackAccount {
	PurpleAccount *account;
	PurpleConnection *gc;
	char *email;
	char *host;
	char *api_url; /* e.g., "https://slack.com/api" */
	char *token; /* url encoded */
	char *d_cookie;

	short login_step;
	GQueue api_calls; /* SlackAPICall */
	PurpleWebsocket *rtm;
	guint rtm_id;
	GHashTable *rtm_call; /* unsigned rtm_id -> SlackRTMCall */
	guint ping_timer;

	struct _SlackTeam {
		char *id;
		char *name;
		char *domain;
	} team;
	struct _SlackUser *self;

	GHashTable *users; /* slack_object_id user_id -> SlackUser (ref) */
	GHashTable *user_names; /* char *user_name -> SlackUser (no ref) */
	GHashTable *ims; /* slack_object_id im_id -> SlackUser (no ref) */

	GHashTable *channels; /* slack_object_id channel_id -> SlackChannel (ref) */
	GHashTable *channel_names; /* char *chan_name -> SlackChannel (no ref) */
	int cid;
	GHashTable *channel_cids; /* int purple_chat_id -> SlackChannel (no ref) */

	PurpleGroup *blist; /* default group for ims/channels */
	GHashTable *buddies; /* char *slack_id -> PurpleBListNode */
	gboolean roomlist_stop;

	guint mark_timer;
	SlackObject *mark_list;

	GQueue avatar_queue; /* SlackUser * queue for avatar downloads */

	gboolean away;
} SlackAccount;

void slack_login_step(SlackAccount *sa);
GHashTable *slack_chat_info_defaults(PurpleConnection *gc, const char *name);

static inline SlackAccount *get_slack_account(PurpleAccount *account) {
	if (!account || !account->gc || strcmp(account->protocol_id, SLACK_PLUGIN_ID))
		return NULL;
	return account->gc->proto_data;
}

#endif // _PURPLE_SLACK_H
