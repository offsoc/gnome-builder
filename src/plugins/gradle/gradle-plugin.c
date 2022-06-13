/* gradle-plugin.c
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

#define G_LOG_DOMAIN "gradle-plugin"

#include "config.h"

#include <libpeas/peas.h>

#include <libide-foundry.h>

#include "gbp-gradle-build-system.h"
#include "gbp-gradle-build-system-discovery.h"
#include "gbp-gradle-pipeline-addin.h"
#include "gbp-gradle-run-command-provider.h"
#include "gbp-gradle-test-provider.h"

_IDE_EXTERN void
_gbp_gradle_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module,
                                              IDE_TYPE_BUILD_SYSTEM,
                                              GBP_TYPE_GRADLE_BUILD_SYSTEM);
  peas_object_module_register_extension_type (module,
                                              IDE_TYPE_BUILD_SYSTEM_DISCOVERY,
                                              GBP_TYPE_GRADLE_BUILD_SYSTEM_DISCOVERY);
  peas_object_module_register_extension_type (module,
                                              IDE_TYPE_PIPELINE_ADDIN,
                                              GBP_TYPE_GRADLE_PIPELINE_ADDIN);
  peas_object_module_register_extension_type (module,
                                              IDE_TYPE_RUN_COMMAND_PROVIDER,
                                              GBP_TYPE_GRADLE_RUN_COMMAND_PROVIDER);
  peas_object_module_register_extension_type (module,
                                              IDE_TYPE_TEST_PROVIDER,
                                              GBP_TYPE_GRADLE_TEST_PROVIDER);
}