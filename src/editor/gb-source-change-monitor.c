/* gb-source-change-monitor.c
 *
 * Copyright (C) 2014 Christian Hergert <christian@hergert.me>
 *
 * This file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "change-monitor"

#include <glib/gi18n.h>
#include <gtksourceview/gtksource.h>
#include <libgit2-glib/ggit.h>

#include "gb-log.h"
#include "gb-source-change-monitor.h"

#define PARSE_TIMEOUT_MSEC       100
#define GB_SOURCE_CHANGE_DELETED (1 << 3)
#define GB_SOURCE_CHANGE_MASK    (0x7)

struct _GbSourceChangeMonitorPrivate
{
  GtkTextBuffer  *buffer;
  GFile          *file;
  GgitRepository *repo;
  GgitBlob       *blob;
  gchar          *relative_path;
  GHashTable     *state;
  guint           changed_handler;
  guint           parse_timeout;
};

enum
{
  PROP_0,
  PROP_BUFFER,
  PROP_FILE,
  LAST_PROP
};

enum
{
  CHANGED,
  LAST_SIGNAL
};

G_DEFINE_TYPE_WITH_PRIVATE (GbSourceChangeMonitor,
                            gb_source_change_monitor,
                            G_TYPE_OBJECT)

static GParamSpec *gParamSpecs [LAST_PROP];
static guint       gSignals [LAST_SIGNAL];

GbSourceChangeFlags
gb_source_change_monitor_get_line (GbSourceChangeMonitor *monitor,
                                   guint                  lineno)
{
  g_return_val_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor), 0);

  if (monitor->priv->state)
    {
      gpointer value;

      value = g_hash_table_lookup (monitor->priv->state,
                                   GINT_TO_POINTER (lineno + 1));
      return (GPOINTER_TO_INT (value) & GB_SOURCE_CHANGE_MASK);
    }

  return GB_SOURCE_CHANGE_NONE;
}

static gint
diff_line_cb (GgitDiffDelta *delta,
              GgitDiffHunk  *hunk,
              GgitDiffLine  *line,
              gpointer       user_data)
{
  GgitDiffLineType type;
  GHashTable *hash = user_data;
  gint new_lineno;
  gint old_lineno;
  gint adjust;

  g_return_val_if_fail (delta, GGIT_ERROR_GIT_ERROR);
  g_return_val_if_fail (hunk, GGIT_ERROR_GIT_ERROR);
  g_return_val_if_fail (line, GGIT_ERROR_GIT_ERROR);
  g_return_val_if_fail (hash, GGIT_ERROR_GIT_ERROR);

  type = ggit_diff_line_get_origin (line);

  if ((type != GGIT_DIFF_LINE_ADDITION) && (type != GGIT_DIFF_LINE_DELETION))
    return 0;

  new_lineno = ggit_diff_line_get_new_lineno (line);
  old_lineno = ggit_diff_line_get_old_lineno (line);

  switch (type)
    {
    case GGIT_DIFF_LINE_ADDITION:
      if (g_hash_table_lookup (hash, GINT_TO_POINTER (new_lineno)))
        g_hash_table_replace (hash,
                              GINT_TO_POINTER (new_lineno),
                              GINT_TO_POINTER (GB_SOURCE_CHANGE_CHANGED));
      else
        g_hash_table_insert (hash,
                             GINT_TO_POINTER (new_lineno),
                             GINT_TO_POINTER (GB_SOURCE_CHANGE_ADDED));
      break;

    case GGIT_DIFF_LINE_DELETION:
      adjust = (ggit_diff_hunk_get_new_start (hunk) -
                ggit_diff_hunk_get_old_start (hunk));
      old_lineno += adjust;
      if (g_hash_table_lookup (hash, GINT_TO_POINTER (old_lineno)))
        g_hash_table_replace (hash,
                              GINT_TO_POINTER (old_lineno),
                              GINT_TO_POINTER (GB_SOURCE_CHANGE_CHANGED));
      else
        g_hash_table_insert (hash,
                             GINT_TO_POINTER (old_lineno),
                             GINT_TO_POINTER (GB_SOURCE_CHANGE_DELETED));
      break;

    case GGIT_DIFF_LINE_CONTEXT:
    case GGIT_DIFF_LINE_CONTEXT_EOFNL:
    case GGIT_DIFF_LINE_ADD_EOFNL:
    case GGIT_DIFF_LINE_DEL_EOFNL:
    case GGIT_DIFF_LINE_FILE_HDR:
    case GGIT_DIFF_LINE_HUNK_HDR:
    case GGIT_DIFF_LINE_BINARY:
    default:
      break;
    }

  return 0;
}

static gboolean
on_parse_timeout (GbSourceChangeMonitor *monitor)
{
  GbSourceChangeMonitorPrivate *priv;
  GtkTextIter begin;
  GtkTextIter end;
  GError *error = NULL;
  gchar *text = NULL;

  /*
   * TODO: Move this to a worker thread!
   *       We probably want to generate the ghashtable and then pass back that
   *       new state to the main thread via an async worker func/cb.
   */

  g_assert (GB_IS_SOURCE_CHANGE_MONITOR (monitor));

  priv = monitor->priv;

  if (!priv->blob || !priv->relative_path || !priv->buffer || !priv->file)
    return G_SOURCE_REMOVE;

  /*
   * First, disable this so any side-effects cause a new parse to occur.
   */
  priv->parse_timeout = 0;

  /*
   * We are about to invalidate everything, so just clear out the hash table.
   */
  g_hash_table_remove_all (priv->state);

  /*
   * Load the contents of the buffer from the GtkTextBuffer.
   */
  gtk_text_buffer_get_bounds (priv->buffer, &begin, &end);
  text = gtk_text_buffer_get_text (priv->buffer, &begin, &end, TRUE);

  /*
   * Ask ggit to diff the buffer for us. We will get a callback for all of
   * the changes that we can then turn into Add/Change line statuses.
   */
  ggit_diff_blob_to_buffer (priv->blob, priv->relative_path,
                            (const guint8 *)text, -1, priv->relative_path,
                            NULL, NULL, NULL, diff_line_cb,
                            priv->state, &error);

  if (error)
    {
      g_message ("Failed to generate diff: %s", error->message);
      g_clear_error (&error);
      GOTO (cleanup);
    }

  /*
   * Notify any listeners (such as the gutter renderer) of potential changes.
   */
  g_signal_emit (monitor, gSignals [CHANGED], 0);

cleanup:
  g_free (text);

  return G_SOURCE_REMOVE;
}

static void
gb_source_change_monitor_queue_parse (GbSourceChangeMonitor *monitor)
{
  GbSourceChangeMonitorPrivate *priv;

  g_return_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor));

  priv = monitor->priv;

  if (!priv->repo || !priv->blob || !priv->file)
    return;

  if (priv->parse_timeout)
    {
      g_source_remove (priv->parse_timeout);
      priv->parse_timeout = 0;
    }

  priv->parse_timeout = g_timeout_add (PARSE_TIMEOUT_MSEC,
                                       (GSourceFunc)on_parse_timeout,
                                       monitor);
}

static void
on_change_cb (GbSourceChangeMonitor *monitor,
              GtkTextBuffer         *buffer)
{
  g_return_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor));
  g_return_if_fail (GTK_IS_TEXT_BUFFER (buffer));

  gb_source_change_monitor_queue_parse (monitor);
}

static void
gb_source_change_monitor_load_blob (GbSourceChangeMonitor *monitor)
{
  GbSourceChangeMonitorPrivate *priv;
  GgitOId *entry_oid = NULL;
  GgitOId *oid = NULL;
  GgitObject *blob = NULL;
  GgitObject *commit = NULL;
  GgitRef *head = NULL;
  GgitTree *tree = NULL;
  GgitTreeEntry *entry = NULL;
  GFile *workdir = NULL;
  GError *error = NULL;
  gchar *relpath = NULL;

  g_return_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor));

  priv = monitor->priv;

  /*
   * Double check we have everything we need.
   */
  if (!priv->repo || !priv->file)
    return;

  /*
   * Work our way through ggit to get to the original blob we care about.
   */
  head = ggit_repository_get_head (priv->repo, &error);
  if (!head)
    GOTO (cleanup);

  oid = ggit_ref_get_target (head);
  if (!oid)
    GOTO (cleanup);

  commit = ggit_repository_lookup (priv->repo, oid, GGIT_TYPE_COMMIT, &error);
  if (!commit)
    GOTO (cleanup);

  tree = ggit_commit_get_tree (GGIT_COMMIT (commit));
  if (!tree)
    GOTO (cleanup);

  workdir = ggit_repository_get_workdir (priv->repo);
  if (!workdir)
    GOTO (cleanup);

  relpath = g_file_get_relative_path (workdir, priv->file);
  if (!relpath)
    GOTO (cleanup);

  entry = ggit_tree_get_by_path (tree, relpath, &error);
  if (!entry)
    GOTO (cleanup);

  entry_oid = ggit_tree_entry_get_id (entry);
  if (!entry_oid)
    GOTO (cleanup);

  blob = ggit_repository_lookup (priv->repo, entry_oid, GGIT_TYPE_BLOB, &error);
  if (!blob)
    GOTO (cleanup);

  priv->blob = g_object_ref (blob);
  priv->relative_path = g_strdup (relpath);

cleanup:
  if (error)
    {
      g_warning ("%s", error->message);
      g_clear_error (&error);
    }

  g_clear_object (&blob);
  g_clear_pointer (&entry_oid, ggit_oid_free);
  g_clear_pointer (&entry, ggit_tree_entry_unref);
  g_clear_pointer (&relpath, g_free);
  g_clear_object (&workdir);
  g_clear_object (&tree);
  g_clear_object (&commit);
  g_clear_pointer (&oid, ggit_oid_free);
  g_clear_object (&head);
}

static void
gb_source_change_monitor_discover_repository (GbSourceChangeMonitor *monitor)
{
  GbSourceChangeMonitorPrivate *priv;

  ENTRY;

  g_return_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor));

  /*
   * TODO: This makes a lot of assumptions.
   *   - that we are local
   *   - that checking disk is free
   *   - that we don't need to cache anything.
   *
   *  and all of those are probably wrong.
   */

  priv = monitor->priv;

  g_clear_object (&priv->repo);

  if (priv->file)
    {
      GFile *repo_file;
      GError *error = NULL;

      repo_file = ggit_repository_discover (priv->file, &error);

      if (!repo_file)
        {
          g_message ("Failed to locate a gir repository: %s", error->message);
          g_clear_error (&error);
          EXIT;
        }

      priv->repo = ggit_repository_open (repo_file, &error);

      if (!priv->repo)
        {
          g_message ("Failed to open git repository: %s", error->message);
          g_clear_error (&error);
        }

      g_clear_object (&repo_file);
    }

  EXIT;
}

GtkTextBuffer *
gb_source_change_monitor_get_buffer (GbSourceChangeMonitor *monitor)
{
  g_return_val_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor), NULL);

  return monitor->priv->buffer;
}

static void
gb_source_change_monitor_set_buffer (GbSourceChangeMonitor *monitor,
                                     GtkTextBuffer         *buffer)
{
  GbSourceChangeMonitorPrivate *priv;

  ENTRY;

  g_return_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor));
  g_return_if_fail (!buffer || GB_IS_SOURCE_CHANGE_MONITOR (monitor));

  priv = monitor->priv;

  if (priv->buffer)
    {
      g_signal_handler_disconnect (priv->buffer, priv->changed_handler);
      priv->changed_handler = 0;
      g_clear_object (&priv->buffer);
    }

  if (buffer)
    {
      priv->buffer = g_object_ref (buffer);
      priv->changed_handler =
        g_signal_connect_object (priv->buffer,
                                 "changed",
                                 G_CALLBACK (on_change_cb),
                                 monitor,
                                 G_CONNECT_SWAPPED);
    }

  gb_source_change_monitor_queue_parse (monitor);

  EXIT;
}

GFile *
gb_source_change_monitor_get_file (GbSourceChangeMonitor *monitor)
{
  g_return_val_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor), NULL);

  return monitor->priv->file;
}

void
gb_source_change_monitor_set_file (GbSourceChangeMonitor *monitor,
                                   GFile                 *file)
{
  GbSourceChangeMonitorPrivate *priv;

  ENTRY;

  g_return_if_fail (GB_IS_SOURCE_CHANGE_MONITOR (monitor));
  g_return_if_fail (!file || G_IS_FILE (file));

  priv = monitor->priv;

  if (file == priv->file)
    EXIT;

  g_clear_object (&priv->file);
  g_clear_object (&priv->blob);
  g_clear_object (&priv->repo);

  if (file)
    {
      priv->file = g_object_ref (file);
      gb_source_change_monitor_discover_repository (monitor);
      gb_source_change_monitor_load_blob (monitor);
    }

  g_object_notify_by_pspec (G_OBJECT (monitor), gParamSpecs [PROP_FILE]);

  gb_source_change_monitor_queue_parse (monitor);

  EXIT;
}

static void
gb_source_change_monitor_dispose (GObject *object)
{
  GbSourceChangeMonitor *monitor = (GbSourceChangeMonitor *)object;

  ENTRY;

  gb_source_change_monitor_set_buffer (monitor, NULL);
  gb_source_change_monitor_set_file (monitor, NULL);

  g_clear_object (&monitor->priv->repo);
  g_clear_object (&monitor->priv->blob);

  if (monitor->priv->parse_timeout)
    {
      g_source_remove (monitor->priv->parse_timeout);
      monitor->priv->parse_timeout = 0;
    }

  G_OBJECT_CLASS (gb_source_change_monitor_parent_class)->dispose (object);

  EXIT;
}

static void
gb_source_change_monitor_finalize (GObject *object)
{
  GbSourceChangeMonitorPrivate *priv = GB_SOURCE_CHANGE_MONITOR (object)->priv;

  g_clear_pointer (&priv->state, g_hash_table_unref);
  g_clear_pointer (&priv->relative_path, g_free);

  G_OBJECT_CLASS (gb_source_change_monitor_parent_class)->finalize (object);
}

static void
gb_source_change_monitor_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  GbSourceChangeMonitor *monitor = GB_SOURCE_CHANGE_MONITOR (object);

  switch (prop_id)
    {
    case PROP_BUFFER:
      g_value_set_object (value,
                          gb_source_change_monitor_get_buffer (monitor));
      break;

    case PROP_FILE:
      g_value_set_object (value,
                          gb_source_change_monitor_get_file (monitor));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_change_monitor_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  GbSourceChangeMonitor *monitor = GB_SOURCE_CHANGE_MONITOR (object);

  switch (prop_id)
    {
    case PROP_BUFFER:
      gb_source_change_monitor_set_buffer (monitor,
                                           g_value_get_object (value));
      break;

    case PROP_FILE:
      gb_source_change_monitor_set_file (monitor, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_change_monitor_class_init (GbSourceChangeMonitorClass *klass)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = gb_source_change_monitor_dispose;
  object_class->finalize = gb_source_change_monitor_finalize;
  object_class->get_property = gb_source_change_monitor_get_property;
  object_class->set_property = gb_source_change_monitor_set_property;

  gParamSpecs [PROP_BUFFER] =
    g_param_spec_object ("buffer",
                         _("Buffer"),
                         _("The text buffer to monitor."),
                         GTK_TYPE_TEXT_BUFFER,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_BUFFER,
                                   gParamSpecs [PROP_BUFFER]);

  gParamSpecs [PROP_FILE] =
    g_param_spec_object ("file",
                         _("File"),
                         _("The file for the buffer."),
                         G_TYPE_FILE,
                         (G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FILE,
                                   gParamSpecs [PROP_FILE]);

  gSignals [CHANGED] =
    g_signal_new ("changed",
                  GB_TYPE_SOURCE_CHANGE_MONITOR,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbSourceChangeMonitorClass, changed),
                  NULL,
                  NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);
}

static void
gb_source_change_monitor_init (GbSourceChangeMonitor *monitor)
{
  ENTRY;
  monitor->priv = gb_source_change_monitor_get_instance_private (monitor);
  monitor->priv->state = g_hash_table_new (g_direct_hash, g_direct_equal);
  EXIT;
}
