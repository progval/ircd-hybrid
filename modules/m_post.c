/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 2001-2022 ircd-hybrid development team
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

/*! \file m_post.c
 * \brief Includes required functions for processing the POST/GET/PUT command.
 * \version $Id$
 */

#include "stdinc.h"
#include "client.h"
#include "ircd.h"
#include "send.h"
#include "parse.h"
#include "modules.h"


/*! \brief GET/POST/PUT command handler
 *
 * \param source_p Pointer to allocated Client struct from which the message
 *                 originally comes from.  This can be a local or remote client.
 * \param parc     Integer holding the number of supplied arguments.
 * \param parv     Argument vector where parv[0] .. parv[parc-1] are non-NULL
 *                 pointers.
 * \note Valid arguments for this command are:
 *      - parv[0] = command
 */
static void
mr_dumb_proxy(struct Client *source_p, int parc, char *parv[])
{
  sendto_realops_flags(UMODE_REJ, L_ALL, SEND_NOTICE,
                       "HTTP Proxy disconnected: [%s@%s]",
                       source_p->username, source_p->host);
  exit_client(source_p, "Client Exit");
}

static struct Message post_msgtab =
{
  .cmd = "POST",
  .handlers[UNREGISTERED_HANDLER] = { .handler = mr_dumb_proxy },
  .handlers[CLIENT_HANDLER] = { .handler = m_ignore },
  .handlers[SERVER_HANDLER] = { .handler = m_ignore },
  .handlers[ENCAP_HANDLER] = { .handler = m_ignore },
  .handlers[OPER_HANDLER] = { .handler = m_ignore }
};

static struct Message get_msgtab =
{
  .cmd = "GET",
  .handlers[UNREGISTERED_HANDLER] = { .handler = mr_dumb_proxy },
  .handlers[CLIENT_HANDLER] = { .handler = m_ignore },
  .handlers[SERVER_HANDLER] = { .handler = m_ignore },
  .handlers[ENCAP_HANDLER] = { .handler = m_ignore },
  .handlers[OPER_HANDLER] = { .handler = m_ignore }
};

static struct Message put_msgtab =
{
  .cmd = "PUT",
  .handlers[UNREGISTERED_HANDLER] = { .handler = mr_dumb_proxy },
  .handlers[CLIENT_HANDLER] = { .handler = m_ignore },
  .handlers[SERVER_HANDLER] = { .handler = m_ignore },
  .handlers[ENCAP_HANDLER] = { .handler = m_ignore },
  .handlers[OPER_HANDLER] = { .handler = m_ignore }
};

static void
module_init(void)
{
  mod_add_cmd(&post_msgtab);
  mod_add_cmd(&get_msgtab);
  mod_add_cmd(&put_msgtab);
}

static void
module_exit(void)
{
  mod_del_cmd(&post_msgtab);
  mod_del_cmd(&get_msgtab);
  mod_del_cmd(&put_msgtab);
}

struct module module_entry =
{
  .version = "$Revision$",
  .modinit = module_init,
  .modexit = module_exit,
};
