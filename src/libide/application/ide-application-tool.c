/* ide-application-tool.c
 *
 * Copyright © 2015 Christian Hergert <christian@hergert.me>
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
 */

#include "application/ide-application-tool.h"

G_DEFINE_INTERFACE (IdeApplicationTool, ide_application_tool, G_TYPE_OBJECT)

static void
ide_application_tool_default_init (IdeApplicationToolInterface *iface)
{
}

/**
 * ide_application_tool_run_async:
 * @self: An #IdeApplicationTool
 * @arguments: (array zero-terminated=1) (element-type utf8): argv for the command
 * @cancellable: (nullable): A #GCancellable or %NULL
 * @callback: A callback to execute upon completion
 * @user_data: User data for @callback
 *
 * Asynchronously runs an application tool. This is typically done on the
 * command line using the `ide` command.
 */
void
ide_application_tool_run_async (IdeApplicationTool  *self,
                                const gchar * const *arguments,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_return_if_fail (IDE_IS_APPLICATION_TOOL (self));
  g_return_if_fail (arguments != NULL);
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  IDE_APPLICATION_TOOL_GET_IFACE (self)->run_async (self, arguments, cancellable, callback, user_data);
}

gint
ide_application_tool_run_finish (IdeApplicationTool  *self,
                                 GAsyncResult        *result,
                                 GError             **error)
{
  g_return_val_if_fail (IDE_IS_APPLICATION_TOOL (self), 0);
  g_return_val_if_fail (G_IS_ASYNC_RESULT (result), 0);

  return IDE_APPLICATION_TOOL_GET_IFACE (self)->run_finish (self, result, error);
}
