/* ide-run-command.h
 *
 * Copyright 2022 Christian Hergert <chergert@redhat.com>
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

#pragma once

#if !defined (IDE_FOUNDRY_INSIDE) && !defined (IDE_FOUNDRY_COMPILATION)
# error "Only <libide-foundry.h> can be included directly."
#endif

#include <libide-core.h>

G_BEGIN_DECLS

#define IDE_TYPE_RUN_COMMAND (ide_run_command_get_type())

IDE_AVAILABLE_IN_ALL
G_DECLARE_DERIVABLE_TYPE (IdeRunCommand, ide_run_command, IDE, RUN_COMMAND, GObject)

struct _IdeRunCommandClass
{
  GObjectClass parent_class;
};

IDE_AVAILABLE_IN_ALL
const char         *ide_run_command_get_id           (IdeRunCommand      *self);
IDE_AVAILABLE_IN_ALL
void                ide_run_command_set_id           (IdeRunCommand      *self,
                                                      const char         *id);
IDE_AVAILABLE_IN_ALL
const char         *ide_run_command_get_display_name (IdeRunCommand      *self);
IDE_AVAILABLE_IN_ALL
void                ide_run_command_set_display_name (IdeRunCommand      *self,
                                                      const char         *display_name);
IDE_AVAILABLE_IN_ALL
const char * const *ide_run_command_get_argv         (IdeRunCommand      *self);
IDE_AVAILABLE_IN_ALL
void                ide_run_command_set_argv         (IdeRunCommand      *self,
                                                      const char * const *argv);
IDE_AVAILABLE_IN_ALL
const char * const *ide_run_command_get_env          (IdeRunCommand      *self);
IDE_AVAILABLE_IN_ALL
void                ide_run_command_set_env          (IdeRunCommand      *self,
                                                      const char * const *env);
IDE_AVAILABLE_IN_ALL
int                 ide_run_command_get_priority     (IdeRunCommand      *self);
IDE_AVAILABLE_IN_ALL
void                ide_run_command_set_priority     (IdeRunCommand      *self,
                                                      int                 priority);

G_END_DECLS