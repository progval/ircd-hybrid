/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 2000-2024 ircd-hybrid development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 *  USA
 */

/*! \file m_module.c
 * \brief Includes required functions for processing the MODULE command.
 */

#include "stdinc.h"
#include "list.h"
#include "client.h"
#include "irc_string.h"
#include "ircd.h"
#include "numeric.h"
#include "log.h"
#include "send.h"
#include "parse.h"
#include "module.h"

/**
 * @brief Command to load a module.
 *
 * This function handles the load command from a client, loading the specified module.
 *
 * @param source Client issuing the command.
 * @param arg Module name to load.
 */
static void
module_cmd_load(struct Client *source, const char *arg)
{
  if (module_load(arg, true, false))
  {
    const struct Module *const module = module_find(arg);
    sendto_one_notice(source, &me, ":Module %s [handle: %p] loaded.", module->name, module->handle);
  }
  else
    sendto_one_notice(source, &me, ":Failed to load module %s: %s", arg, module_get_error());
}

/**
 * @brief Command to unload a module.
 *
 * This function handles the unload command from a client, unloading the specified module.
 *
 * @param source Client issuing the command.
 * @param arg Module name to unload.
 */
static void
module_cmd_unload(struct Client *source, const char *arg)
{
  const struct Module *const module = module_find(arg);
  if (module == NULL)
  {
    sendto_one_notice(source, &me, ":Module %s is not loaded", arg);
    return;
  }

  if (module->core)
  {
    sendto_one_notice(source, &me, ":Module %s is a core module and may not be unloaded", arg);
    return;
  }

  if (module->resident)
  {
    sendto_one_notice(source, &me, ":Module %s is a resident module and may not be unloaded", arg);
    return;
  }

  if (module_unload(arg, true))
    sendto_one_notice(source, &me, ":Module %s unloaded successfully", arg);
  else
    sendto_one_notice(source, &me, ":Failed to unload module %s: %s", arg, module_get_error());
}

/**
 * @brief Command to reload a single module.
 *
 * This function handles the reload command from a client, reloading the specified module.
 *
 * @param source Client issuing the command.
 * @param arg Module name to reload.
 */
static void
module_cmd_reload_single(struct Client *source, const char *arg)
{
  const struct Module *module = module_find(arg);
  if (module == NULL)
  {
    sendto_one_notice(source, &me, ":Module %s is not loaded", arg);
    return;
  }

  if (module->resident)
  {
    sendto_one_notice(source, &me, ":Module %s is a resident module and may not be unloaded", arg);
    return;
  }

  const bool core = module->core;

  if (module_unload(arg, true) == 0 || module_load(arg, true, false) == 0)
  {
    sendto_one_notice(source, &me, ":Failed to reload module %s: %s", arg, module_get_error());

    if (core)
    {
      sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                           "Error reloading core module: %s: terminating ircd", arg);
      log_write(LOG_TYPE_IRCD, "Error loading core module %s: terminating ircd", arg);
      exit(EXIT_FAILURE);
    }

    return;
  }

  module = module_find(arg);
  sendto_one_notice(source, &me, ":Module %s [handle: %p] loaded.",
                    module->name, module->handle);
}

/**
 * @brief Command to reload all modules.
 *
 * This function handles the reload command from a client, reloading all modules.
 *
 * @param source Client issuing the command.
 */
static void
module_cmd_reload_all(struct Client *source)
{
  unsigned int unloaded_count = 0, loaded_count = 0;
  bool success = module_reload_all(&unloaded_count, &loaded_count, true, false);

  if (success)
    sendto_one_notice(source, &me, ":All modules reloaded successfully");
  else
    sendto_one_notice(source, &me, ":Module reload encountered issues: %s", module_get_error());

  sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                       "Module Reload: %u modules unloaded, %u modules loaded", unloaded_count, loaded_count);
  log_write(LOG_TYPE_IRCD, "Module Reload: %u modules unloaded, %u modules loaded", unloaded_count, loaded_count);

  list_node_t *node;
  LIST_FOREACH(node, module_get_list()->head)
  {
    const struct Module *const module = node->data;
    if (module->core && module_find(module->name) == NULL)
    {
      sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                           "Error reloading core modules: terminating ircd");
      log_write(LOG_TYPE_IRCD, "Error reloading core modules: terminating ircd");
      exit(EXIT_FAILURE);
    }
  }
}

/**
 * @brief Command to reload modules.
 *
 * This function handles the reload command from a client, reloading the specified module or all modules if the argument is "*".
 *
 * @param source Client issuing the command.
 * @param arg Module name to reload, or "*" to reload all modules.
 */
static void
module_cmd_reload(struct Client *source, const char *arg)
{
  if (strcmp(arg, "*") == 0)
    module_cmd_reload_all(source);
  else
    module_cmd_reload_single(source, arg);
}

/**
 * @brief Command to list loaded modules.
 *
 * This function handles the list command from a client, listing all loaded modules.
 *
 * @param source Client issuing the command.
 * @param arg Optional argument to filter modules.
 */
static void
module_cmd_list(struct Client *source, const char *arg)
{
  list_node_t *node;
  LIST_FOREACH(node, module_get_list()->head)
  {
    const struct Module *const module = node->data;
    if (!EmptyString(arg) && match(arg, module->name))
      continue;

    sendto_one_numeric(source, &me, RPL_MODLIST,
                       module->name, module->handle, "*", module_get_attributes(module));
  }

  sendto_one_numeric(source, &me, RPL_ENDOFMODLIST);
}

/**
 * @brief Module command table structure.
 */
struct ModuleStruct
{
  const char *cmd;
  void (*handler)(struct Client *, const char *);
  bool arg_required;
};

/**
 * @brief Module command table.
 */
static const struct ModuleStruct module_cmd_table[] =
{
  { .cmd = "LOAD", .handler = module_cmd_load, .arg_required = true  },
  { .cmd = "UNLOAD", .handler = module_cmd_unload, .arg_required = true  },
  { .cmd = "RELOAD", .handler = module_cmd_reload, .arg_required = true  },
  { .cmd = "LIST", .handler = module_cmd_list, .arg_required = false },
  { .cmd = NULL }
};

/*! \brief MODULE command handler
 *
 * \param source Pointer to allocated Client struct from which the message
 *                 originally comes from.  This can be a local or remote client.
 * \param parc     Integer holding the number of supplied arguments.
 * \param parv     Argument vector where parv[0] .. parv[parc-1] are non-NULL
 *                 pointers.
 * \note Valid arguments for this command are:
 *      - parv[0] = command
 *      - parv[1] = MODULE subcommand [LOAD, UNLOAD, RELOAD, LIST]
 *      - parv[2] = module name
 */
static void
mo_module(struct Client *source, int parc, char *parv[])
{
  const char *const subcmd = parv[1];
  const char *const module = parv[2];

  if (!HasOFlag(source, OPER_FLAG_MODULE))
  {
    sendto_one_numeric(source, &me, ERR_NOPRIVS, "module");
    return;
  }

  for (const struct ModuleStruct *tab = module_cmd_table; tab->handler; ++tab)
  {
    if (irccmp(tab->cmd, subcmd))
      continue;

    if (tab->arg_required && EmptyString(module))
    {
      sendto_one_numeric(source, &me, ERR_NEEDMOREPARAMS, "MODULE");
      return;
    }

    tab->handler(source, module);
    return;
  }

  sendto_one_notice(source, &me, ":%s is not a valid option. Choose from LOAD, UNLOAD, RELOAD, LIST",
                    subcmd);
}

static struct Command module_msgtab =
{
  .name = "MODULE",
  .handlers[UNREGISTERED_HANDLER] = { .handler = m_unregistered },
  .handlers[CLIENT_HANDLER] = { .handler = m_not_oper },
  .handlers[SERVER_HANDLER] = { .handler = m_ignore },
  .handlers[ENCAP_HANDLER] = { .handler = m_ignore },
  .handlers[OPER_HANDLER] = { .handler = mo_module, .args_min = 2 }
};

static void
init_handler(void)
{
  command_add(&module_msgtab);
}

static void
exit_handler(void)
{
  command_del(&module_msgtab);
}

struct Module module_entry =
{
  .init_handler = init_handler,
  .exit_handler = exit_handler,
  .resident = true
};
