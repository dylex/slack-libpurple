#include "slack-auth.h"
#include "slack-api.h"
#include "slack-json.h"

#include "util.h"

static gboolean
slack_auth_login_signin_cb(SlackAccount *sa, gpointer user_data, json_value *json, const char *error) {
	const char *token = json_get_prop_strptr(json, "token");

	if(error != NULL || token == NULL) {
		purple_connection_error_reason(
			purple_account_get_connection(sa->account),
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
			error ? error : "No token provided");

		return FALSE;
	}

	/* set the new token in the slack account */
	g_free(sa->token);
	sa->token = g_strdup(token);

	/* save the token as the password */
	purple_account_set_password(sa->account, sa->token);

	/* now that we've signed in, we need to clear the values we overrode for
	 * authentication and set them to the regular values.
	 */
	g_free(sa->api_url);
	sa->api_url = g_strdup_printf("https://%s/api", sa->host);

	slack_login_step(sa);
	return FALSE;
}

static gboolean
slack_auth_login_finduser_cb(SlackAccount *sa, gpointer user_data, json_value *json, const char *error) {
	const char *user_id = json_get_prop_strptr(json, "user_id");

	if(error != NULL || user_id == NULL) {
		purple_connection_error_reason(
			purple_account_get_connection(sa->account),
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
			error ? error : "User not found");

		return FALSE;
	}

	/* now do the actual login */
	slack_login_step(sa);
	slack_api_get(sa, slack_auth_login_signin_cb,
		NULL, "auth.signin",
		"user", user_id,
		"password", purple_account_get_password(sa->account),
		"team", sa->team.id,
		NULL);
	return FALSE;
}

static gboolean
slack_auth_login_findteam_cb(SlackAccount *sa, gpointer user_data, json_value *json, const char *error) {
	const char *team_id = json_get_prop_strptr(json, "team_id");

	if(error != NULL || team_id == NULL) {
		purple_connection_error_reason(
			purple_account_get_connection(sa->account),
			PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
			error ? error : "Team not found");

		return FALSE;
	}

	sa->team.id = g_strdup(team_id);

	/* now validate that the user exists and get their ID. */
	slack_login_step(sa);
	slack_api_get(sa, slack_auth_login_finduser_cb,
		NULL, "auth.findUser",
		"email", sa->email,
		"team", sa->team.id,
		NULL);
	return FALSE;
}

void
slack_auth_login(SlackAccount *sa) {
	/* validate the team and get it's ID */
	slack_api_get(sa, slack_auth_login_findteam_cb,
		NULL, "auth.findTeam",
		"domain", sa->host,
		NULL);
}
