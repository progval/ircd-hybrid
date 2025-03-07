/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 1998-2022 ircd-hybrid development team
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

/*! \file conf_gecos.c
 * \brief Implements gecos {} block configuration management.
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "send.h"
#include "client.h"
#include "ircd.h"
#include "memory.h"
#include "conf.h"
#include "conf_gecos.h"


static dlink_list gecos_list;


const dlink_list *
gecos_get_list(void)
{
  return &gecos_list;
}

void
gecos_clear(void)
{
  dlink_node *node, *node_next;

  DLINK_FOREACH_SAFE(node, node_next, gecos_list.head)
  {
    struct GecosItem *gecos = node->data;

    if (gecos->in_database == false)
      gecos_delete(gecos, false);
  }
}

void
gecos_delete(struct GecosItem *gecos, bool expired)
{
  if (expired == true)
    sendto_realops_flags(UMODE_EXPIRATION, L_ALL, SEND_NOTICE, "Temporary X-line for [%s] expired",
                         gecos->mask);

  dlinkDelete(&gecos->node, &gecos_list);
  xfree(gecos->mask);
  xfree(gecos->reason);
  xfree(gecos);
}

struct GecosItem *
gecos_make(void)
{
  struct GecosItem *gecos = xcalloc(sizeof(*gecos));
  dlinkAdd(gecos, &gecos->node, &gecos_list);

  return gecos;
}

struct GecosItem *
gecos_find(const char *name, int (*compare)(const char *, const char *))
{
  dlink_node *node, *node_next;

  DLINK_FOREACH_SAFE(node, node_next, gecos_list.head)
  {
    struct GecosItem *gecos = node->data;

    if (gecos->expire &&
        (gecos->expire <= event_base->time.sec_real))
      gecos_delete(gecos, true);
    else if (compare(gecos->mask, name) == 0)
      return gecos;
  }

  return NULL;
}

void
gecos_expire(void)
{
  dlink_node *node, *node_next;

  DLINK_FOREACH_SAFE(node, node_next, gecos_list.head)
  {
    struct GecosItem *gecos = node->data;

    if (gecos->expire &&
        (gecos->expire <= event_base->time.sec_real))
      gecos_delete(gecos, true);
  }
}
