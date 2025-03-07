/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 2000-2022 ircd-hybrid development team
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

/*! \file list.c
 * \brief Maintains doubly-linked lists.
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "memory.h"


/* make_dlink_node()
 *
 * inputs       - NONE
 * output       - pointer to new dlink_node
 * side effects	- NONE
 */
dlink_node *
make_dlink_node(void)
{
  dlink_node *node = xcalloc(sizeof(*node));

  return node;
}

/* free_dlink_node()
 *
 * inputs       - pointer to dlink_node
 * output       - NONE
 * side effects	- free given dlink_node
 */
void
free_dlink_node(dlink_node *node)
{
  xfree(node);
}

/*
 * dlink_ routines are stolen from squid, except for dlinkAddBefore,
 * which is mine.
 *   -- adrian
 */
void
dlinkAdd(void *data, dlink_node *m, dlink_list *list)
{
  m->data = data;
  m->prev = NULL;
  m->next = list->head;

  /* Assumption: If list->tail != NULL, list->head != NULL */
  if (list->head)
    list->head->prev = m;
  else if (list->tail == NULL)
    list->tail = m;

  list->head = m;
  list->length++;
}

void
dlinkAddBefore(dlink_node *b, void *data, dlink_node *m, dlink_list *list)
{
  /* Shortcut - if it's the first one, call dlinkAdd only */
  if (b == list->head)
    dlinkAdd(data, m, list);
  else
  {
    m->data = data;
    b->prev->next = m;
    m->prev = b->prev;
    b->prev = m;
    m->next = b;
    list->length++;
  }
}

void
dlinkAddTail(void *data, dlink_node *m, dlink_list *list)
{
  m->data = data;
  m->next = NULL;
  m->prev = list->tail;

  /* Assumption: If list->tail != NULL, list->head != NULL */
  if (list->tail)
    list->tail->next = m;
  else if (list->head == NULL)
    list->head = m;

  list->tail = m;
  list->length++;
}

/* Execution profiles show that this function is called the most
 * often of all non-spontaneous functions. So it had better be
 * efficient. */
void
dlinkDelete(dlink_node *m, dlink_list *list)
{
  assert(list->length > 0);

  /* Assumption: If m->next == NULL, then list->tail == m
   *      and:   If m->prev == NULL, then list->head == m
   */
  if (m->next)
    m->next->prev = m->prev;
  else
  {
    assert(list->tail == m);
    list->tail = m->prev;
  }

  if (m->prev)
    m->prev->next = m->next;
  else
  {
    assert(list->head == m);
    list->head = m->next;
  }

  /* Set this to NULL does matter */
  m->next = NULL;
  m->prev = NULL;

  list->length--;
}

/*
 * dlinkFind
 * inputs       - list to search
 *              - data
 * output       - pointer to link or NULL if not found
 * side effects - Look for ptr in the linked listed pointed to by link.
 */
dlink_node *
dlinkFind(dlink_list *list, const void *data)
{
  dlink_node *node;

  DLINK_FOREACH(node, list->head)
    if (node->data == data)
      return node;

  return NULL;
}

void
dlinkMoveList(dlink_list *from, dlink_list *to)
{
  /* There are three cases */
  /* case one, nothing in from list */
  if (from->head == NULL)
    return;

  /* case two, nothing in to list */
  /* actually if to->head is NULL and to->tail isn't, that's a bug */
  if (to->head == NULL)
  {
    to->head = from->head;
    to->tail = from->tail;
    from->head = from->tail = NULL;
    to->length = from->length;
    from->length = 0;
    return;
  }

  /* third case play with the links */
  from->tail->next = to->head;
  from->head->prev = to->head->prev;
  to->head->prev = from->tail;
  to->head = from->head;
  from->head = from->tail = NULL;
  to->length += from->length;
  from->length = 0;
  /* I think I got that right */
}

void
dlink_move_node(dlink_node *m, dlink_list *list_del, dlink_list *list_add)
{
  /* Assumption: If m->next == NULL, then list_del->tail == m
   *      and:   If m->prev == NULL, then list_del->head == m
   */
  if (m->next)
    m->next->prev = m->prev;
  else
  {
    assert(list_del->tail == m);
    list_del->tail = m->prev;
  }

  if (m->prev)
    m->prev->next = m->next;
  else
  {
    assert(list_del->head == m);
    list_del->head = m->next;
  }

  /* Set this to NULL does matter */
  m->prev = NULL;
  m->next = list_add->head;

  /* Assumption: If list_add->tail != NULL, list_add->head != NULL */
  if (list_add->head)
    list_add->head->prev = m;
  else if (list_add->tail == NULL)
    list_add->tail = m;

  list_add->head = m;
  list_add->length++;
  list_del->length--;
}

dlink_node *
dlinkFindDelete(dlink_list *list, void *data)
{
  dlink_node *m;

  DLINK_FOREACH(m, list->head)
  {
    if (m->data == data)
    {
      dlinkDelete(m, list);
      return m;
    }
  }

  return NULL;
}
