#include "gva-common.h"

#include "gva-error.h"

static GArray *history_file_offset_array = NULL;
static GHashTable *history_file_offset_table = NULL;

static GIOChannel *
history_file_open (GError **error)
{
        GIOChannel *channel = NULL;

#ifdef HISTORY_FILE
        channel = g_io_channel_new_file (HISTORY_FILE, "r", error);

        if (channel != NULL)
        {
                GIOStatus status;

                status = g_io_channel_set_encoding (channel, NULL, NULL);
                g_assert (status == G_IO_STATUS_NORMAL);
        }
#else
        g_set_error (
                error, GVA_ERROR, GVA_ERROR_CONFIG,
                _("This program is not configured to show "
                  "arcade history information."));
#endif

        return channel;
}

static void
history_file_mark (const gchar *line, gint64 offset)
{
        gchar **names;
        guint length, ii;

        g_array_append_val (history_file_offset_array, offset);

        /* Skip "$info=" prefix. */
        names = g_strsplit (line + 6, ",", -1);
        length = g_strv_length (names);

        for (ii = 0; ii < length; ii++)
        {
                if (*g_strstrip (names[ii]) == '\0')
                        continue;

                g_hash_table_insert (
                        history_file_offset_table, g_strdup (names[ii]),
                        GUINT_TO_POINTER (history_file_offset_array->len - 1));
        }

        g_strfreev (names);
}

gboolean
gva_history_init (GError **error)
{
        GIOChannel *channel;
        GIOStatus status;
        GString *buffer;
        gint64 offset = 0;

        g_return_val_if_fail (history_file_offset_array == NULL, FALSE);
        g_return_val_if_fail (history_file_offset_table == NULL, FALSE);

        channel = history_file_open (error);
        if (channel == NULL)
                return FALSE;

        history_file_offset_array =
                g_array_new (FALSE, FALSE, sizeof (gint64));

        history_file_offset_table = g_hash_table_new_full (
                g_str_hash, g_str_equal,
                (GDestroyNotify) g_free,
                (GDestroyNotify) NULL);

        buffer = g_string_sized_new (1024);

        while (TRUE)
        {
                status = G_IO_STATUS_AGAIN;
                while (status == G_IO_STATUS_AGAIN)
                        status = g_io_channel_read_line_string (
                                channel, buffer, NULL, error);

                if (status != G_IO_STATUS_NORMAL)
                        break;

                offset += buffer->len;

                if (g_str_has_prefix (buffer->str, "$info="))
                        history_file_mark (buffer->str, offset);
        }

        g_string_free (buffer, TRUE);
        g_io_channel_unref (channel);

        return (status == G_IO_STATUS_EOF);
}

gchar *
gva_history_lookup (const gchar *name,
                    GError **error)
{
        GIOChannel *channel;
        GIOStatus status;
        GString *buffer;
        GString *history;
        gboolean free_history;
        gpointer key, value;
        gint64 offset;
        guint index;

        g_return_val_if_fail (name != NULL, NULL);
        g_return_val_if_fail (history_file_offset_array != NULL, NULL);
        g_return_val_if_fail (history_file_offset_table != NULL, NULL);

        channel = history_file_open (error);
        if (channel == NULL)
                return NULL;

        buffer = g_string_sized_new (1024);
        history = g_string_sized_new (8096);
        free_history = TRUE;  /* assume failure */

        if (!g_hash_table_lookup_extended (
                history_file_offset_table, name, &key, &value))
                goto exit;

        index = GPOINTER_TO_UINT (value);
        g_assert (index < history_file_offset_array->len);
        offset = g_array_index (history_file_offset_array, gint64, index);

        status = G_IO_STATUS_AGAIN;
        while (status == G_IO_STATUS_AGAIN)
                status = g_io_channel_seek_position (
                        channel, offset, G_SEEK_SET, error);
        if (status == G_IO_STATUS_ERROR)
                goto exit;

        while (TRUE)
        {
                gchar *utf8;
                gsize bytes_written;

                status = G_IO_STATUS_AGAIN;
                while (status == G_IO_STATUS_AGAIN)
                        status = g_io_channel_read_line_string (
                                channel, buffer, NULL, error);

                if (status == G_IO_STATUS_ERROR)
                        goto exit;

                if (status == G_IO_STATUS_EOF)
                {
                        g_warning ("Unexpected end of history file");
                        break;
                }

                if (g_str_has_prefix (buffer->str, "$info="))
                {
                        g_warning ("Unexpected $info line in history file");
                        g_warning ("\"%s\"", buffer->str);
                        goto exit;
                }

                if (g_str_has_prefix (buffer->str, "$bio"))
                        continue;

                if (g_str_has_prefix (buffer->str, "$end"))
                        break;

                /* Be forgiving of the input.  If a conversion
                 * error occurs, skip the line and move on. */
                utf8 = g_locale_to_utf8 (
                        buffer->str, buffer->len, NULL, &bytes_written, NULL);
                if (utf8 != NULL)
                        g_string_append_len (history, utf8, bytes_written);
                g_free (utf8);
        }

        free_history = FALSE;

exit:
        g_io_channel_unref (channel);
        g_string_free (buffer, TRUE);

        return g_string_free (history, free_history);
}