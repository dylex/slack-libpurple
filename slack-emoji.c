#include <debug.h>

#include "slack-json.h"

#include "slack-emoji.h"
#include "slack-util.h"

json_value * emoji_json_data = NULL;

void load_emoji_data(const gchar* file_name) {
    char * file_contents;
    int file_size;
    FILE *fp;

    json_char* json;
    json_value* value;


    fp = fopen(file_name, "r");

    if (fp == NULL) {
        purple_debug_error("slack", "Could not open emoji file:  %s\n", file_name);
        fclose(fp);
        return;
    }
    ////Find the files size
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    rewind(fp);
    purple_debug_info("slack", "Emoji Data File size: %d\n", file_size);
    //Allocate the memory to contain the whole file
    // Ideally this should happen only once on program load.
    file_contents = (char*)malloc(file_size);

    if (fread(file_contents, file_size, 1, fp) != 1) {
        purple_debug_error("slack", "Could not read emoji file:  %s\n", file_name);
        fclose(fp);
        free(file_contents);
        return;
    }
    json = (json_char*)file_contents;
    emoji_json_data = json_parse(json, file_size);
    free(file_contents);
    fclose(fp);

    if (emoji_json_data == NULL) {
        purple_debug_error("slack", "Could not parse emoji file:  %s\n", file_name);
        free(file_contents);
        json_value_free(emoji_json_data);
        return;
    }
    //Value should be an object
    if (emoji_json_data->type != json_object) {
        purple_debug_error("slack", "Emoji file is not an object:  %s\n", file_name);
        json_value_free(emoji_json_data);
        return;
    }
}

void unloaded_emoji_data() {
    json_value_free(emoji_json_data);
}

int get_unicode_from_emoji_short(const char *short_name ) {
    load_emoji_data("simple_emoji.json");
    json_value * emoji = json_get_prop(emoji_json_data, short_name);

    if (emoji == NULL || emoji->type != json_object) {
        purple_debug_error("slack", "Could not find emoji:  %s\n", short_name);
        return 1;
    }
    char * unicode_value = json_get_prop_strptr(emoji, "unified");
    purple_debug_info("slack", "Unicode value: %s\n", unicode_value);
    //Convert the string to hex
    int unicode_point = strtol(unicode_value, NULL, 16);

    unload_emoji_data();
    return unicode_point;

}

void replace_emoji_short_names(GString *html, gchar* s) {
    GRegex * regex = NULL;
	GMatchInfo * match_info = NULL;


    //This regular expressions purpose is to find all emoji short names in the format :emoji-name:
    //The short name is then replaced with the found unicode value.
	regex = g_regex_new(":(?:[^:\\s]|::)*:", 0, 0, NULL);
	int result = g_regex_match(regex, s, 0, &match_info);
	purple_debug_info("slack", "Result: %d", result);

	while (g_match_info_matches(match_info)) {
		gchar *match = g_match_info_fetch(match_info, 0);
		//Get characters after the first and before the last
		char * emoji_name = g_strndup(match+1, strlen(match)-2);
		purple_debug_info("slack", "Emoji name: %s\n", emoji_name);
        //Later it would be usefull to have some function that determines whether the emoji is unicode or not.
        //If its  not this is where the image replacement functionality could come in.

		int unicode_emoji = get_unicode_from_emoji_short(emoji_name);
		//Take unicode point and convert to utf8
		GString * emoji_string = g_string_new("");
		g_string_append_unichar(emoji_string, unicode_emoji);
		g_string_replace_bp(html, match, emoji_string->str);
        //Remove the allocated string for the match text.
		g_free(match);
        //Go to the next available match if there isnt one the match_info will become NULL.
		g_match_info_next(match_info, NULL);
	}
}
