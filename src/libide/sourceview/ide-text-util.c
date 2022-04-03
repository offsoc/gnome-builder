/*
 * gedit-view.c
 * This file is part of gedit
 *
 * Copyright 1998, 1999 Alex Roberts, Evan Lawrence
 * Copyright 2000, 2002 Chema Celorio, Paolo Maggi
 * Copyright 2003-2005 Paolo Maggi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "ide-text-util"

#include "config.h"

#include "ide-text-util.h"

void
ide_text_util_delete_line (GtkTextView *text_view,
                           gint         count)
{
	GtkTextIter start;
	GtkTextIter end;
	GtkTextBuffer *buffer;

	buffer = gtk_text_view_get_buffer (text_view);

	gtk_text_view_reset_im_context (text_view);

	/* If there is a selection delete the selected lines and ignore count. */
	if (gtk_text_buffer_get_selection_bounds (buffer, &start, &end))
	{
		gtk_text_iter_order (&start, &end);

		if (gtk_text_iter_starts_line (&end))
		{
			/* Do not delete the line with the cursor if the cursor
			 * is at the beginning of the line.
			 */
			count = 0;
		}
		else
		{
			count = 1;
		}
	}

	gtk_text_iter_set_line_offset (&start, 0);

	if (count > 0)
	{
		gtk_text_iter_forward_lines (&end, count);

		if (gtk_text_iter_is_end (&end))
		{
			if (gtk_text_iter_backward_line (&start) &&
			    !gtk_text_iter_ends_line (&start))
			{
				gtk_text_iter_forward_to_line_end (&start);
			}
		}
	}
	else if (count < 0)
	{
		if (!gtk_text_iter_ends_line (&end))
		{
			gtk_text_iter_forward_to_line_end (&end);
		}

		while (count < 0)
		{
			if (!gtk_text_iter_backward_line (&start))
			{
				break;
			}

			count++;
		}

		if (count == 0)
		{
			if (!gtk_text_iter_ends_line (&start))
			{
				gtk_text_iter_forward_to_line_end (&start);
			}
		}
		else
		{
			gtk_text_iter_forward_line (&end);
		}
	}

	if (!gtk_text_iter_equal (&start, &end))
	{
		GtkTextIter cur = start;
		gtk_text_iter_set_line_offset (&cur, 0);

		gtk_text_buffer_begin_user_action (buffer);

		gtk_text_buffer_place_cursor (buffer, &cur);

		gtk_text_buffer_delete_interactive (buffer,
						    &start,
						    &end,
						    gtk_text_view_get_editable (text_view));

		gtk_text_buffer_end_user_action (buffer);

		gtk_text_view_scroll_mark_onscreen (text_view,
						    gtk_text_buffer_get_insert (buffer));
	}
	else
	{
		gtk_widget_error_bell (GTK_WIDGET (text_view));
	}
}

static gboolean
find_prefix_match (const GtkTextIter *limit,
                   const GtkTextIter *end,
                   GtkTextIter       *found_start,
                   GtkTextIter       *found_end,
                   const char        *prefix,
                   gsize              len,
                   gsize              n_chars)
{
  g_autofree gchar *copy = g_utf8_substring (prefix, 0, n_chars);

  if (gtk_text_iter_backward_search (end, copy, GTK_TEXT_SEARCH_TEXT_ONLY, found_start, found_end, limit))
    return gtk_text_iter_equal (found_end, end);

  return FALSE;
}

void
ide_text_util_remove_common_prefix (GtkTextIter *begin,
                                    const gchar *prefix)
{
  GtkTextIter rm_begin;
  GtkTextIter rm_end;
  GtkTextIter line_start;
  GtkTextIter found_start, found_end;
  gboolean found = FALSE;
  gsize len;
  gsize count = 1;

  g_return_if_fail (begin != NULL);

  if (prefix == NULL || prefix[0] == 0)
    return;

  len = g_utf8_strlen (prefix, -1);
  line_start = *begin;
  gtk_text_iter_set_line_offset (&line_start, 0);

  while (count <= len &&
         find_prefix_match (&line_start, begin, &found_start, &found_end, prefix, len, count))
    {
      rm_begin = found_start;
      rm_end = found_end;
      count++;
      found = TRUE;
    }

  if (found)
    {
      gtk_text_buffer_delete (gtk_text_iter_get_buffer (begin), &rm_begin, &rm_end);
      *begin = rm_begin;
    }
}
