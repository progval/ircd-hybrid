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

/*! \file send.c
 * \brief Functions for sending messages.
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "send.h"
#include "channel.h"
#include "client.h"
#include "dbuf.h"
#include "irc_string.h"
#include "ircd.h"
#include "s_bsd.h"
#include "server_capab.h"
#include "conf_class.h"
#include "log.h"


static uintmax_t current_serial;


/* send_format()
 *
 * inputs
 *		- buffer
 *		- format pattern to use
 *		- var args
 * output	- number of bytes formatted output
 * side effects	- modifies sendbuf
 */
static void
send_format(struct dbuf_block *buffer, const char *pattern, va_list args)
{
  /*
   * from rfc1459
   *
   * IRC messages are always lines of characters terminated with a CR-LF
   * (Carriage Return - Line Feed) pair, and these messages shall not
   * exceed 512 characters in length,  counting all characters
   * including the trailing CR-LF.
   * Thus, there are 510 characters maximum allowed
   * for the command and its parameters.  There is no provision for
   * continuation message lines.  See section 7 for more details about
   * current implementations.
   */
  dbuf_put_args(buffer, pattern, args);

  if (buffer->size > IRCD_BUFSIZE - 2)
    buffer->size = IRCD_BUFSIZE - 2;

  buffer->data[buffer->size++] = '\r';
  buffer->data[buffer->size++] = '\n';
}

/*
 ** send_message
 **      Internal utility which appends given buffer to the sockets
 **      sendq.
 */
static void
send_message(struct Client *to, struct dbuf_block *buffer)
{
  assert(!IsMe(to));
  assert(to != &me);
  assert(MyConnect(to));

  if (dbuf_length(&to->connection->buf_sendq) + buffer->size > get_sendq(&to->connection->confs))
  {
    if (IsServer(to))
      sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                           "Max SendQ limit exceeded for %s: %zu > %u",
                           client_get_name(to, HIDE_IP),
                           (dbuf_length(&to->connection->buf_sendq) + buffer->size),
                           get_sendq(&to->connection->confs));

    if (IsClient(to))
      AddFlag(to, FLAGS_SENDQEX);

    dead_link_on_write(to, 0);
    return;
  }

  dbuf_add(&to->connection->buf_sendq, buffer);

  /*
   * Update statistics. The following is slightly incorrect because
   * it counts messages even if queued, but bytes only really sent.
   * Queued bytes get updated in send_queued_write().
   */
  ++to->connection->send.messages;
  ++me.connection->send.messages;

  send_queued_write(to);
}

/* send_message_remote()
 *
 * inputs	- pointer to client from message is being sent
 * 		- pointer to client to send to
 *		- pointer to buffer
 * output	- none
 * side effects	- Despite the function name, this only sends to directly
 *		  connected clients.
 *
 */
static void
send_message_remote(struct Client *to, const struct Client *from, struct dbuf_block *buffer)
{
  assert(MyConnect(to));
  assert(IsServer(to));
  assert(!IsMe(to));
  assert(to->from == to);

  if (to == from->from)
    sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE, "Send message to %s dropped from %s (Fake Dir)",
                         to->name, from->name);
  else
    send_message(to, buffer);
}

/*
 ** sendq_unblocked
 **      Called when a socket is ready for writing.
 */
void
sendq_unblocked(fde_t *F, void *data)
{
  struct Client *const client = data;

  assert(client);
  assert(client->connection);
  assert(client->connection->fd);
  assert(client->connection->fd == F);

  DelFlag(client, FLAGS_BLOCKED);
  send_queued_write(client);
}

/*
 ** send_queued_write
 **      This is called when there is a chance that some output would
 **      be possible. This attempts to empty the send queue as far as
 **      possible, and then if any data is left, a write is rescheduled.
 */
void
send_queued_write(struct Client *to)
{
  ssize_t retlen;

  /*
   * Once socket is marked dead, we cannot start writing to it,
   * even if the error is removed...
   */
  if (IsDead(to) || HasFlag(to, FLAGS_BLOCKED))
    return;  /* no use calling send() now */

  /* Next, lets try to write some data */
  while (dbuf_length(&to->connection->buf_sendq))
  {
    bool want_read = false;
    const struct dbuf_block *first = to->connection->buf_sendq.blocks.head->data;

    if (tls_isusing(&to->connection->fd->tls))
    {
      retlen = tls_write(&to->connection->fd->tls, first->data + to->connection->buf_sendq.pos,
                                                   first->size - to->connection->buf_sendq.pos, &want_read);

      if (want_read == true)
        return;  /* Retry later, don't register for write events */
    }
    else
      retlen = send(to->connection->fd->fd, first->data + to->connection->buf_sendq.pos,
                                            first->size - to->connection->buf_sendq.pos, 0);

    if (retlen <= 0)
    {
      if (retlen < 0 && comm_ignore_errno(errno) == true)
      {
        AddFlag(to, FLAGS_BLOCKED);
        /* We have a non-fatal error, reschedule a write */
        comm_setselect(to->connection->fd, COMM_SELECT_WRITE, sendq_unblocked, to, 0);
      }
      else
        dead_link_on_write(to, errno);
      return;
    }

    dbuf_delete(&to->connection->buf_sendq, retlen);

    /* We have some data written .. update counters */
    to->connection->send.bytes += retlen;
    me.connection->send.bytes += retlen;
  }
}

/* sendto_one()
 *
 * inputs	- pointer to destination client
 *		- var args message
 * output	- NONE
 * side effects	- send message to single client
 */
void
sendto_one(struct Client *to, const char *pattern, ...)
{
  va_list args;

  if (IsDead(to->from))
    return;  /* This socket has already been marked as dead */

  va_start(args, pattern);

  struct dbuf_block *buffer = dbuf_alloc();
  send_format(buffer, pattern, args);

  va_end(args);

  send_message(to->from, buffer);

  dbuf_ref_free(buffer);
}

void
sendto_one_numeric(struct Client *to, const struct Client *from, enum irc_numerics numeric, ...)
{
  va_list args;

  if (IsDead(to->from))
    return;  /* This socket has already been marked as dead */

  const char *dest = ID_or_name(to, to);
  if (EmptyString(dest))
    dest = "*";

  struct dbuf_block *buffer = dbuf_alloc();
  dbuf_put_fmt(buffer, ":%s %03d %s ", ID_or_name(from, to), numeric & ~SND_EXPLICIT, dest);

  va_start(args, numeric);

  const char *numstr;
  if (numeric & SND_EXPLICIT)
    numstr = va_arg(args, const char *);
  else
    numstr = numeric_form(numeric);

  send_format(buffer, numstr, args);
  va_end(args);

  send_message(to->from, buffer);

  dbuf_ref_free(buffer);
}

void
sendto_one_notice(struct Client *to, const struct Client *from, const char *pattern, ...)
{
  va_list args;

  if (IsDead(to->from))
    return;  /* This socket has already been marked as dead */

  const char *dest = ID_or_name(to, to);
  if (EmptyString(dest))
    dest = "*";

  struct dbuf_block *buffer = dbuf_alloc();
  dbuf_put_fmt(buffer, ":%s NOTICE %s ", ID_or_name(from, to), dest);

  va_start(args, pattern);
  send_format(buffer, pattern, args);
  va_end(args);

  send_message(to->from, buffer);

  dbuf_ref_free(buffer);
}

/* sendto_channel_butone()
 *
 * inputs	- pointer to client(server) to NOT send message to
 *		- pointer to client that is sending this message
 *		- pointer to channel being sent to
 *		- vargs message
 * output	- NONE
 * side effects	- message as given is sent to given channel members.
 *
 * WARNING - +D clients are ignored
 */
void
sendto_channel_butone(struct Client *one, const struct Client *from,
                      struct Channel *channel, unsigned int type,
                      const char *pattern, ...)
{
  va_list args_l, args_r;
  dlink_node *node;
  struct dbuf_block *buffer_l = dbuf_alloc();
  struct dbuf_block *buffer_r = dbuf_alloc();

  if (IsClient(from))
    dbuf_put_fmt(buffer_l, ":%s!%s@%s ", from->name, from->username, from->host);
  else
    dbuf_put_fmt(buffer_l, ":%s ", from->name);

  dbuf_put_fmt(buffer_r, ":%s ", from->id);

  va_start(args_l, pattern);
  va_start(args_r, pattern);
  send_format(buffer_l, pattern, args_l);
  send_format(buffer_r, pattern, args_r);
  va_end(args_l);
  va_end(args_r);

  ++current_serial;

  DLINK_FOREACH(node, channel->members.head)
  {
    struct ChannelMember *member = node->data;
    struct Client *target = member->client;

    assert(IsClient(target));

    if (IsDead(target->from))
      continue;

    if (one && (target->from == one->from))
      continue;

    if (type && (member->flags & type) == 0)
      continue;

    if (HasUMode(target, UMODE_DEAF))
      continue;

    if (MyConnect(target))
      send_message(target, buffer_l);
    else if (target->from->connection->serial != current_serial)
      send_message_remote(target->from, from, buffer_r);

    target->from->connection->serial = current_serial;
  }

  dbuf_ref_free(buffer_l);
  dbuf_ref_free(buffer_r);
}

/* sendto_server()
 *
 * inputs       - pointer to client to NOT send to
 *              - pointer to channel
 *              - capabs or'd together which must ALL be present
 *              - capabs or'd together which must ALL NOT be present
 *              - printf style format string
 *              - args to format string
 * output       - NONE
 * side effects - Send a message to all connected servers, except the
 *                client 'one' (if non-NULL), as long as the servers
 *                support ALL capabs in 'capab', and NO capabs in 'nocapab'.
 *
 * This function was written in an attempt to merge together the other
 * billion sendto_*serv*() functions, which sprung up with capabs,
 * lazylinks, uids, etc.
 * -davidt
 */
void
sendto_server(const struct Client *one,
              const unsigned int capab,
              const unsigned int nocapab,
              const char *format, ...)
{
  va_list args;
  dlink_node *node;
  struct dbuf_block *buffer = dbuf_alloc();

  va_start(args, format);
  send_format(buffer, format, args);
  va_end(args);

  DLINK_FOREACH(node, local_server_list.head)
  {
    struct Client *client = node->data;

    /* If dead already skip */
    if (IsDead(client))
      continue;

    /* check against 'one' */
    if (one && (client == one->from))
      continue;

    /* check we have required capabs */
    if (IsCapable(client, capab) != capab)
      continue;

    /* check we don't have any forbidden capabs */
    if (IsCapable(client, nocapab))
      continue;

    send_message(client, buffer);
  }

  dbuf_ref_free(buffer);
}

/* sendto_common_channels_local()
 *
 * inputs	- pointer to client
 *		- pattern to send
 * output	- NONE
 * side effects	- Sends a message to all people on local server who are
 * 		  in same channel with user.
 *		  used by m_nick.c and exit_one_client.
 */
void
sendto_common_channels_local(struct Client *user, bool touser, unsigned int poscap,
                             unsigned int negcap, const char *pattern, ...)
{
  va_list args;
  dlink_node *node, *node2;
  struct dbuf_block *buffer = dbuf_alloc();

  va_start(args, pattern);
  send_format(buffer, pattern, args);
  va_end(args);

  ++current_serial;

  DLINK_FOREACH(node, user->channel.head)
  {
    struct ChannelMember *member = node->data;
    struct Channel *channel = member->channel;

    DLINK_FOREACH(node2, channel->members_local.head)
    {
      struct ChannelMember *member2 = node2->data;
      struct Client *target = member2->client;

      if (IsDead(target))
        continue;

      if (target == user)
        continue;

      if (target->connection->serial == current_serial)
        continue;

      if (poscap && HasCap(target, poscap) != poscap)
        continue;

      if (negcap && HasCap(target, negcap))
        continue;

      target->connection->serial = current_serial;
      send_message(target, buffer);
    }
  }

  if (touser == true && MyConnect(user) && !IsDead(user))
    if (HasCap(user, poscap) == poscap)
      send_message(user, buffer);

  dbuf_ref_free(buffer);
}

/*! \brief Send a message to members of a channel that are locally connected to this server.
 * \param one      Client to skip; can be NULL
 * \param channel    Destination channel
 * \param status   Channel member status flags clients must have
 * \param poscap   Positive client capabilities flags (CAP)
 * \param negcap   Negative client capabilities flags (CAP)
 * \param pattern  Format string for command arguments
 */
void
sendto_channel_local(const struct Client *one, struct Channel *channel, unsigned int status,
                     unsigned int poscap, unsigned int negcap, const char *pattern, ...)
{
  va_list args;
  dlink_node *node;
  struct dbuf_block *buffer = dbuf_alloc();

  va_start(args, pattern);
  send_format(buffer, pattern, args);
  va_end(args);

  DLINK_FOREACH(node, channel->members_local.head)
  {
    struct ChannelMember *member = node->data;
    struct Client *target = member->client;

    if (IsDead(target))
      continue;

    if (one && (target == one->from))
      continue;

    if (status && (member->flags & status) == 0)
      continue;

    if (poscap && HasCap(target, poscap) != poscap)
      continue;

    if (negcap && HasCap(target, negcap))
      continue;

    send_message(target, buffer);
  }

  dbuf_ref_free(buffer);
}

/*
 ** match_it() and sendto_match_butone() ARE only used
 ** to send a msg to all ppl on servers/hosts that match a specified mask
 ** (used for enhanced PRIVMSGs) for opers
 **
 ** addition -- Armin, 8jun90 (gruner@informatik.tu-muenchen.de)
 **
 */

/* match_it()
 *
 * inputs	- client pointer to match on
 *		- actual mask to match
 *		- what to match on, HOST or SERVER
 * output	- 1 or 0 if match or not
 * side effects	- NONE
 */
static bool
match_it(const struct Client *one, const char *mask, unsigned int what)
{
  if (what == MATCH_HOST)
    return match(mask, one->host) == 0;

  return match(mask, one->servptr->name) == 0;
}

/* sendto_match_butone()
 *
 * Send to all clients which match the mask in a way defined on 'what';
 * either by user hostname or user servername.
 *
 * ugh. ONLY used by m_message.c to send an "oper magic" message. ugh.
 */
void
sendto_match_butone(const struct Client *one, const struct Client *from,
                    const char *mask, int what, const char *pattern, ...)
{
  va_list args_l, args_r;
  dlink_node *node;
  struct dbuf_block *buffer_l = dbuf_alloc();
  struct dbuf_block *buffer_r = dbuf_alloc();

  dbuf_put_fmt(buffer_l, ":%s!%s@%s ", from->name, from->username, from->host);
  dbuf_put_fmt(buffer_r, ":%s ", from->id);

  va_start(args_l, pattern);
  va_start(args_r, pattern);
  send_format(buffer_l, pattern, args_l);
  send_format(buffer_r, pattern, args_r);
  va_end(args_l);
  va_end(args_r);

  /* Scan the local clients */
  DLINK_FOREACH(node, local_client_list.head)
  {
    struct Client *client = node->data;

    if (IsDead(client))
      continue;

    if (one && (client == one->from))
      continue;

    if (match_it(client, mask, what) == false)
      continue;

    send_message(client, buffer_l);
  }

  /* Now scan servers */
  DLINK_FOREACH(node, local_server_list.head)
  {
    struct Client *client = node->data;

    /*
     * The old code looped through every client on the network for each
     * server to check if the server (client) has at least 1 client
     * matching the mask, using something like:
     *
     * for (target = GlobalClientList; target; target = target->next)
     *        if (IsRegisteredUser(target) &&
     *                        match_it(target, mask, what) &&
     *                        (target->from == client))
     *   vsendto_prefix_one(client, from, pattern, args);
     *
     * That way, we wouldn't send the message to a server who didn't have
     * a matching client. However, on a network such as EFNet, that code
     * would have looped through about 50 servers, and in each loop, loop
     * through about 50k clients as well, calling match() in each nested
     * loop. That is a very bad thing cpu wise - just send the message to
     * every connected server and let that server deal with it.
     * -wnder
     */
    if (IsDead(client))
      continue;

    if (one && (client == one->from))
      continue;

    send_message_remote(client, from, buffer_r);
  }

  dbuf_ref_free(buffer_l);
  dbuf_ref_free(buffer_r);
}

/* sendto_match_servs()
 *
 * inputs       - source client
 *              - mask to send to
 *              - capab needed
 *              - data
 * outputs	- none
 * side effects	- data sent to servers matching with capab
 */
void
sendto_match_servs(const struct Client *source_p, const char *mask, unsigned int capab,
                   const char *pattern, ...)
{
  va_list args;
  dlink_node *node;
  struct dbuf_block *buffer = dbuf_alloc();

  dbuf_put_fmt(buffer, ":%s ", source_p->id);

  va_start(args, pattern);
  send_format(buffer, pattern, args);
  va_end(args);

  ++current_serial;

  DLINK_FOREACH(node, global_server_list.head)
  {
    struct Client *target = node->data;

    if (IsDead(target->from))
      continue;

    /* Do not attempt to send to ourselves ... */
    if (IsMe(target))
      continue;

    /* ... or the source */
    if (target->from == source_p->from)
      continue;

    if (target->from->connection->serial == current_serial)
      continue;

    if (IsCapable(target->from, capab) != capab)
      continue;

    if (match(mask, target->name))
      continue;

    target->from->connection->serial = current_serial;
    send_message_remote(target->from, source_p, buffer);
  }

  dbuf_ref_free(buffer);
}

/* sendto_anywhere()
 *
 * inputs	- pointer to dest client
 * 		- pointer to from client
 * 		- varags
 * output	- NONE
 * side effects	- less efficient than sendto_remote and sendto_one
 * 		  but useful when one does not know where target "lives"
 */
void
sendto_anywhere(struct Client *to, const struct Client *from,
                const char *command,
                const char *pattern, ...)
{
  va_list args;

  if (IsDead(to->from))
    return;

  struct dbuf_block *buffer = dbuf_alloc();
  if (MyClient(to) && IsClient(from))
    dbuf_put_fmt(buffer, ":%s!%s@%s %s %s ", from->name, from->username,
                 from->host, command, to->name);
  else
    dbuf_put_fmt(buffer, ":%s %s %s ", ID_or_name(from, to),
                 command, ID_or_name(to, to));

  va_start(args, pattern);
  send_format(buffer, pattern, args);
  va_end(args);

  if (MyConnect(to))
    send_message(to, buffer);
  else
    send_message_remote(to->from, from, buffer);

  dbuf_ref_free(buffer);
}

/* sendto_realops_flags()
 *
 * inputs	- flag types of messages to show to real opers
 *		- flag indicating opers/admins
 *		- var args input message
 * output	- NONE
 * side effects	- Send to *local* ops only but NOT +s nonopers.
 */
void
sendto_realops_flags(unsigned int flags, int level, int type, const char *pattern, ...)
{
  va_list args;
  dlink_node *node;
  const char *ntype = "???";

  switch (type)
  {
    case SEND_NOTICE:
      ntype = "Notice";
      break;
    case SEND_GLOBAL:
      ntype = "Global";
      break;
    case SEND_LOCOPS:
      ntype = "LocOps";
      break;
    default:
      assert(0);
  }

  struct dbuf_block *buffer = dbuf_alloc();
  dbuf_put_fmt(buffer, ":%s NOTICE * :*** %s -- ", me.name, ntype); 

  va_start(args, pattern);
  send_format(buffer, pattern, args);
  va_end(args);

  DLINK_FOREACH(node, oper_list.head)
  {
    struct Client *client = node->data;
    assert(HasUMode(client, UMODE_OPER));

    if (IsDead(client))
      continue;

    /*
     * If we're sending it to opers and they're an admin, skip.
     * If we're sending it to admins, and they're not, skip.
     */
    if (((level == L_ADMIN) && !HasUMode(client, UMODE_ADMIN)) ||
        ((level == L_OPER) && HasUMode(client, UMODE_ADMIN)))
      continue;

    if (!HasUMode(client, flags))
      continue;

    send_message(client, buffer);
  }

  dbuf_ref_free(buffer);
}

/* ts_warn()
 *
 * inputs       - var args message
 * output       - NONE
 * side effects - Call sendto_realops_flags, with some flood checking
 *                (at most 5 warnings every 5 seconds)
 */
void
sendto_realops_flags_ratelimited(uintmax_t *rate, const char *pattern, ...)
{
  va_list args;
  char buffer[IRCD_BUFSIZE];

  if ((event_base->time.sec_monotonic - *rate) < 20)
    return;

  *rate = event_base->time.sec_monotonic;

  va_start(args, pattern);
  vsnprintf(buffer, sizeof(buffer), pattern, args);
  va_end(args);

  sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE, "%s", buffer);
  ilog(LOG_TYPE_IRCD, "%s", buffer);
}

/* sendto_wallops_flags()
 *
 * inputs       - flag types of messages to show to real opers
 *              - client sending request
 *              - var args input message
 * output       - NONE
 * side effects - Send a wallops to local opers
 */
void
sendto_wallops_flags(unsigned int flags, const struct Client *source_p,
                     const char *pattern, ...)
{
  va_list args;
  dlink_node *node;
  struct dbuf_block *buffer = dbuf_alloc();

  if (IsClient(source_p))
    dbuf_put_fmt(buffer, ":%s!%s@%s WALLOPS :", source_p->name, source_p->username, source_p->host);
  else
    dbuf_put_fmt(buffer, ":%s WALLOPS :", source_p->name);

  va_start(args, pattern);
  send_format(buffer, pattern, args);
  va_end(args);

  DLINK_FOREACH(node, oper_list.head)
  {
    struct Client *client = node->data;
    assert(client->umodes & UMODE_OPER);

    if (IsDead(client))
      continue;

    if (!HasUMode(client, flags))
      continue;

    send_message(client, buffer);
  }

  dbuf_ref_free(buffer);
}
