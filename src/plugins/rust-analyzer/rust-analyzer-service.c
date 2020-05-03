/* rust-analyzer-service.c
 *
 * Copyright 2020 Günther Wagner <info@gunibert.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "rust-analyzer-service"

#include "rust-analyzer-service.h"
#include "rust-analyzer-transfer.h"
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <libide-core.h>

struct _RustAnalyzerService
{
  IdeObject parent_instance;
  IdeLspClient  *client;
  IdeSubprocessSupervisor *supervisor;

  ServiceState state;
};

G_DEFINE_TYPE (RustAnalyzerService, rust_analyzer_service, IDE_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_CLIENT,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

RustAnalyzerService *
rust_analyzer_service_new (void)
{
  return g_object_new (RUST_TYPE_ANALYZER_SERVICE, NULL);
}

static void
rust_analyzer_service_finalize (GObject *object)
{
  RustAnalyzerService *self = (RustAnalyzerService *)object;

  g_clear_object (&self->client);

  G_OBJECT_CLASS (rust_analyzer_service_parent_class)->finalize (object);
}

static void
rust_analyzer_service_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  RustAnalyzerService *self = RUST_ANALYZER_SERVICE (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, rust_analyzer_service_get_client (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
rust_analyzer_service_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  RustAnalyzerService *self = RUST_ANALYZER_SERVICE (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      rust_analyzer_service_set_client (self, g_value_get_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
rust_analyzer_service_destroy (IdeObject *object)
{
  RustAnalyzerService *self = RUST_ANALYZER_SERVICE (object);

  if (self->supervisor != NULL)
    {
      g_autoptr(IdeSubprocessSupervisor) supervisor = g_steal_pointer (&self->supervisor);

      ide_subprocess_supervisor_stop (supervisor);
    }

  IDE_OBJECT_CLASS (rust_analyzer_service_parent_class)->destroy (object);
}

static void
rust_analyzer_service_class_init (RustAnalyzerServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  IdeObjectClass *i_class = IDE_OBJECT_CLASS (klass);

  object_class->finalize = rust_analyzer_service_finalize;
  object_class->get_property = rust_analyzer_service_get_property;
  object_class->set_property = rust_analyzer_service_set_property;

  i_class->destroy = rust_analyzer_service_destroy;

  properties [PROP_CLIENT] =
    g_param_spec_object ("client",
                         "Client",
                         "The Language Server client",
                         IDE_TYPE_LSP_CLIENT,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
rust_analyzer_service_init (RustAnalyzerService *self)
{
  self->client = NULL;
  self->state = RUST_ANALYZER_SERVICE_INIT;
}

IdeLspClient *
rust_analyzer_service_get_client (RustAnalyzerService *self)
{
  g_return_val_if_fail (RUST_IS_ANALYZER_SERVICE (self), NULL);

  return self->client;
}

void
rust_analyzer_service_set_client (RustAnalyzerService *self,
                                  IdeLspClient        *client)
{
  g_return_if_fail (RUST_IS_ANALYZER_SERVICE (self));
  g_return_if_fail (!client || IDE_IS_LSP_CLIENT (client));

  if (g_set_object (&self->client, client))
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_CLIENT]);
    }
}

void
rust_analyzer_service_lsp_started (IdeSubprocessSupervisor *supervisor,
                                   IdeSubprocess           *subprocess,
                                   gpointer                 user_data)
{
  g_autoptr(GIOStream) io_stream = NULL;
  GInputStream *input;
  GOutputStream *output;
  IdeLspClient *client = NULL;

  RustAnalyzerService *self = RUST_ANALYZER_SERVICE (user_data);

  input = ide_subprocess_get_stdout_pipe (subprocess);
  output = ide_subprocess_get_stdin_pipe (subprocess);
  io_stream = g_simple_io_stream_new (input, output);

  if (self->client != NULL)
    {
      ide_lsp_client_stop (self->client);
      ide_object_destroy (IDE_OBJECT (self->client));
    }

  client = ide_lsp_client_new (io_stream);
  rust_analyzer_service_set_client (self, client);
  ide_object_append (IDE_OBJECT (self), IDE_OBJECT (client));
  ide_lsp_client_add_language (client, "rust");
  ide_lsp_client_start (client);
}

static gboolean
rust_analyzer_service_check_rust_analyzer_bin (RustAnalyzerService *self)
{
  // Check if `rust-analyzer` can be found on PATH or if there is an executable
  // in typical location
  g_autoptr(GFile) rust_analyzer_bin_file = NULL;
  g_autofree gchar *rust_analyzer_bin = NULL;
  g_autoptr(GFileInfo) file_info = NULL;

  rust_analyzer_bin = g_find_program_in_path ("rust-analyzer");
  if (rust_analyzer_bin == NULL)
    {
      g_autofree gchar *path = NULL;
      const gchar *homedir = g_get_home_dir ();

      path = g_build_path (G_DIR_SEPARATOR_S, homedir, ".cargo", "bin", "rust-analyzer", NULL);
      rust_analyzer_bin_file = g_file_new_for_path (path);
    }
  else
    {
      rust_analyzer_bin_file = g_file_new_for_path (rust_analyzer_bin);
    }

  if (!g_file_query_exists (rust_analyzer_bin_file, NULL))
    {
      return FALSE;
    }

  file_info = g_file_query_info (rust_analyzer_bin_file,
                                 "*",
                                 G_FILE_QUERY_INFO_NONE,
                                 NULL, NULL);

  if (ide_str_equal0 ("application/x-sharedlib", g_file_info_get_content_type (file_info)))
      return TRUE;

  return FALSE;
}

void
rust_analyzer_service_ensure_started (RustAnalyzerService *self)
{
  if (self->state == RUST_ANALYZER_SERVICE_INIT)
    {
      if (!rust_analyzer_service_check_rust_analyzer_bin (self))
        {
          g_autoptr(IdeNotification) notification = NULL;
          IdeContext *context = NULL;

          self->state = RUST_ANALYZER_SERVICE_OFFER_DOWNLOAD;

          notification = ide_notification_new ();
          ide_notification_set_id (notification, "org.gnome-builder.rust-analyzer");
          ide_notification_set_title (notification, "Your computer is missing the Rust Analyzer Language Server");
          ide_notification_set_body (notification, "The Language Server is necessary to provide IDE features like completion or diagnostic");
          ide_notification_set_icon_name (notification, "dialog-warning-symbolic");
          ide_notification_add_button (notification, "Install Language Server", NULL, "win.install-rust-analyzer");
          ide_notification_set_urgent (notification, TRUE);
          context = ide_object_get_context (IDE_OBJECT (self));
          ide_notification_attach (notification, IDE_OBJECT (context));
        }
      else
          self->state = RUST_ANALYZER_SERVICE_READY;
    }
  else if (self->state == RUST_ANALYZER_SERVICE_READY)
    {
      g_autofree gchar *newpath = NULL;
      g_autoptr(IdeSubprocessLauncher) launcher = NULL;
      IdeContext *context = NULL;
      GFile *workdir = NULL;
      const gchar *oldpath = NULL;

      launcher = ide_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDIN_PIPE);
      ide_subprocess_launcher_set_run_on_host (launcher, TRUE);
      ide_subprocess_launcher_set_clear_env (launcher, TRUE);

      context = ide_object_get_context (IDE_OBJECT (self));
      workdir = ide_context_ref_workdir (context);
      ide_subprocess_launcher_set_cwd (launcher, g_file_get_path (workdir));
      oldpath = g_getenv ("PATH");
      newpath = g_strdup_printf ("%s/%s:%s", g_get_home_dir (), ".cargo/bin", oldpath);
      ide_subprocess_launcher_setenv (launcher, "PATH", newpath, TRUE);

      ide_subprocess_launcher_push_argv (launcher, "rust-analyzer");

      self->supervisor = ide_subprocess_supervisor_new ();
      g_signal_connect (self->supervisor, "spawned", G_CALLBACK (rust_analyzer_service_lsp_started), self);
      ide_subprocess_supervisor_set_launcher (self->supervisor, launcher);
      ide_subprocess_supervisor_start (self->supervisor);
      self->state = RUST_ANALYZER_SERVICE_LSP_STARTED;
    }
}

void
rust_analyzer_service_set_state (RustAnalyzerService *self,
                                 ServiceState         state)
{
  g_return_if_fail (RUST_IS_ANALYZER_SERVICE (self));

  self->state = state;
}
