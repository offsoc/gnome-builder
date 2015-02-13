/* ide-indent-style.h
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef IDE_INDENT_STYLE_H
#define IDE_INDENT_STYLE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define IDE_TYPE_INDENT_STYLE (ide_indent_style_get_type())

typedef enum
{
  IDE_INDENT_STYLE_NONE            = 0,
  IDE_INDENT_STYLE_TABS            = 1,
  IDE_INDENT_STYLE_SPACES          = 2,
  IDE_INDENT_STYLE_TABS_AND_SPACES = 3,
} IdeIndentStyle;

GType ide_indent_style_get_type (void);

G_END_DECLS

#endif /* IDE_INDENT_STYLE_H */
