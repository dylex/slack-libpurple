#ifndef _PURPLE_SLACK_JSON_H
#define _PURPLE_SLACK_JSON_H

#include <glib.h>
#include "json.h"

#define json_get_selector(JSON, TYPE, SELECTOR, DEFAULT) ({ \
		__typeof__(JSON) _val = (JSON); \
		_val && _val->type == json_##TYPE ? SELECTOR : DEFAULT; \
	})
#define json_get_type(JSON, TYPE) \
	json_get_selector(JSON, TYPE, _val, NULL)
#define json_get_strptr(JSON) \
	json_get_selector(JSON, string, _val->u.string.ptr, NULL)
#define json_get_val(JSON, TYPE, DEF) \
	json_get_selector(JSON, TYPE, _val->u.TYPE, DEF)
#define json_get_boolean(JSON, DEF) \
	json_get_val(JSON, boolean, DEF)

json_value *json_get_prop(json_value *val, const char *prop) __attribute__((pure));

#define json_get_prop_type(JSON, PROP, TYPE) \
	json_get_type(json_get_prop(JSON, PROP), TYPE)
#define json_get_prop_strptr(JSON, PROP) \
	json_get_strptr(json_get_prop(JSON, PROP))
#define json_get_prop_val(JSON, PROP, TYPE, DEF) \
	json_get_val(json_get_prop(JSON, PROP), TYPE, DEF)
#define json_get_prop_boolean(JSON, PROP, DEF) \
	json_get_boolean(json_get_prop(JSON, PROP), DEF)

/* Add an escaped, quoted json string to a GString */
GString *append_json_string(GString *str, const char *s);

time_t slack_parse_time(json_value *val);

#endif
