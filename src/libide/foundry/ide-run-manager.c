/* ide-run-manager.c
 *
 * Copyright 2016-2019 Christian Hergert <chergert@redhat.com>
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

#define G_LOG_DOMAIN "ide-run-manager"

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <libpeas/peas.h>
#include <libpeas/peas-autocleanups.h>

#include <libide-core.h>
#include <libide-plugins.h>
#include <libide-threading.h>
#include <libide-vcs.h>

#include "ide-private.h"

#include "ide-build-manager.h"
#include "ide-build-system.h"
#include "ide-deploy-strategy.h"
#include "ide-device-manager.h"
#include "ide-foundry-compat.h"
#include "ide-run-command.h"
#include "ide-run-command-provider.h"
#include "ide-run-context.h"
#include "ide-run-manager-private.h"
#include "ide-run-manager.h"
#include "ide-runner.h"
#include "ide-runtime.h"

struct _IdeRunManager
{
  IdeObject                parent_instance;

  GCancellable            *cancellable;
  IdeNotification         *notif;
  IdeExtensionSetAdapter  *run_command_providers;

  const IdeRunHandlerInfo *handler;
  GList                   *handlers;

  IdeSubprocess           *current_subprocess;
  IdeRunCommand           *current_run_command;

  /* Keep track of last change sequence from the file monitor
   * so that we can maybe skip past install phase and make
   * secondary execution time faster.
   */
  guint64                  last_change_seq;
  guint64                  pending_last_change_seq;

  char                    *default_run_command;

  guint                    busy;

  guint                    messages_debug_all : 1;
  guint                    has_installed_once : 1;
};

static void initable_iface_init                         (GInitableIface *iface);
static void ide_run_manager_actions_run                 (IdeRunManager  *self,
                                                         GVariant       *param);
static void ide_run_manager_actions_run_with_handler    (IdeRunManager  *self,
                                                         GVariant       *param);
static void ide_run_manager_actions_stop                (IdeRunManager  *self,
                                                         GVariant       *param);
static void ide_run_manager_actions_messages_debug_all  (IdeRunManager  *self,
                                                         GVariant       *param);
static void ide_run_manager_actions_default_run_command (IdeRunManager  *self,
                                                         GVariant       *param);
static void ide_run_manager_actions_color_scheme        (IdeRunManager  *self,
                                                         GVariant       *param);
static void ide_run_manager_actions_high_contrast       (IdeRunManager  *self,
                                                         GVariant       *param);
static void ide_run_manager_actions_text_direction      (IdeRunManager  *self,
                                                         GVariant       *param);

IDE_DEFINE_ACTION_GROUP (IdeRunManager, ide_run_manager, {
  { "run", ide_run_manager_actions_run },
  { "run-with-handler", ide_run_manager_actions_run_with_handler, "s" },
  { "stop", ide_run_manager_actions_stop },
  { "messages-debug-all", ide_run_manager_actions_messages_debug_all, NULL, "false" },
  { "default-run-command", ide_run_manager_actions_default_run_command, "s", "''" },
  { "color-scheme", ide_run_manager_actions_color_scheme, "s", "'follow'" },
  { "high-contrast", ide_run_manager_actions_high_contrast, NULL, "false" },
  { "text-direction", ide_run_manager_actions_text_direction, "s", "''" },
})

G_DEFINE_TYPE_EXTENDED (IdeRunManager, ide_run_manager, IDE_TYPE_OBJECT, G_TYPE_FLAG_FINAL,
                        G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init)
                        G_IMPLEMENT_INTERFACE (G_TYPE_ACTION_GROUP, ide_run_manager_init_action_group))

enum {
  PROP_0,
  PROP_BUSY,
  PROP_HANDLER,
  N_PROPS
};

enum {
  RUN,
  STOPPED,
  N_SIGNALS
};

static GParamSpec *properties [N_PROPS];
static guint signals [N_SIGNALS];

static void
ide_run_manager_actions_high_contrast (IdeRunManager *self,
                                       GVariant      *param)
{
  GVariant *state;

  g_assert (IDE_IS_RUN_MANAGER (self));

  state = ide_run_manager_get_action_state (self, "high-contrast");
  ide_run_manager_set_action_state (self,
                                    "high-contrast",
                                    g_variant_new_boolean (!g_variant_get_boolean (state)));
}

static void
ide_run_manager_actions_text_direction (IdeRunManager *self,
                                        GVariant      *param)
{
  const char *str;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));

  str = g_variant_get_string (param, NULL);
  if (g_strv_contains (IDE_STRV_INIT ("ltr", "rtl"), str))
    ide_run_manager_set_action_state (self,
                                      "text-direction",
                                      g_variant_new_string (str));
}

static void
ide_run_manager_actions_color_scheme (IdeRunManager *self,
                                      GVariant      *param)
{
  const char *str;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));

  str = g_variant_get_string (param, NULL);
  if (!g_strv_contains (IDE_STRV_INIT ("follow", "force-light", "force-dark"), str))
    str = "follow";

  ide_run_manager_set_action_state (self,
                                    "color-scheme",
                                    g_variant_new_string (str));
}

static void
ide_run_manager_actions_default_run_command (IdeRunManager *self,
                                             GVariant      *param)
{
  const char *str;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));

  str = g_variant_get_string (param, NULL);
  if (ide_str_empty0 (str))
    str = NULL;

  if (g_strcmp0 (str, self->default_run_command) != 0)
    {
      g_free (self->default_run_command);
      self->default_run_command = g_strdup (str);
      ide_run_manager_set_action_state (self,
                                        "default-run-command",
                                        g_variant_new_string (str ? str : ""));
    }
}

static void
ide_run_handler_info_free (gpointer data)
{
  IdeRunHandlerInfo *info = data;

  g_clear_pointer (&info->id, g_free);
  g_clear_pointer (&info->title, g_free);
  g_clear_pointer (&info->icon_name, g_free);

  if (info->handler_data_destroy)
    {
      GDestroyNotify notify = g_steal_pointer (&info->handler_data_destroy);
      gpointer notify_data = g_steal_pointer (&info->handler_data);

      notify (notify_data);
    }

  g_slice_free (IdeRunHandlerInfo, info);
}

static void
ide_run_manager_update_action_enabled (IdeRunManager *self)
{
  IdeBuildManager *build_manager;
  IdeContext *context;
  gboolean can_build;

  g_assert (IDE_IS_RUN_MANAGER (self));

  context = ide_object_get_context (IDE_OBJECT (self));
  build_manager = ide_build_manager_from_context (context);
  can_build = ide_build_manager_get_can_build (build_manager);

  ide_run_manager_set_action_enabled (self, "run",
                                      self->busy == 0 && can_build == TRUE);
  ide_run_manager_set_action_enabled (self, "run-with-handler",
                                      self->busy == 0 && can_build == TRUE);
  ide_run_manager_set_action_enabled (self, "stop", self->busy > 0);
}


static void
ide_run_manager_mark_busy (IdeRunManager *self)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));

  self->busy++;

  if (self->busy == 1)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_BUSY]);
      ide_run_manager_update_action_enabled (self);
    }

  IDE_EXIT;
}

static void
ide_run_manager_unmark_busy (IdeRunManager *self)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));

  self->busy--;

  if (self->busy == 0)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_BUSY]);
      ide_run_manager_update_action_enabled (self);
    }

  IDE_EXIT;
}

static void
ide_run_manager_dispose (GObject *object)
{
  IdeRunManager *self = (IdeRunManager *)object;

  self->handler = NULL;

  g_clear_pointer (&self->default_run_command, g_free);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->current_run_command);
  g_clear_object (&self->current_subprocess);

  ide_clear_and_destroy_object (&self->run_command_providers);

  g_list_free_full (self->handlers, ide_run_handler_info_free);
  self->handlers = NULL;

  G_OBJECT_CLASS (ide_run_manager_parent_class)->dispose (object);
}

static void
ide_run_manager_notify_can_build (IdeRunManager   *self,
                                  GParamSpec      *pspec,
                                  IdeBuildManager *build_manager)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (G_IS_PARAM_SPEC (pspec));
  g_assert (IDE_IS_BUILD_MANAGER (build_manager));

  ide_run_manager_update_action_enabled (self);

  IDE_EXIT;
}

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
  IdeRunManager *self = (IdeRunManager *)initable;
  IdeBuildManager *build_manager;
  IdeContext *context;

  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));

  context = ide_object_get_context (IDE_OBJECT (self));
  build_manager = ide_build_manager_from_context (context);

  g_signal_connect_object (build_manager,
                           "notify::can-build",
                           G_CALLBACK (ide_run_manager_notify_can_build),
                           self,
                           G_CONNECT_SWAPPED);

  ide_run_manager_update_action_enabled (self);

  self->run_command_providers = ide_extension_set_adapter_new (IDE_OBJECT (self),
                                                               peas_engine_get_default (),
                                                               IDE_TYPE_RUN_COMMAND_PROVIDER,
                                                               NULL, NULL);

  IDE_RETURN (TRUE);
}

static void
initable_iface_init (GInitableIface *iface)
{
  iface->init = initable_init;
}

static void
ide_run_manager_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  IdeRunManager *self = IDE_RUN_MANAGER (object);

  switch (prop_id)
    {
    case PROP_BUSY:
      g_value_set_boolean (value, ide_run_manager_get_busy (self));
      break;

    case PROP_HANDLER:
      g_value_set_string (value, ide_run_manager_get_handler (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_run_manager_class_init (IdeRunManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ide_run_manager_dispose;
  object_class->get_property = ide_run_manager_get_property;

  properties [PROP_BUSY] =
    g_param_spec_boolean ("busy",
                          "Busy",
                          "Busy",
                          FALSE,
                          (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  properties [PROP_HANDLER] =
    g_param_spec_string ("handler",
                         "Handler",
                         "Handler",
                         "run",
                         (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /**
   * IdeRunManager::run:
   * @self: An #IdeRunManager
   * @run_context: An #IdeRunContext
   *
   * This signal is emitted to allow plugins to add additional settings to a
   * run context before a launcher is created.
   *
   * Generally this can only be used in certain situations and you probably
   * want to modify the run context in another way such as a deploy strategry,
   * runtime, or similar.
   */
  signals [RUN] =
    g_signal_new_class_handler ("run",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                G_TYPE_NONE,
                                1,
                                IDE_TYPE_RUN_CONTEXT);

  /**
   * IdeRunManager::stopped:
   *
   * This signal is emitted when the run manager has stopped the currently
   * executing inferior.
   */
  signals [STOPPED] =
    g_signal_new ("stopped",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL,
                  NULL,
                  NULL,
                  G_TYPE_NONE,
                  0);
}

gboolean
ide_run_manager_get_busy (IdeRunManager *self)
{
  g_return_val_if_fail (IDE_IS_RUN_MANAGER (self), FALSE);

  return self->busy > 0;
}

static gboolean
ide_run_manager_check_busy (IdeRunManager  *self,
                            GError        **error)
{
  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (error != NULL);

  if (ide_run_manager_get_busy (self))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_BUSY,
                   "%s",
                   _("Cannot run target, another target is running"));
      return TRUE;
    }

  return FALSE;
}

static void
setup_basic_environment (IdeRunContext *run_context)
{
  static const char *copy_env[] = {
    "AT_SPI_BUS_ADDRESS",
    "COLORTERM",
    "DBUS_SESSION_BUS_ADDRESS",
    "DBUS_SYSTEM_BUS_ADDRESS",
    "DESKTOP_SESSION",
    "DISPLAY",
    "LANG",
    "SHELL",
    "SSH_AUTH_SOCK",
    "USER",
    "WAYLAND_DISPLAY",
    "XAUTHORITY",
    "XDG_CURRENT_DESKTOP",
    "XDG_MENU_PREFIX",
#if 0
    /* Can't copy these as they could mess up Flatpak. We might
     * be able to add something to run-context to allow the flatpak
     * plugin to filter them out without affecting others.
     */
    "XDG_DATA_DIRS",
    "XDG_RUNTIME_DIR",
#endif
    "XDG_SEAT",
    "XDG_SESSION_DESKTOP",
    "XDG_SESSION_ID",
    "XDG_SESSION_TYPE",
    "XDG_VTNR",
  };
  const gchar * const *host_environ = _ide_host_environ ();

  for (guint i = 0; i < G_N_ELEMENTS (copy_env); i++)
    {
      const char *key = copy_env[i];
      const char *val = g_environ_getenv ((char **)host_environ, key);

      if (val != NULL)
        ide_run_context_setenv (run_context, key, val);
    }
}

static void
apply_messages_debug (IdeRunContext *run_context,
                      gboolean       messages_debug_all)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_CONTEXT (run_context));

  if (messages_debug_all)
    ide_run_context_setenv (run_context, "G_MESSAGES_DEBUG", "all");

  IDE_EXIT;
}

static void
apply_color_scheme (IdeRunContext *run_context,
                    const char    *color_scheme)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_CONTEXT (run_context));
  g_assert (color_scheme != NULL);

  g_debug ("Applying color-scheme \"%s\"", color_scheme);

  if (ide_str_equal0 (color_scheme, "follow"))
    {
      ide_run_context_unsetenv (run_context, "ADW_DEBUG_COLOR_SCHEME");
      ide_run_context_unsetenv (run_context, "HDY_DEBUG_COLOR_SCHEME");
    }
  else if (ide_str_equal0 (color_scheme, "force-light"))
    {
      ide_run_context_setenv (run_context, "ADW_DEBUG_COLOR_SCHEME", "prefer-light");
      ide_run_context_setenv (run_context, "HDY_DEBUG_COLOR_SCHEME", "prefer-light");
    }
  else if (ide_str_equal0 (color_scheme, "force-dark"))
    {
      ide_run_context_setenv (run_context, "ADW_DEBUG_COLOR_SCHEME", "prefer-dark");
      ide_run_context_setenv (run_context, "HDY_DEBUG_COLOR_SCHEME", "prefer-dark");
    }
  else
    {
      g_warn_if_reached ();
    }

  IDE_EXIT;
}

static void
apply_high_contrast (IdeRunContext *run_context,
                     gboolean       high_contrast)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_CONTEXT (run_context));

  g_debug ("Applying high-contrast %d", high_contrast);

  if (high_contrast)
    {
      ide_run_context_setenv (run_context, "ADW_DEBUG_HIGH_CONTRAST", "1");
      ide_run_context_setenv (run_context, "HDY_DEBUG_HIGH_CONTRAST", "1");
    }
  else
    {
      ide_run_context_unsetenv (run_context, "ADW_DEBUG_HIGH_CONTRAST");
      ide_run_context_unsetenv (run_context, "HDY_DEBUG_HIGH_CONTRAST");
    }

  IDE_EXIT;
}

static void
apply_text_direction (IdeRunContext *run_context,
                      const char    *text_dir_str)
{
  GtkTextDirection dir;

  IDE_ENTRY;

  g_assert (IDE_IS_RUN_CONTEXT (run_context));

  if (ide_str_equal0 (text_dir_str, "rtl"))
    dir = GTK_TEXT_DIR_RTL;
  else if (ide_str_equal0 (text_dir_str, "ltr"))
    dir = GTK_TEXT_DIR_LTR;
  else
    g_return_if_reached ();

  if (dir != gtk_widget_get_default_direction ())
    ide_run_context_setenv (run_context, "GTK_DEBUG", "invert-text-dir");

  IDE_EXIT;
}

static inline const char *
get_action_state_string (IdeRunManager *self,
                         const char    *action_name)
{
  GVariant *state = ide_run_manager_get_action_state (self, action_name);
  return g_variant_get_string (state, NULL);
}

static inline gboolean
get_action_state_bool (IdeRunManager *self,
                       const char    *action_name)
{
  GVariant *state = ide_run_manager_get_action_state (self, action_name);
  return g_variant_get_boolean (state);
}

static void
ide_run_manager_install_cb (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  IdeBuildManager *build_manager = (IdeBuildManager *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(GError) error = NULL;

  IDE_ENTRY;

  g_assert (IDE_IS_BUILD_MANAGER (build_manager));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (IDE_IS_TASK (task));

  if (!ide_build_manager_build_finish (build_manager, result, &error))
    ide_task_return_error (task, g_steal_pointer (&error));
  else
    ide_task_return_boolean (task, TRUE);

  IDE_EXIT;
}

static void
ide_run_manager_install_async (IdeRunManager       *self,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autoptr(IdeContext) context = NULL;
  g_autoptr(GSettings) project_settings = NULL;
  g_autoptr(IdeTask) task = NULL;
  IdeBuildManager *build_manager;
  IdeVcsMonitor *monitor;
  guint64 sequence = 0;

  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));

  context = ide_object_ref_context (IDE_OBJECT (self));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, ide_run_manager_install_async);

  project_settings = ide_context_ref_project_settings (context);
  if (!g_settings_get_boolean (project_settings, "install-before-run"))
    {
      ide_task_return_boolean (task, TRUE);
      IDE_EXIT;
    }

  monitor = ide_vcs_monitor_from_context (context);
  if (monitor != NULL)
    sequence = ide_vcs_monitor_get_sequence (monitor);

  if (self->has_installed_once && sequence == self->last_change_seq)
    {
      ide_task_return_boolean (task, TRUE);
      IDE_EXIT;
    }

  self->pending_last_change_seq = sequence;

  build_manager = ide_build_manager_from_context (context);
  ide_build_manager_build_async (build_manager,
                                 IDE_PIPELINE_PHASE_INSTALL,
                                 NULL,
                                 cancellable,
                                 ide_run_manager_install_cb,
                                 g_steal_pointer (&task));

  IDE_EXIT;
}

static gboolean
ide_run_manager_install_finish (IdeRunManager  *self,
                                GAsyncResult   *result,
                                GError        **error)
{
  gboolean ret;

  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (IDE_IS_TASK (result));

  ret = ide_task_propagate_boolean (IDE_TASK (result), error);

  IDE_RETURN (ret);
}

static void
ide_run_manager_run_subprocess_wait_check_cb (GObject      *object,
                                              GAsyncResult *result,
                                              gpointer      user_data)
{
  IdeSubprocess *subprocess = (IdeSubprocess *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(GError) error = NULL;
  IdeRunManager *self;

  IDE_ENTRY;

  g_assert (IDE_IS_SUBPROCESS (subprocess));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (IDE_IS_TASK (task));

  self = ide_task_get_source_object (task);
  g_assert (IDE_IS_RUN_MANAGER (self));

  if (self->notif != NULL)
    ide_notification_withdraw (self->notif);

  g_clear_object (&self->notif);
  g_clear_object (&self->current_subprocess);

  if (!ide_subprocess_wait_check_finish (subprocess, result, &error))
    ide_task_return_error (task, g_steal_pointer (&error));
  else
    ide_task_return_boolean (task, TRUE);

  g_signal_emit (self, signals[STOPPED], 0);

  IDE_EXIT;
}

static void
ide_run_manager_prepare_run_context (IdeRunManager *self,
                                     IdeRunContext *run_context,
                                     IdeRunCommand *run_command)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (IDE_IS_RUN_CONTEXT (run_context));
  g_assert (IDE_IS_RUN_COMMAND (run_command));

  /* The very first thing we need to do is allow the current run handler
   * to inject any command wrapper it needs. This might be something like
   * gdb, or valgrind, etc.
   */
  if (self->handler && self->handler->handler)
    self->handler->handler (self, run_context, self->handler->handler_data);

  /* First we need to setup our basic runtime envronment so that we can be
   * reasonably certain the application can access the desktop session.
   */
  setup_basic_environment (run_context);

  /* Now push a new layer so that we can keep those values separate from
   * what is configured in the run command. The run-command's environment
   * will override anything set in our layer above.
   */
  ide_run_context_push (run_context, NULL, NULL, NULL);

  /* Setup working directory */
  {
    const char *cwd = ide_run_command_get_cwd (run_command);

    if (cwd != NULL)
      ide_run_context_set_cwd (run_context, cwd);
  }

  /* Setup command arguments */
  {
    const char * const *argv = ide_run_command_get_argv (run_command);

    if (argv != NULL && argv[0] != NULL)
      ide_run_context_append_args (run_context, argv);
  }

  /* Setup command environment */
  {
    const char * const *env = ide_run_command_get_environ (run_command);

    if (env != NULL && env[0] != NULL)
      ide_run_context_add_environ (run_context, env);
  }

  /* Now overlay runtime-tweaks as needed. Put this in a layer so that
   * we can debug where things are set/changed to help us when we need
   * to track down bugs in handlers/runtimes/devices/etc. All of our
   * changes will get persisted to the lower layer when merging anyway.
   *
   * TODO: These could probably be moved into a plugin rather than in
   * the foundry itself. That way they can be disabled by users who are
   * doing nothing with GTK/GNOME applications.
   */
  ide_run_context_push (run_context, NULL, NULL, NULL);
  apply_color_scheme (run_context, get_action_state_string (self, "color-scheme"));
  apply_high_contrast (run_context, get_action_state_bool (self, "high-contrast"));
  apply_text_direction (run_context, get_action_state_string (self, "text-direction"));
  apply_messages_debug (run_context, self->messages_debug_all);

  /* Allow plugins to track anything in the mix. For example the
   * terminal plugin will attach a PTY here for stdin/stdout/stderr.
   */
  g_signal_emit (self, signals [RUN], 0, run_context);

  IDE_EXIT;
}

static void
ide_run_manager_run_deploy_cb (GObject      *object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  IdeDeployStrategy *deploy_strategy = (IdeDeployStrategy *)object;
  g_autoptr(IdeSubprocessLauncher) launcher = NULL;
  g_autoptr(IdeSubprocess) subprocess = NULL;
  g_autoptr(IdeRunContext) run_context = NULL;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(GError) error = NULL;
  IdeNotification *notif;
  IdeRunManager *self;
  IdePipeline *pipeline;

  IDE_ENTRY;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (IDE_IS_DEPLOY_STRATEGY (deploy_strategy));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (IDE_IS_TASK (task));

  self = ide_task_get_source_object (task);
  pipeline = ide_task_get_task_data (task);
  notif = g_object_get_data (G_OBJECT (deploy_strategy), "PROGRESS");

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (IDE_IS_PIPELINE (pipeline));
  g_assert (IDE_IS_NOTIFICATION (notif));

  /* Withdraw our deploy notification */
  ide_notification_withdraw (notif);
  ide_object_destroy (IDE_OBJECT (notif));

  if (!ide_deploy_strategy_deploy_finish (deploy_strategy, result, &error))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      IDE_EXIT;
    }

  if (self->current_run_command == NULL)
    {
      ide_task_return_new_error (task,
                                 G_IO_ERROR,
                                 G_IO_ERROR_CANCELLED,
                                 "The operation was cancelled");
      IDE_EXIT;
    }

  /* Setup the run context */
  run_context = ide_run_context_new ();
  ide_deploy_strategy_prepare_run_context (deploy_strategy, pipeline, run_context);
  ide_run_manager_prepare_run_context (self, run_context, self->current_run_command);

  /* Now setup our launcher and bail if there was a failure */
  if (!(launcher = ide_run_context_end (run_context, &error)))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      IDE_EXIT;
    }

  /* Bail if we couldn't actually launch anything */
  if (!(subprocess = ide_subprocess_launcher_spawn (launcher, NULL, &error)))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      IDE_EXIT;
    }

  if (self->notif != NULL)
    ide_notification_withdraw (self->notif);

  /* Setup notification */
  {
    const char *name = ide_run_command_get_display_name (self->current_run_command);
    /* translators: %s is replaced with the name of the users run command */
    g_autofree char *title = g_strdup_printf (_("Running %s…"), name);

    g_clear_object (&self->notif);
    self->notif = g_object_new (IDE_TYPE_NOTIFICATION,
                                "id", "org.gnome.builder.run-manager.run",
                                "title", title,
                                NULL);
    ide_notification_attach (self->notif, IDE_OBJECT (self));
  }

  /* Wait for the application to finish running */
  ide_subprocess_wait_check_async (subprocess,
                                   ide_task_get_cancellable (task),
                                   ide_run_manager_run_subprocess_wait_check_cb,
                                   g_object_ref (task));

  IDE_EXIT;
}

static void
ide_run_manager_run_discover_run_command_cb (GObject      *object,
                                             GAsyncResult *result,
                                             gpointer      user_data)
{
  IdeRunManager *self = (IdeRunManager *)object;
  g_autoptr(IdeNotification) notif = NULL;
  g_autoptr(IdeRunCommand) run_command = NULL;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(GError) error = NULL;
  IdeDeployStrategy *deploy_strategy;
  GCancellable *cancellable;
  IdePipeline *pipeline;
  IdeContext *context;

  IDE_ENTRY;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (IDE_IS_TASK (task));

  if (!(run_command = ide_run_manager_discover_run_command_finish (self, result, &error)))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      IDE_EXIT;
    }

  g_set_object (&self->current_run_command, run_command);

  cancellable = ide_task_get_cancellable (task);
  g_assert (!cancellable || G_IS_CANCELLABLE (task));

  pipeline = ide_task_get_task_data (task);
  g_assert (IDE_IS_PIPELINE (pipeline));

  context = ide_object_get_context (IDE_OBJECT (pipeline));
  g_assert (IDE_IS_CONTEXT (context));

  deploy_strategy = ide_pipeline_get_deploy_strategy (pipeline);
  g_assert (IDE_IS_DEPLOY_STRATEGY (deploy_strategy));

  notif = g_object_new (IDE_TYPE_NOTIFICATION,
                        "id", "org.gnome.builder.run-manager.deploy",
                        "title", _("Deploying to device…"),
                        "icon-name", "package-x-generic-symbolic",
                        "has-progress", TRUE,
                        "progress-is-imprecise", FALSE,
                        NULL);
  ide_notification_attach (notif, IDE_OBJECT (context));
  g_object_set_data_full (G_OBJECT (deploy_strategy),
                          "PROGRESS",
                          g_object_ref (notif),
                          g_object_unref);

  ide_deploy_strategy_deploy_async (deploy_strategy,
                                    pipeline,
                                    ide_notification_file_progress_callback,
                                    g_object_ref (notif),
                                    g_object_unref,
                                    cancellable,
                                    ide_run_manager_run_deploy_cb,
                                    g_steal_pointer (&task));

  IDE_EXIT;
}

static void
ide_run_manager_run_install_cb (GObject      *object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  IdeRunManager *self = (IdeRunManager *)object;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(GError) error = NULL;

  IDE_ENTRY;

  g_assert (IDE_IS_MAIN_THREAD ());
  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (IDE_IS_TASK (task));

  if (!ide_run_manager_install_finish (self, result, &error))
    ide_task_return_error (task, g_steal_pointer (&error));
  else
    ide_run_manager_discover_run_command_async (self,
                                                ide_task_get_cancellable (task),
                                                ide_run_manager_run_discover_run_command_cb,
                                                g_object_ref (task));

  IDE_EXIT;
}

void
ide_run_manager_run_async (IdeRunManager       *self,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  g_autoptr(IdeTask) task = NULL;
  g_autoptr(GCancellable) local_cancellable = NULL;
  g_autoptr(GError) error = NULL;
  IdeBuildManager *build_manager;
  IdePipeline *pipeline;
  IdeContext *context;

  IDE_ENTRY;

  g_return_if_fail (IDE_IS_MAIN_THREAD ());
  g_return_if_fail (IDE_IS_RUN_MANAGER (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (!g_cancellable_is_cancelled (self->cancellable));

  if (cancellable == NULL)
    cancellable = local_cancellable = g_cancellable_new ();
  ide_cancellable_chain (cancellable, self->cancellable);

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, ide_run_manager_run_async);

  if (ide_task_return_error_if_cancelled (task))
    IDE_EXIT;

  if (ide_run_manager_check_busy (self, &error))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      IDE_EXIT;
    }

  ide_run_manager_mark_busy (self);
  g_signal_connect_object (task,
                           "notify::completed",
                           G_CALLBACK (ide_run_manager_unmark_busy),
                           self,
                           G_CONNECT_SWAPPED);

  context = ide_object_get_context (IDE_OBJECT (self));
  build_manager = ide_build_manager_from_context (context);
  pipeline = ide_build_manager_get_pipeline (build_manager);

  if (pipeline == NULL)
    {
      ide_task_return_new_error (task,
                                 G_IO_ERROR,
                                 G_IO_ERROR_NOT_FOUND,
                                 "A pipeline cannot be found");
      IDE_EXIT;
    }

  ide_task_set_task_data (task, g_object_ref (pipeline), g_object_unref);

  ide_run_manager_install_async (self,
                                 cancellable,
                                 ide_run_manager_run_install_cb,
                                 g_steal_pointer (&task));

  IDE_EXIT;
}

gboolean
ide_run_manager_run_finish (IdeRunManager  *self,
                            GAsyncResult   *result,
                            GError        **error)
{
  gboolean ret;

  IDE_ENTRY;

  g_return_val_if_fail (IDE_IS_RUN_MANAGER (self), FALSE);
  g_return_val_if_fail (IDE_IS_TASK (result), FALSE);

  ret = ide_task_propagate_boolean (IDE_TASK (result), error);

  IDE_RETURN (ret);
}

static gboolean
do_cancel_in_timeout (gpointer user_data)
{
  g_autoptr(GCancellable) cancellable = user_data;

  IDE_ENTRY;

  g_assert (G_IS_CANCELLABLE (cancellable));

  if (!g_cancellable_is_cancelled (cancellable))
    g_cancellable_cancel (cancellable);

  IDE_RETURN (G_SOURCE_REMOVE);
}

void
ide_run_manager_cancel (IdeRunManager *self)
{
  IDE_ENTRY;

  g_return_if_fail (IDE_IS_MAIN_THREAD ());
  g_return_if_fail (IDE_IS_RUN_MANAGER (self));

  /* If the runner is still active, we can just force_exit that instead
   * of cancelling a bunch of in-flight things. This is more useful since
   * it means that we can override the exit signal.
   */
  if (self->current_subprocess != NULL)
    {
      ide_subprocess_force_exit (self->current_subprocess);
      IDE_EXIT;
    }

  /* Make sure tasks are cancelled too */
  if (self->cancellable != NULL)
    g_timeout_add (0, do_cancel_in_timeout, g_steal_pointer (&self->cancellable));
  self->cancellable = g_cancellable_new ();

  IDE_EXIT;
}

void
ide_run_manager_set_handler (IdeRunManager *self,
                             const gchar   *id)
{
  g_return_if_fail (IDE_IS_RUN_MANAGER (self));

  self->handler = NULL;

  for (GList *iter = self->handlers; iter; iter = iter->next)
    {
      const IdeRunHandlerInfo *info = iter->data;

      if (g_strcmp0 (info->id, id) == 0)
        {
          self->handler = info;
          IDE_TRACE_MSG ("run handler set to %s", info->title);
          g_object_notify_by_pspec (G_OBJECT (self), properties [PROP_HANDLER]);
          break;
        }
    }
}

void
ide_run_manager_add_handler (IdeRunManager  *self,
                             const gchar    *id,
                             const gchar    *title,
                             const gchar    *icon_name,
                             IdeRunHandler   run_handler,
                             gpointer        user_data,
                             GDestroyNotify  user_data_destroy)
{
  IdeRunHandlerInfo *info;

  g_return_if_fail (IDE_IS_RUN_MANAGER (self));
  g_return_if_fail (id != NULL);
  g_return_if_fail (title != NULL);

  info = g_slice_new0 (IdeRunHandlerInfo);
  info->id = g_strdup (id);
  info->title = g_strdup (title);
  info->icon_name = g_strdup (icon_name);
  info->handler = run_handler;
  info->handler_data = user_data;
  info->handler_data_destroy = user_data_destroy;

  self->handlers = g_list_append (self->handlers, info);

  if (self->handler == NULL)
    self->handler = info;
}

void
ide_run_manager_remove_handler (IdeRunManager *self,
                                const gchar   *id)
{
  g_return_if_fail (IDE_IS_RUN_MANAGER (self));
  g_return_if_fail (id != NULL);

  for (GList *iter = self->handlers; iter; iter = iter->next)
    {
      IdeRunHandlerInfo *info = iter->data;

      if (g_strcmp0 (info->id, id) == 0)
        {
          self->handlers = g_list_delete_link (self->handlers, iter);

          if (self->handler == info && self->handlers != NULL)
            self->handler = self->handlers->data;
          else
            self->handler = NULL;

          ide_run_handler_info_free (info);

          break;
        }
    }
}

const GList *
_ide_run_manager_get_handlers (IdeRunManager *self)
{
  g_return_val_if_fail (IDE_IS_RUN_MANAGER (self), NULL);

  return self->handlers;
}

const gchar *
ide_run_manager_get_handler (IdeRunManager *self)
{
  g_return_val_if_fail (IDE_IS_RUN_MANAGER (self), NULL);

  if (self->handler != NULL)
    return self->handler->id;

  return NULL;
}

static void
ide_run_manager_run_action_cb (GObject      *object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  IdeRunManager *self = (IdeRunManager *)object;
  IdeContext *context;
  g_autoptr(GError) error = NULL;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (G_IS_ASYNC_RESULT (result));

  context = ide_object_get_context (IDE_OBJECT (self));

  /* Propagate the error to the context */
  if (!ide_run_manager_run_finish (self, result, &error))
    ide_context_warning (context, "%s", error->message);
}

static void
ide_run_manager_actions_run (IdeRunManager *self,
                             GVariant      *param)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));

  ide_run_manager_run_async (self,
                             NULL,
                             ide_run_manager_run_action_cb,
                             NULL);

  IDE_EXIT;
}

static void
ide_run_manager_actions_run_with_handler (IdeRunManager *self,
                                          GVariant      *param)
{
  const gchar *handler = NULL;
  g_autoptr(GVariant) sunk = NULL;

  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));

  if (param != NULL)
  {
    handler = g_variant_get_string (param, NULL);
    if (g_variant_is_floating (param))
      sunk = g_variant_ref_sink (param);
  }

  /* Use specified handler, if provided */
  if (!ide_str_empty0 (handler))
    ide_run_manager_set_handler (self, handler);

  ide_run_manager_run_async (self,
                             NULL,
                             ide_run_manager_run_action_cb,
                             NULL);

  IDE_EXIT;
}

static void
ide_run_manager_actions_stop (IdeRunManager *self,
                              GVariant      *param)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));

  ide_run_manager_cancel (self);

  IDE_EXIT;
}

static void
ide_run_manager_init (IdeRunManager *self)
{
  GtkTextDirection text_dir;

  self->cancellable = g_cancellable_new ();

  /* Setup initial text direction state */
  text_dir = gtk_widget_get_default_direction ();
  if (text_dir == GTK_TEXT_DIR_LTR)
    ide_run_manager_set_action_state (self,
                                      "text-direction",
                                      text_dir == GTK_TEXT_DIR_LTR ?
                                        g_variant_new_string ("ltr") :
                                        g_variant_new_string ("rtl"));

  ide_run_manager_add_handler (self,
                               "run",
                               _("Run"),
                               "builder-run-start-symbolic",
                               NULL,
                               NULL,
                               NULL);
}

void
_ide_run_manager_drop_caches (IdeRunManager *self)
{
  g_return_if_fail (IDE_IS_RUN_MANAGER (self));

  self->last_change_seq = 0;
}

static void
ide_run_manager_actions_messages_debug_all (IdeRunManager *self,
                                            GVariant      *param)
{
  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));

  self->messages_debug_all = !self->messages_debug_all;
  ide_run_manager_set_action_state (self,
                                    "messages-debug-all",
                                    g_variant_new_boolean (self->messages_debug_all));

  IDE_EXIT;
}

typedef struct
{
  GString *errors;
  GListStore *store;
  int n_active;
} ListCommands;

static void
list_commands_free (ListCommands *state)
{
  g_assert (state != NULL);
  g_assert (state->n_active == 0);

  g_string_free (state->errors, TRUE);
  state->errors = NULL;
  g_clear_object (&state->store);
  g_slice_free (ListCommands, state);
}

static void
ide_run_manager_list_commands_cb (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  IdeRunCommandProvider *provider = (IdeRunCommandProvider *)object;
  g_autoptr(GListModel) model = NULL;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(GError) error = NULL;
  ListCommands *state;

  g_assert (IDE_IS_RUN_COMMAND_PROVIDER (provider));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (IDE_IS_TASK (task));

  state = ide_task_get_task_data (task);

  g_assert (state != NULL);
  g_assert (state->n_active > 0);
  g_assert (G_IS_LIST_STORE (state->store));

  if (!(model = ide_run_command_provider_list_commands_finish (provider, result, &error)))
    {
      if (!ide_error_ignore (error))
        {
          if (state->errors->len > 0)
            g_string_append (state->errors, "; ");
          g_string_append (state->errors, error->message);
        }
    }
  else
    {
      g_list_store_append (state->store, model);
    }

  state->n_active--;

  if (state->n_active == 0)
    {
      if (state->errors->len > 0)
        ide_task_return_new_error (task,
                                   G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "%s",
                                   state->errors->str);
      else
        ide_task_return_pointer (task,
                                 gtk_flatten_list_model_new (G_LIST_MODEL (g_steal_pointer (&state->store))),
                                 g_object_unref);
    }
}

static void
ide_run_manager_list_commands_foreach_cb (IdeExtensionSetAdapter *set,
                                          PeasPluginInfo         *plugin_info,
                                          PeasExtension          *exten,
                                          gpointer                user_data)
{
  IdeRunCommandProvider *provider = (IdeRunCommandProvider *)exten;
  IdeTask *task = user_data;
  ListCommands *state;

  g_assert (IDE_IS_EXTENSION_SET_ADAPTER (set));
  g_assert (plugin_info != NULL);
  g_assert (IDE_IS_RUN_COMMAND_PROVIDER (provider));
  g_assert (IDE_IS_TASK (task));

  state = ide_task_get_task_data (task);
  state->n_active++;

  ide_run_command_provider_list_commands_async (provider,
                                                ide_task_get_cancellable (task),
                                                ide_run_manager_list_commands_cb,
                                                g_object_ref (task));
}

void
ide_run_manager_list_commands_async (IdeRunManager       *self,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  g_autoptr(IdeTask) task = NULL;
  ListCommands *state;

  IDE_ENTRY;

  g_return_if_fail (IDE_IS_RUN_MANAGER (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  state = g_slice_new0 (ListCommands);
  state->store = g_list_store_new (G_TYPE_LIST_MODEL);
  state->errors = g_string_new (NULL);

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, ide_run_manager_list_commands_async);
  ide_task_set_task_data (task, state, list_commands_free);

  if (self->run_command_providers)
    ide_extension_set_adapter_foreach (self->run_command_providers,
                                       ide_run_manager_list_commands_foreach_cb,
                                       task);

  if (state->n_active == 0)
    ide_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_NOT_SUPPORTED,
                               "No run command providers available");

  IDE_EXIT;
}

/**
 * ide_run_manager_list_commands_finish:
 *
 * Returns: (transfer full): a #GListModel of #IdeRunCommand
 */
GListModel *
ide_run_manager_list_commands_finish (IdeRunManager  *self,
                                      GAsyncResult   *result,
                                      GError        **error)
{
  g_return_val_if_fail (IDE_IS_RUN_MANAGER (self), NULL);
  g_return_val_if_fail (IDE_IS_TASK (result), NULL);

  return ide_task_propagate_pointer (IDE_TASK (result), error);
}

static void
ide_run_manager_discover_run_command_cb (GObject      *object,
                                         GAsyncResult *result,
                                         gpointer      user_data)
{
  IdeRunManager *self = (IdeRunManager *)object;
  g_autoptr(IdeRunCommand) best = NULL;
  g_autoptr(GListModel) model = NULL;
  g_autoptr(IdeTask) task = user_data;
  g_autoptr(GError) error = NULL;
  const char *default_id;
  guint n_items;
  int best_priority = G_MAXINT;

  IDE_ENTRY;

  g_assert (IDE_IS_RUN_MANAGER (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (IDE_IS_TASK (task));

  if (!(model = ide_run_manager_list_commands_finish (self, result, &error)))
    {
      ide_task_return_error (task, g_steal_pointer (&error));
      IDE_EXIT;
    }

  default_id = ide_task_get_task_data (task);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(IdeRunCommand) run_command = g_list_model_get_item (model, i);
      const char *id;
      int priority;

      g_assert (IDE_IS_RUN_COMMAND (run_command));

      id = ide_run_command_get_id (run_command);
      priority = ide_run_command_get_priority (run_command);

      if (!ide_str_empty0 (id) &&
          !ide_str_empty0 (default_id) &&
          strcmp (default_id, id) == 0)
        {
          ide_task_return_pointer (task,
                                   g_steal_pointer (&run_command),
                                   g_object_unref);
          IDE_EXIT;
        }

      if (best == NULL || priority < best_priority)
        {
          g_set_object (&best, run_command);
          best_priority = priority;
        }
    }

  if (best != NULL)
    ide_task_return_pointer (task,
                             g_steal_pointer (&best),
                             g_object_unref);
  else
    ide_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_NOT_FOUND,
                               "No run command discovered. Set one manually.");

  IDE_EXIT;
}

void
ide_run_manager_discover_run_command_async (IdeRunManager       *self,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data)
{
  g_autoptr(IdeTask) task = NULL;

  IDE_ENTRY;

  g_return_if_fail (IDE_IS_RUN_MANAGER (self));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = ide_task_new (self, cancellable, callback, user_data);
  ide_task_set_source_tag (task, ide_run_manager_discover_run_command_async);
  ide_task_set_task_data (task, g_strdup (self->default_run_command), g_free);

  ide_run_manager_list_commands_async (self,
                                       cancellable,
                                       ide_run_manager_discover_run_command_cb,
                                       g_steal_pointer (&task));

  IDE_EXIT;
}

/**
 * ide_run_manager_discover_run_command_finish:
 * @self: a #IdeRunManager
 *
 * Complete request to discover the default run command.
 *
 * Returns: (transfer full): an #IdeRunCommand if successful; otherwise
 *   %NULL and @error is set.
 */
IdeRunCommand *
ide_run_manager_discover_run_command_finish (IdeRunManager  *self,
                                             GAsyncResult   *result,
                                             GError        **error)
{
  IdeRunCommand *run_command;

  IDE_ENTRY;

  g_return_val_if_fail (IDE_IS_RUN_MANAGER (self), NULL);
  g_return_val_if_fail (IDE_IS_TASK (result), NULL);

  run_command = ide_task_propagate_pointer (IDE_TASK (result), error);

  g_return_val_if_fail (!run_command || IDE_IS_RUN_COMMAND (run_command), NULL);

  IDE_RETURN (run_command);
}
