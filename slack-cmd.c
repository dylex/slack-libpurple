#include <cmds.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-message.h"

/* really all commands are handled server-side, but OPT_PROTO_SLACK_COMMANDS_NATIVE doesn't quite work right (when the same command is registered for other things), so we defensively register a trivial handler for at least all the builtin commands.
 * copied from https://get.slack.help/hc/en-us/articles/201259356-using-slash-commands */
static const char *slack_cmds[] = {
	"me [your text]:  Display italicized action text, e.g. \"/me does a dance\" will display as \"does a dance\"",
	"msg @someone [your message]:  Send a private direct message to another member",
	"dm @someone [your message]:  Send a private direct message to another member",
	"shrug [your message]:  Appends ¯\\_(&#x30c4;)_/¯ to the end of your message",
	"archive:  Archive the current channel",
	"collapse:  Collapse all inline images and video in the current channel (opposite of /expand)",
	"expand:  Expand all inline images and video in the current channel (opposite of /collapse)",
	"invite @someone [#channel]:  Invite a member to a channel",
	"join [#channel]:  Open a channel and become a member",
	"kick @someone:  Remove a member from the current channel. This action may be restricted to Workspace Owners or Admins",
	"remove @someone:  Remove a member from the current channel. This action may be restricted to Workspace Owners or Admins",
	"leave:  Leave a channel",
	"close:  Leave a channel",
	"part:  Leave a channel",
	"away:  Toggle your \"away\" status",
	"mute:  Mute a channel (or unmute a channel that is muted)",
	"open [#channel]:  Open a channel",
	"rename [new name]:  Rename a channel (Admin only)",
	"topic [text]:  Set the channel topic",
	"who:  List members in the current channel",
	"remind [@someone or #channel] to [What] [When]:  Set a reminder a member or a channel",
	"remind help:  Learn more about how to set reminders",
	"remind list:  Get a list of reminders you have set",
	"apps:  Search for Slack apps in the App Directory",
	"search [your text]:  Search Slack messages and files",
	"dnd [some description of time]:  Start or end a Do Not Disturb session",
	"feed help [or subscribe, list, remove]:  Manage RSS subscriptions",
	"feedback [your text]:  Send feedback to Slack",
	"prefs:  Open your preferences",
	"shortcuts:  Open the keyboard shortcuts menu",
	"star:  Star the current channel or conversation",
	NULL
};

static void send_cmd_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	PurpleConversation *conv = data;

	if (error) {
		purple_conversation_write(conv, NULL, error, PURPLE_MESSAGE_ERROR, time(NULL));
		return;
	}

	char *response = json_get_prop_strptr(json, "response");
	if (response) {
		GString *html = g_string_new(NULL);
		PurpleMessageFlags flags = PURPLE_MESSAGE_SYSTEM;
		slack_message_to_html(html, sa, response, &flags);
		purple_conversation_write(conv, NULL, html->str, flags, time(NULL));
		g_string_free(html, TRUE);
	}
}

static PurpleCmdRet send_cmd(PurpleConversation *conv, const gchar *cmd, gchar **args, gchar **error, void *data) {
	SlackAccount *sa = get_slack_account(conv->account);
	if (!sa)
		return PURPLE_CMD_RET_FAILED;

	SlackObject *obj = slack_conversation_get_channel(sa, conv);
	GString *msg = g_string_sized_new(strlen(cmd)+1);
	g_string_append_c(msg, '/');
	g_string_append(msg, cmd);

	/* https://github.com/ErikKalkoken/slackApiDoc/blob/master/chat.command.md */
	slack_api_call(sa, send_cmd_cb, conv, "chat.command", "channel", obj->id, "command", msg->str, "text", args && args[0] ? args[0] : "", NULL);
	g_string_free(msg, TRUE);

	return PURPLE_CMD_RET_OK;
}

void slack_cmd_register() {
	const char **cmdp = slack_cmds;
	char cmdbuf[16] = "";
	while (*cmdp) {
		const char *cmd = *cmdp;
		unsigned i = 0;
		for (i = 0; cmd[i] != ' ' && cmd[i] != ':' && cmd[i] && i < sizeof(cmdbuf)-1; i++)
			cmdbuf[i] = cmd[i];
		cmdbuf[i] = 0;

		purple_cmd_register(cmdbuf, "s", PURPLE_CMD_P_PRPL, PURPLE_CMD_FLAG_CHAT | PURPLE_CMD_FLAG_IM | PURPLE_CMD_FLAG_PRPL_ONLY | PURPLE_CMD_FLAG_ALLOW_WRONG_ARGS,
				SLACK_PLUGIN_ID, send_cmd, cmd, NULL);
		cmdp++;
	}
}
