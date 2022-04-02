/* ide-source-view.h
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

#include <libide-core.h>

#include <gtksourceview/gtksource.h>

G_BEGIN_DECLS

#define IDE_TYPE_SOURCE_VIEW (ide_source_view_get_type())

IDE_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (IdeSourceView, ide_source_view, IDE, SOURCE_VIEW, GtkSourceView)

IDE_AVAILABLE_IN_ALL
GtkWidget *ide_source_view_new                 (void);
IDE_AVAILABLE_IN_ALL
void       ide_source_view_scroll_to_insert    (IdeSourceView *self);
IDE_AVAILABLE_IN_ALL
char      *ide_source_view_dup_position_label  (IdeSourceView *self);
IDE_AVAILABLE_IN_ALL
void       ide_source_view_get_visual_position (IdeSourceView *self,
                                                guint         *line,
                                                guint         *line_column);

G_END_DECLS
