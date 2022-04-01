#include <debug.h>

#include "slack-json.h"

#include "slack-emoji.h"
#include "slack-util.h"

#include <stdio.h>


int get_unicode_from_emoji_short(const char *short_name ) {
    //Open the json file containing the emoji list
    const char * file_name = "simple_emoji.json";
    char * file_contents;
    int file_size;
    FILE *fp;

    json_char* json;
    json_value* value;


    fp = fopen(file_name, "r");

    if (fp == NULL) {
        purple_debug_error("slack", "Could not open emoji file:  %s\n", file_name);
        //Replace with purple debug later
        fclose(fp);
        return;
    }
    ////Find the files size
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    rewind(fp);
    purple_debug_info("slack", "Emoji Data File size: %d\n", file_size);
    ////Allocate the memory to contain the whole file
    file_contents = (char*)malloc(file_size);
    //file_contents[file_size] = "\0";

    if (fread(file_contents, file_size, 1, fp) != 1) {
        purple_debug_error("slack", "Could not read emoji file:  %s\n", file_name);
        //Replace with purple debug later
        //fprintf(stderr, "Error reading simple_emoji.json\n");
        fclose(fp);
        free(file_contents);
        return;
    }
    json = (json_char*)file_contents;
    value = json_parse(json, file_size);

    if (value == NULL) {
        purple_debug_error("slack", "Could not parse emoji file:  %s\n", file_name);
        //Replace with purple debug later
        fclose(fp);
        free(file_contents);
        return;
    }
    //Value should be an object
    if (value->type != json_object) {
        purple_debug_error("slack", "Emoji file is not an object:  %s\n", file_name);
        //Replace with purple debug later
        fclose(fp);
        free(file_contents);
        return;
    }

    json_value * emoji = json_get_prop(value, short_name);
    if (emoji == NULL || emoji->type != json_object) {
        purple_debug_error("slack", "Could not find emoji:  %s\n", short_name);
        //Replace with purple debug later
        fclose(fp);
        free(file_contents);
        return;
    }
    char * unicode_value = json_get_prop_strptr(emoji, "unified");
    purple_debug_info("slack", "Unicode value: %s\n", unicode_value);
    //Convert the string to hex
    int unicode_point = strtol(unicode_value, NULL, 16);

    json_value_free(value);
    return unicode_point;

}

void replace_emoji_short_names(GString *html, gchar* s) {
    GRegex * regex = NULL;
	GMatchInfo * match_info = NULL;
	regex = g_regex_new(":(?:[^:\\s]|::)*:", 0, 0, NULL);
	int result = g_regex_match(regex, s, 0, &match_info);
	purple_debug_info("slack", "Result: %d", result);

	while (g_match_info_matches(match_info)) {
		gchar *match = g_match_info_fetch(match_info, 0);
		//Get characters after the first and before the last
		char * emoji_name = g_strndup(match+1, strlen(match)-2);
		purple_debug_info("slack", "Emoji name: %s\n", emoji_name);
		int wc = get_unicode_from_emoji_short(emoji_name);
		//Take wc and convert to utf8
		GString * emoji_string = g_string_new("");
		g_string_append_unichar(emoji_string, wc);
		g_string_replace_bp(html, match, emoji_string->str);

		g_free(match);
		//g_string_append_unichar(html, wc);
		g_match_info_next(match_info, NULL);
	}
}