
#include "slack-emoji.h"

typedef struct {
	const char *scenario;
	const char *slackcode;
	const char *unicode;
} TestData;

static const TestData data[] = {
	{
		"/emoji/exact-match",
		":mountain_cableway:",
		"🚠"
	},
	{
		"/emoji/leading-match",
		":floppy_disk: is the save icon",
		"💾 is the save icon"
	},
	{
		"/emoji/trailing-match",
		"kick :football:",
		"kick 🏈"
	},
	{
		"/emoji/middle-match",
		"a :zombie: eats your branes",
		"a 🧟 eats your branes",
	},
	{
		"/emoji/many-matches",
		":cloud: :arrow_right: :snowflake: :arrow_right: :snowman: :arrow_right: :sunny: :arrow_right: :smiley: :arrow_right: :umbrella: :arrow_right: :cry: :arrow_right: :skull_and_crossbones:",
		"☁️ ➡️ ❄️ ➡️ ☃️ ➡️ ☀️ ➡️ 😃 ➡️ ☂️ ➡️ 😢 ➡️ ☠️",
	},
	{
		"/emoji/joined-matches",
		":baby_bottle::baby_chick:",
		"🍼🐤",
	}
};

static void test_emoji_roundtrip(gconstpointer opaque)
{
	const TestData *data = opaque;
	SlackEmojiContext *emoji = slack_emoji_new();

	char *unicode = slack_emoji_slackcode_to_unicode(emoji, data->slackcode);
	char *slackcode = slack_emoji_unicode_to_slackcode(emoji, data->unicode);

	g_assert_cmpstr(unicode, ==, data->unicode);
	g_assert_cmpstr(slackcode, ==, data->slackcode);

	slack_emoji_free(emoji);
}

int main(int argc, char **argv) {
	gsize i;
	g_test_init(&argc, &argv);

	for (i = 0; i < G_N_ELEMENTS(data); i++) {
		g_test_add_data_func(data[i].scenario,
				     &data[i],
				     test_emoji_roundtrip);
	}

	g_test_run();
}
