/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 2016-2022 ircd-hybrid development team
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

/*! \file m_svsaccount.c
 * \brief Includes required functions for processing the SVSACCOUNT command.
 * \version $Id$
 */

#include "stdinc.h"
#include "client.h"
#include "ircd.h"
#include "send.h"
#include "parse.h"
#include "modules.h"
#include "irc_string.h"


/*! \brief SVSACCOUNT command handler
 *
 * \param source_p Pointer to allocated Client struct from which the message
 *                 originally comes from.  This can be a local or remote client.
 * \param parc     Integer holding the number of supplied arguments.
 * \param parv     Argument vector where parv[0] .. parv[parc-1] are non-NULL
 *                 pointers.
 * \note Valid arguments for this command are:
 *      - parv[0] = command
 *      - parv[1] = nickname
 *      - parv[2] = TS
 *      - parv[3] = account name
 */
static void
ms_svsaccount(struct Client *source_p, int parc, char *parv[])
{
  if (!HasFlag(source_p, FLAGS_SERVICE))
    return;

  struct Client *target_p = find_person(source_p, parv[1]);
  if (target_p == NULL)
    return;

  uintmax_t ts = strtoumax(parv[2], NULL, 10);
  if (ts && (ts != target_p->tsinfo))
    return;

  strlcpy(target_p->account, parv[3], sizeof(target_p->account));
  sendto_common_channels_local(target_p, true, CAP_ACCOUNT_NOTIFY, 0, ":%s!%s@%s ACCOUNT %s",
                               target_p->name, target_p->username,
                               target_p->host, target_p->account);
  sendto_server(source_p, 0, 0, ":%s SVSACCOUNT %s %ju %s",
                source_p->id,
                target_p->id, target_p->tsinfo, target_p->account);
}

static struct Message svsaccount_msgtab =
{
  .cmd = "SVSACCOUNT",
  .handlers[UNREGISTERED_HANDLER] = { .handler = m_unregistered },
  .handlers[CLIENT_HANDLER] = { .handler = m_ignore },
  .handlers[SERVER_HANDLER] = { .handler = ms_svsaccount, .args_min = 4 },
  .handlers[ENCAP_HANDLER] = { .handler = m_ignore },
  .handlers[OPER_HANDLER] = { .handler = m_ignore }
};

static void
module_init(void)
{
  mod_add_cmd(&svsaccount_msgtab);
}

static void
module_exit(void)
{
  mod_del_cmd(&svsaccount_msgtab);
}

struct module module_entry =
{
  .version = "$Revision$",
  .modinit = module_init,
  .modexit = module_exit,
};
