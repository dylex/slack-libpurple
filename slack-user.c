#include <debug.h>

#include "slack-json.h"
#include "slack-api.h"
#include "slack-blist.h"
#include "slack-thread.h"
#include "slack-user.h"
#include "slack-im.h"

G_DEFINE_TYPE(SlackUser, slack_user, SLACK_TYPE_OBJECT);

static void slack_user_finalize(GObject *gobj) {
	SlackUser *user = SLACK_USER(gobj);

	g_free(user->status);
	g_free(user->avatar_hash);
	g_free(user->avatar_url);

	G_OBJECT_CLASS(slack_user_parent_class)->finalize(gobj);
}

static void slack_user_class_init(SlackUserClass *klass) {
	GObjectClass *gobj = G_OBJECT_CLASS(klass);
	gobj->finalize = slack_user_finalize;
}

static void slack_user_init(SlackUser *self) {
}

SlackUser *slack_user_set(SlackAccount *sa, const char *sid, const char *name) {
	slack_object_id id;
	slack_object_id_set(id, sid);
	g_warn_if_fail(name);

	SlackUser *user = g_hash_table_lookup(sa->users, id);

	if (!user) {
		user = g_object_new(SLACK_TYPE_USER, NULL);
		slack_object_id_copy(user->object.id, id);
		g_hash_table_replace(sa->users, user->object.id, user);
	}

	if (g_strcmp0(user->object.name, name)) {
		purple_debug_misc("slack", "user %s: %s\n", sid, name);

		if (user->object.name)
			g_hash_table_remove(sa->user_names, user->object.name);
		g_free(user->object.name);
		user->object.name = g_strdup(name);
		g_hash_table_insert(sa->user_names, user->object.name, user);
		if (user->object.buddy)
			purple_blist_rename_buddy(user_buddy(user), user->object.name);
	}

	return user;
}

SlackUser *slack_user_update(SlackAccount *sa, json_value *json) {
	const char *sid = json_get_prop_strptr(json, "id");
	if (!sid)
		return NULL;

	SlackUser *user;

	if (json_get_prop_boolean(json, "deleted", FALSE)) {
		user = (SlackUser*)slack_object_hash_table_lookup(sa->users, sid);
		if (!user)
			return NULL;
		if (user->object.name)
			g_hash_table_remove(sa->user_names, user->object.name);
		if (*user->im)
			g_hash_table_remove(sa->ims, user->im);
		slack_object_hash_table_remove(sa->users, sid);
		return NULL;
	}

	user = slack_user_set(sa, sid, json_get_prop_strptr(json, "name"));

	json_value *profile = json_get_prop_type(json, "profile", object);
	if (profile) {
		const char *display = json_get_prop_strptr1(profile, "display_name");
		if (display)
			serv_got_alias(sa->gc, user->object.name, display);

		const char *status = json_get_prop_strptr1(profile, "status_text") ?: json_get_prop_strptr1(profile, "current_status");
		g_free(user->status);
		user->status = g_strdup(status);

		if (purple_account_get_bool(sa->account, "enable_avatar_download", FALSE)) {
			const char *avatar_hash = json_get_prop_strptr1(profile, "avatar_hash");
			const char *avatar_url = json_get_prop_strptr1(profile, "image_192");
			g_free(user->avatar_hash);
			g_free(user->avatar_url);
			user->avatar_hash = g_strdup(avatar_hash);
			user->avatar_url = g_strdup(avatar_url);
			slack_update_avatar(sa, user);
		}

		if (user == sa->self)
			purple_account_set_user_info(sa->account, sa->self->status);
	}

	return user;
}

void slack_user_changed(SlackAccount *sa, json_value *json) {
	slack_user_update(sa, json_get_prop(json, "user"));
}

static gboolean users_list_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	json_value *members = json_get_prop_type(json, "members", array);
	if (!members) {
		purple_connection_error_reason(sa->gc,
				PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error ?: "Missing user list");
		return FALSE;
	}

	for (unsigned i = 0; i < members->u.array.length; i ++)
		slack_user_update(sa, members->u.array.values[i]);

	char *cursor = json_get_prop_strptr1(json_get_prop(json, "response_metadata"), "next_cursor");
	if (cursor)
		slack_api_post(sa, users_list_cb, NULL, "users.list", "presence", "false", SLACK_PAGINATE_LIMIT_ARG, "cursor", cursor, NULL);
	else
		slack_login_step(sa);
	return FALSE;
}

void slack_users_load(SlackAccount *sa) {
	// g_hash_table_remove_all(sa->users); /* this isn't really necessary, and we'd prefer to preserve self */
	slack_api_post(sa, users_list_cb, NULL, "users.list", "presence", "false", SLACK_PAGINATE_LIMIT_ARG, NULL);
}

struct user_retrieve {
	SlackUserCallback *cb;
	gpointer data;
};

static gboolean user_retrieve_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	struct user_retrieve *lookup = data;
	json_value *user = json_get_prop_type(json, "user", object);
	SlackUser *obj = NULL;
	if (!user || error)
		purple_debug_error("slack", "Error retrieving user: %s\n", error ?: "missing");
	else
		obj = slack_user_update(sa, user);
	lookup->cb(sa, lookup->data, obj);
	g_free(lookup);
	return FALSE;
}

void slack_user_retrieve(SlackAccount *sa, const char *uid, SlackUserCallback *cb, gpointer data) {
	SlackUser *user = (SlackUser *)slack_object_hash_table_lookup(sa->users, uid);
	if (user && !uid)
		return cb(sa, data, user);
	struct user_retrieve *lookup = g_new(struct user_retrieve, 1);
	lookup->cb = cb;
	lookup->data = data;
	slack_api_post(sa, user_retrieve_cb, lookup, "users.info", "user", uid, NULL);
}

static void presence_set(SlackAccount *sa, json_value *json, const char *presence) {
	if (json->type != json_string)
		return;
	const char *id = json->u.string.ptr;
	SlackUser *user = (SlackUser*)slack_object_hash_table_lookup(sa->users, id);
	if (!user || !user->object.name)
		return;
	purple_debug_misc("slack", "setting user %s presence to %s\n", user->object.name, presence);
	purple_prpl_got_user_status(sa->account, user->object.name, presence, NULL);
}

void slack_presence_change(SlackAccount *sa, json_value *json) {
	json_value *users = json_get_prop(json, "users");
	if (!users)
		users = json_get_prop(json, "user");
	const char *presence = json_get_prop_strptr(json, "presence");
	if (!users || !presence)
		return;

	if (users->type == json_array)
		for (unsigned i = 0; i < users->u.array.length; i ++)
			presence_set(sa, users->u.array.values[i], presence);
	else
		presence_set(sa, users, presence);
}

char *slack_status_text(PurpleBuddy *buddy) {
	SlackAccount *sa;
	SlackObject *obj = slack_blist_node_get_obj(PURPLE_BLIST_NODE(buddy), &sa);
	g_return_val_if_fail(SLACK_IS_USER(obj), NULL);
	SlackUser *user = (SlackUser*)obj;
	return user ? g_strdup(user->status) : NULL;
}

static gboolean users_info_cb(SlackAccount *sa, gpointer data, json_value *json, const char *error) {
	char *who = data;

	json = json_get_prop_type(json, "user", object);

	if (error || !json) {
		/* need to close userinfo dialog somehow? */
		purple_notify_error(sa->gc, "User info error", "No such user", error ?: who);
		g_free(who);
		return FALSE;
	}

	PurpleNotifyUserInfo *info = purple_notify_user_info_new();

	const char *s;
	time_t t;
	if ((s = json_get_prop_strptr(json, "id")))
		purple_notify_user_info_add_pair_plaintext(info, "id", s);
	if ((s = json_get_prop_strptr(json, "name")))
		purple_notify_user_info_add_pair_plaintext(info, "username", s);
	if (json_get_prop_boolean(json, "deleted", FALSE))
		purple_notify_user_info_add_pair_plaintext(info, "status", "deleted");
	if (json_get_prop_boolean(json, "is_primary_owner", FALSE))
		purple_notify_user_info_add_pair_plaintext(info, "role", "primary owner");
	else if (json_get_prop_boolean(json, "is_owner", FALSE))
		purple_notify_user_info_add_pair_plaintext(info, "role", "owner");
	else if (json_get_prop_boolean(json, "is_admin", FALSE))
		purple_notify_user_info_add_pair_plaintext(info, "role", "admin");
	else if (json_get_prop_boolean(json, "is_ultra_restricted", FALSE))
		purple_notify_user_info_add_pair_plaintext(info, "role", "ultra restricted");
	else if (json_get_prop_boolean(json, "is_restricted", FALSE))
		purple_notify_user_info_add_pair_plaintext(info, "role", "restricted");
	if (json_get_prop_boolean(json, "has_2fa", FALSE)) {
		s = json_get_prop_strptr(json, "two_factor_type");
		purple_notify_user_info_add_pair_plaintext(info, "2fa", s ?: "true");
	}
	if ((t = slack_parse_time(json_get_prop(json, "updated"))))
		purple_notify_user_info_add_pair_plaintext(info, "updated", purple_date_format_long(localtime(&t)));

	json_value *prof = json_get_prop_type(json, "profile", object);
	if (prof) {
		purple_notify_user_info_add_section_header(info, "profile");
		if ((s = json_get_prop_strptr(prof, "display_name")))
			purple_notify_user_info_add_pair_plaintext(info, "name", s);
		if ((s = json_get_prop_strptr(prof, "status_text")))
			purple_notify_user_info_add_pair_plaintext(info, "status", s);
		if ((s = json_get_prop_strptr(prof, "first_name")))
			purple_notify_user_info_add_pair_plaintext(info, "first name", s);
		if ((s = json_get_prop_strptr(prof, "last_name")))
			purple_notify_user_info_add_pair_plaintext(info, "last name", s);
		if ((s = json_get_prop_strptr(prof, "real_name")))
			purple_notify_user_info_add_pair_plaintext(info, "real name", s);
		if ((s = json_get_prop_strptr(prof, "email")))
			purple_notify_user_info_add_pair_plaintext(info, "email", s);
		if ((s = json_get_prop_strptr(prof, "skype")))
			purple_notify_user_info_add_pair_plaintext(info, "skype", s);
		if ((s = json_get_prop_strptr(prof, "phone")))
			purple_notify_user_info_add_pair_plaintext(info, "phone", s);
		if ((s = json_get_prop_strptr(prof, "title")))
			purple_notify_user_info_add_pair_plaintext(info, "title", s);
	}

	purple_notify_userinfo(sa->gc, who, info, NULL, NULL);
	purple_notify_user_info_destroy(info);
	g_free(who);
	return FALSE;
}

void slack_set_info(PurpleConnection *gc, const char *info) {
	SlackAccount *sa = gc->proto_data;
	slack_api_post(sa, NULL, NULL, "users.profile.set", "name", "status_text", "value", info, NULL);
}

void slack_get_info(PurpleConnection *gc, const char *who) {
	SlackAccount *sa = gc->proto_data;
	SlackUser *user = g_hash_table_lookup(sa->user_names, who);
	if (!user)
		users_info_cb(sa, g_strdup(who), NULL, NULL);
	else
		slack_api_post(sa, users_info_cb, g_strdup(who), "users.info", "user", user->object.id, NULL);
}

static void avatar_load_next(SlackAccount *sa);

static void avatar_cb(G_GNUC_UNUSED PurpleUtilFetchUrlData *fetch, gpointer data, const gchar *buf, gsize len, const gchar *error) {
	SlackAccount *sa = data;
	SlackUser *user = g_queue_pop_head(&sa->avatar_queue);
	g_return_if_fail(user);
	if (error) {
		purple_debug_warning("slack", "avatar download failed: %s\n", error);
	} else {
		gpointer icon_data = g_memdup(buf, len);
		purple_buddy_icons_set_for_user(sa->account, user->object.name, icon_data, len, user->avatar_hash);
	}

	g_object_unref(user);
	avatar_load_next(sa);
}

static void avatar_load_next(SlackAccount *sa) {
	SlackUser *user = g_queue_peek_head(&sa->avatar_queue);
	if (!user)
		return;
	purple_debug_misc("slack", "downloading avatar for %s\n", user->object.name);
	purple_util_fetch_url_request_len_with_account(sa->account, user->avatar_url, TRUE, NULL, TRUE, NULL, FALSE, 131072, avatar_cb, sa);
}

void slack_update_avatar(SlackAccount *sa, SlackUser *user) {
	if (!(user->object.buddy && user->avatar_hash && user->avatar_url))
		return;

	const char *checksum = purple_buddy_icons_get_checksum_for_user(user_buddy(user));
	if (!g_strcmp0(checksum, user->avatar_hash))
		return;

	/* if nothing was on the queue being loaded, we will start a new load */
	gboolean empty = g_queue_is_empty(&sa->avatar_queue);

	/* increase user ref-count to be decreased in avatar_cb */
	g_object_ref(user);
	g_queue_push_tail(&sa->avatar_queue, user);
	purple_debug_misc("slack", "new avatar for %s, queueing for download.\n", user->object.name);
	if (empty)
		avatar_load_next(sa);
}
