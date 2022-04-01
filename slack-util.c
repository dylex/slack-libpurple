#include "slack-util.h"
#include "glibcompat.h"
#include <string.h>

/*
  Backported G string replace function from glib 2.32

*/
guint g_string_replace_bp(GString * string, const gchar * find, const gchar * replace)
{
  gsize f_len, r_len, pos;
  gchar *cur, *next;
  guint n = 0;
  guint limit = 0;

  g_return_val_if_fail (string != NULL, 0);
  g_return_val_if_fail (find != NULL, 0);
  g_return_val_if_fail (replace != NULL, 0);

  f_len = strlen (find);
  r_len = strlen (replace);
  cur = string->str;

  while ((next = strstr (cur, find)) != NULL)
    {
      pos = next - string->str;
      g_string_erase (string, pos, f_len);
      g_string_insert (string, pos, replace);
      cur = string->str + pos + r_len;
      n++;
      if (n == limit)
        break;
    }

  return n;
}