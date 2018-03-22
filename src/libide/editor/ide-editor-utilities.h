/* ide-editor-utilities.h
 *
 * Copyright 2017 Christian Hergert <chergert@redhat.com>
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

#pragma once

#include "ide-version-macros.h"

#include "layout/ide-layout-pane.h"

G_BEGIN_DECLS

#define IDE_TYPE_EDITOR_UTILITIES (ide_editor_utilities_get_type())

IDE_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (IdeEditorUtilities, ide_editor_utilities, IDE, EDITOR_UTILITIES, IdeLayoutPane)

/* Use GtkContainer api to add your DzlDockWidget */

G_END_DECLS
