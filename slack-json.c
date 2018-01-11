#include <string.h>

#include "slack-json.h"

json_value *json_get_prop(json_value *val, const char *index) {
	if (!val || val->type != json_object) {
		return NULL;
	}

	for (unsigned int i = 0; i < val->u.object.length; ++ i) {
		if (!strcmp (val->u.object.values[i].name, index)) {
			return val->u.object.values[i].value;
		}
	}

	return NULL;
}

GString *append_json_string(GString *str, const char *s) {
	g_string_append_c(str, '"');
	const char *p = s;
	char c;
	for (;;) {
		switch ((c = *p)) {
			case '\0':
			case '"':
			case '\\': break;
			case '\b': c = 'b'; break;
			case '\f': c = 'f'; break;
			case '\n': c = 'n'; break;
			case '\r': c = 'r'; break;
			case '\t': c = 't'; break;
			default:
				p++;
				continue;
		}

		g_string_append_len(str, s, p-s);
		if (!c)
			break;
		g_string_append_c(str, '\\');
		g_string_append_c(str, c);
		s = ++p;
	}

	return g_string_append_c(str, '"');
}

time_t slack_parse_time(json_value *val) {
	if (!val)
		return 0;
	if (val->type == json_integer)
		return val->u.integer;
	if (val->type == json_double)
		return val->u.dbl;
	if (val->type == json_string)
		/* "EPOCH.0000ID", atol is sufficient */
		return atol(val->u.string.ptr);
	return 0;
}
