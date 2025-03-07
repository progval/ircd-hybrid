/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 1997-2022 ircd-hybrid development team
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

/*! \file m_pong.c
 * \brief Includes required functions for processing the PONG command.
 * \version $Id$
 */

#include "stdinc.h"
#include "ircd.h"
#include "user.h"
#include "client.h"
#include "hash.h"
#include "numeric.h"
#include "conf.h"
#include "send.h"
#include "irc_string.h"
#include "parse.h"
#include "modules.h"
#include "server.h"


/*! \brief PONG command handler
 *
 * \param source_p Pointer to allocated Client struct from which the message
 *                 originally comes from.  This can be a local or remote client.
 * \param parc     Integer holding the number of supplied arguments.
 * \param parv     Argument vector where parv[0] .. parv[parc-1] are non-NULL
 *                 pointers.
 * \note Valid arguments for this command are:
 *      - parv[0] = command
 *      - parv[1] = origin
 *      - parv[2] = destination
 */
static void
ms_pong(struct Client *source_p, int parc, char *parv[])
{
  if (EmptyString(parv[1]))
  {
    sendto_one_numeric(source_p, &me, ERR_NOORIGIN);
    return;
  }

  const char *const destination = parv[2];
  if (!EmptyString(destination))
  {
    struct Client *target_p;
    if ((target_p = hash_find_client(destination)) ||
        (target_p = hash_find_id(destination)))
    {
      if (!IsMe(target_p) && target_p->from != source_p->from)
        sendto_one(target_p, ":%s PONG %s %s",
                   ID_or_name(source_p, target_p), parv[1],
                   ID_or_name(target_p, target_p));
    }
    else if (!IsDigit(*destination))
      sendto_one_numeric(source_p, &me, ERR_NOSUCHSERVER, destination);
  }
}

/*! \brief PONG command handler
 *
 * \param source_p Pointer to allocated Client struct from which the message
 *                 originally comes from.  This can be a local or remote client.
 * \param parc     Integer holding the number of supplied arguments.
 * \param parv     Argument vector where parv[0] .. parv[parc-1] are non-NULL
 *                 pointers.
 * \note Valid arguments for this command are:
 *      - parv[0] = command
 *      - parv[1] = origin/ping cookie
 */
static void
mr_pong(struct Client *source_p, int parc, char *parv[])
{
  assert(MyConnect(source_p));

  if (parc == 2 && !EmptyString(parv[1]))
  {
    if (ConfigGeneral.ping_cookie && source_p->connection->random_ping)
    {
      unsigned int incoming_ping = strtoul(parv[1], NULL, 10);

      if (source_p->connection->random_ping == incoming_ping)
      {
        AddFlag(source_p, FLAGS_PING_COOKIE);

        if (source_p->connection->registration == 0)
          register_local_user(source_p);
      }
      else
        sendto_one_numeric(source_p, &me, ERR_WRONGPONG,
                           source_p->connection->random_ping);
    }
  }
  else
    sendto_one_numeric(source_p, &me, ERR_NOORIGIN);
}

static struct Message pong_msgtab =
{
  .cmd = "PONG",
  .handlers[UNREGISTERED_HANDLER] = { .handler = mr_pong },
  .handlers[CLIENT_HANDLER] = { .handler = m_ignore },
  .handlers[SERVER_HANDLER] = { .handler = ms_pong },
  .handlers[ENCAP_HANDLER] = { .handler = m_ignore },
  .handlers[OPER_HANDLER] = { .handler = m_ignore }
};

static void
module_init(void)
{
  mod_add_cmd(&pong_msgtab);
}

static void
module_exit(void)
{
  mod_del_cmd(&pong_msgtab);
}

struct module module_entry =
{
  .version = "$Revision$",
  .modinit = module_init,
  .modexit = module_exit,
};
