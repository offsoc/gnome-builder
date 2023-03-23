/* ide-lsp-plugin-hover-provider.c
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

#define G_LOG_DOMAIN "ide-lsp-plugin-hover-provider"

#include "config.h"

#include <libpeas.h>

#include <libide-code.h>

#include "ide-lsp-hover-provider.h"
#include "ide-lsp-plugin-private.h"
#include "ide-lsp-service.h"

typedef struct _IdeLspPluginHoverProviderClass
{
  IdeLspHoverProviderClass  parent_class;
  IdeLspPluginInfo              *info;
} IdeLspPluginHoverProviderClass;

static void
ide_lsp_plugin_hover_provider_prepare (IdeLspHoverProvider *provider)
{
  IdeLspPluginHoverProviderClass *klass = (IdeLspPluginHoverProviderClass *)(((GTypeInstance *)provider)->g_class);
  g_autoptr(IdeLspServiceClass) service_class = g_type_class_ref (klass->info->service_type);

  ide_lsp_service_class_bind_client (service_class, IDE_OBJECT (provider));
}

static void
ide_lsp_plugin_hover_provider_class_init (IdeLspPluginHoverProviderClass *klass,
                                          IdeLspPluginInfo               *info)
{
  IDE_LSP_HOVER_PROVIDER_CLASS (klass)->prepare = ide_lsp_plugin_hover_provider_prepare;
  klass->info = info;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
GObject *
ide_lsp_plugin_create_hover_provider (guint             n_parameters,
                                       GParameter       *parameters,
                                       IdeLspPluginInfo *info)
{
  ide_lsp_plugin_remove_plugin_info_param (&n_parameters, parameters);

  if G_UNLIKELY (info->hover_provider_type == G_TYPE_INVALID)
    {
      g_autofree char *name = g_strconcat (info->module_name, "+HoverProvider", NULL);

      info->hover_provider_type =
        g_type_register_static (IDE_TYPE_LSP_HOVER_PROVIDER,
                                name,
                                &(GTypeInfo) {
                                  sizeof (IdeLspPluginHoverProviderClass),
                                  NULL,
                                  NULL,
                                  (GClassInitFunc)ide_lsp_plugin_hover_provider_class_init,
                                  NULL,
                                  ide_lsp_plugin_info_ref (info),
                                  sizeof (IdeLspHoverProvider),
                                  0,
                                  NULL,
                                  NULL,
                                },
                                G_TYPE_FLAG_FINAL);
    }

  return g_object_newv (info->hover_provider_type, n_parameters, parameters);
}
G_GNUC_END_IGNORE_DEPRECATIONS
