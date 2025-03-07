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

/*! \file channel.c
 * \brief Responsible for managing channels, members, bans and topics
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "channel.h"
#include "channel_invite.h"
#include "channel_mode.h"
#include "client.h"
#include "hash.h"
#include "conf.h"
#include "conf_resv.h"
#include "hostmask.h"
#include "irc_string.h"
#include "ircd.h"
#include "numeric.h"
#include "server.h"
#include "send.h"
#include "event.h"
#include "memory.h"
#include "misc.h"
#include "extban.h"


/** Doubly linked list containing a list of all channels. */
static dlink_list channel_list;


/*! \brief Returns the channel_list as constant
 * \return channel_list
 */
const dlink_list *
channel_get_list(void)
{
  return &channel_list;
}

/*! \brief Adds a user to a channel by adding another link to the
 *         channels member chain.
 * \param channel    Pointer to channel to add client to
 * \param client     Pointer to client (who) to add
 * \param flags      Flags for chanops etc
 * \param flood_ctrl Whether to count this join in flood calculations
 */
void
add_user_to_channel(struct Channel *channel, struct Client *client,
                    unsigned int flags, bool flood_ctrl)
{
  assert(IsClient(client));

  if (GlobalSetOptions.joinfloodtime)
  {
    if (flood_ctrl == true)
      ++channel->number_joined;

    channel->number_joined -= (event_base->time.sec_monotonic - channel->last_join_time) *
      (((float)GlobalSetOptions.joinfloodcount) /
       (float)GlobalSetOptions.joinfloodtime);

    if (channel->number_joined <= 0)
    {
      channel->number_joined = 0;
      ClearJoinFloodNoticed(channel);
    }
    else if (channel->number_joined >= GlobalSetOptions.joinfloodcount)
    {
      channel->number_joined = GlobalSetOptions.joinfloodcount;

      if (!IsSetJoinFloodNoticed(channel))
      {
        SetJoinFloodNoticed(channel);
        sendto_realops_flags(UMODE_FLOOD, L_ALL, SEND_NOTICE,
                             "Possible Join Flooder %s on %s target: %s",
                             client_get_name(client, HIDE_IP),
                             client->servptr->name, channel->name);
      }
    }

    channel->last_join_time = event_base->time.sec_monotonic;
  }

  struct ChannelMember *member = xcalloc(sizeof(*member));
  member->client = client;
  member->channel = channel;
  member->flags = flags;

  dlinkAdd(member, &member->channode, &channel->members);

  if (MyConnect(client))
    dlinkAdd(member, &member->locchannode, &channel->members_local);

  dlinkAdd(member, &member->usernode, &client->channel);
}

/*! \brief Deletes an user from a channel by removing a link in the
 *         channels member chain.
 * \param member Pointer to Membership struct
 */
void
remove_user_from_channel(struct ChannelMember *member)
{
  struct Client *const client = member->client;
  struct Channel *const channel = member->channel;

  dlinkDelete(&member->channode, &channel->members);

  if (MyConnect(client))
    dlinkDelete(&member->locchannode, &channel->members_local);

  dlinkDelete(&member->usernode, &client->channel);

  xfree(member);

  if (channel->members.head == NULL)
    channel_free(channel);
}

/* remove_a_mode()
 *
 * inputs       -
 * output       - NONE
 * side effects - remove ONE mode from a channel
 */
void
channel_demote_members(struct Channel *channel, const struct Client *client)
{
  dlink_node *node;
  char modebuf[MAXMODEPARAMS + 1];
  char parabuf[MAXMODEPARAMS * (NICKLEN + 1) + 1];
  char *mbuf = modebuf;
  char *pbuf = parabuf;
  unsigned int pargs = 0;
  struct member_status
  {
    unsigned char prefix;
    unsigned int flag;
  } table[] =
  {
    { 'o', CHFL_CHANOP },
    { 'h', CHFL_HALFOP },
    { 'v', CHFL_VOICE  },
    { '\0', 0 }
  };

  DLINK_FOREACH(node, channel->members.head)
  {
    struct ChannelMember *member = node->data;

    for (const struct member_status *status = table; status->flag; ++status)
    {
      if (member->flags & status->flag)
      {
        member->flags &= ~status->flag;
        *mbuf++ = status->prefix;
        pbuf += snprintf(pbuf, sizeof(parabuf) - (pbuf - parabuf), "%s ", member->client->name);

        if (++pargs >= MAXMODEPARAMS)
        {
          *mbuf = *(pbuf - 1) = '\0';
          sendto_channel_local(NULL, channel, 0, 0, 0, ":%s MODE %s -%s %s",
                               client->name, channel->name, modebuf, parabuf);

          mbuf = modebuf;
          pbuf = parabuf;
          pargs = 0;
        }
      }
    }
  }

  if (pargs)
  {
    *mbuf = *(pbuf - 1) = '\0';
    sendto_channel_local(NULL, channel, 0, 0, 0, ":%s MODE %s -%s %s",
                         client->name, channel->name, modebuf, parabuf);
  }
}

/* channel_send_members()
 *
 * inputs       -
 * output       - NONE
 * side effects -
 */
static void
channel_send_members(struct Client *client, const struct Channel *channel,
                     const char *modebuf, const char *parabuf)
{
  dlink_node *node;
  char buf[IRCD_BUFSIZE];
  int tlen;              /* length of text to append */
  char *t, *start;       /* temp char pointer */

  start = t = buf + snprintf(buf, sizeof(buf), ":%s SJOIN %ju %s %s %s:",
                             me.id, channel->creation_time,
                             channel->name, modebuf, parabuf);

  DLINK_FOREACH(node, channel->members.head)
  {
    const struct ChannelMember *member = node->data;

    tlen = strlen(member->client->id) + 1;  /* +1 for space */
    tlen += member_get_prefix_len(member, true);

    /*
     * Space will be converted into CR, but we also need space for LF..
     * That's why we use '- 1' here -adx
     */
    if (t + tlen - buf > sizeof(buf) - 1)
    {
      *(t - 1) = '\0';  /* Kill the space and terminate the string */
      sendto_one(client, "%s", buf);
      t = start;
    }

    t += snprintf(t, sizeof(buf) - (t - buf), "%s%s ", member_get_prefix(member, true), member->client->id);
  }

  /* Should always be non-NULL unless we have a kind of persistent channels */
  if (channel->members.head)
    --t;  /* Take the space out */
  *t = '\0';
  sendto_one(client, "%s", buf);
}

/*! \brief Sends +b/+e/+I
 * \param client   Client pointer to server
 * \param channel  Pointer to channel
 * \param list     Pointer to list of modes to send
 * \param flag     Char flag flagging type of mode. Currently this can be 'b', e' or 'I'
 */
static void
channel_send_mask_list(struct Client *client, const struct Channel *channel,
                       const dlink_list *list, const char flag)
{
  dlink_node *node;
  char modebuf[IRCD_BUFSIZE];
  char parabuf[IRCD_BUFSIZE];
  size_t tlen, mlen, cur_len;
  char *pbuf = parabuf;

  if (dlink_list_length(list) == 0)
    return;

  mlen = snprintf(modebuf, sizeof(modebuf), ":%s BMASK %ju %s %c :", me.id,
                  channel->creation_time, channel->name, flag);
  cur_len = mlen;

  DLINK_FOREACH(node, list->head)
  {
    const struct Ban *ban = node->data;

    tlen = ban->banstr_len + 1;  /* +1 for space */

    /*
     * Send buffer and start over if we cannot fit another ban
     */
    if (cur_len + (tlen - 1) > sizeof(parabuf) - 2)
    {
      *(pbuf - 1) = '\0';  /* Get rid of trailing space on buffer */
      sendto_one(client, "%s%s", modebuf, parabuf);

      cur_len = mlen;
      pbuf = parabuf;
    }

    pbuf += snprintf(pbuf, sizeof(parabuf) - (pbuf - parabuf), "%s ", ban->banstr);
    cur_len += tlen;
  }

  *(pbuf - 1) = '\0';  /* Get rid of trailing space on buffer */
  sendto_one(client, "%s%s", modebuf, parabuf);
}

/*! \brief Send "client" a full list of the modes for channel channel
 * \param client  Pointer to client client
 * \param channel Pointer to channel pointer
 */
void
channel_send_modes(struct Client *client, const struct Channel *channel)
{
  char modebuf[MODEBUFLEN] = "";
  char parabuf[MODEBUFLEN] = "";

  channel_modes(channel, client, NULL, modebuf, parabuf);
  channel_send_members(client, channel, modebuf, parabuf);

  channel_send_mask_list(client, channel, &channel->banlist, 'b');
  channel_send_mask_list(client, channel, &channel->exceptlist, 'e');
  channel_send_mask_list(client, channel, &channel->invexlist, 'I');
}

/*! \brief Check channel name for invalid characters
 * \param name Pointer to channel name string
 * \param local Indicates whether it's a local or remote creation
 * \return false if invalid, true otherwise
 */
bool
channel_check_name(const char *name, bool local)
{
  const char *p = name;

  assert(!EmptyString(p));

  if (!IsChanPrefix(*p))
    return false;

  if (local == false || ConfigChannel.disable_fake_channels == 0)
  {
    while (*++p)
      if (!IsChanChar(*p))
        return false;
  }
  else
  {
    while (*++p)
      if (!IsVisibleChanChar(*p))
        return false;
  }

  return p - name <= CHANNELLEN;
}

void
remove_ban(struct Ban *ban, dlink_list *list)
{
  dlinkDelete(&ban->node, list);
  xfree(ban);
}

/* channel_free_mask_list()
 *
 * inputs       - pointer to dlink_list
 * output       - NONE
 * side effects -
 */
static void
channel_free_mask_list(dlink_list *list)
{
  while (list->head)
  {
    struct Ban *ban = list->head->data;
    remove_ban(ban, list);
  }
}

/*! \brief Get Channel block for name (and allocate a new channel
 *         block, if it didn't exist before)
 * \param name Channel name
 * \return Channel block
 */
struct Channel *
channel_make(const char *name)
{
  assert(!EmptyString(name));

  struct Channel *channel = xcalloc(sizeof(*channel));
  channel->hnextch = channel;
  /* Doesn't hurt to set it here */
  channel->creation_time = event_base->time.sec_real;
  channel->last_join_time = event_base->time.sec_monotonic;

  /* Cache channel name length to avoid repetitive strlen() calls. */
  channel->name_len = strlcpy(channel->name, name, sizeof(channel->name));
  if (channel->name_len >= sizeof(channel->name))
    channel->name_len = sizeof(channel->name) - 1;

  dlinkAdd(channel, &channel->node, &channel_list);
  hash_add_channel(channel);

  return channel;
}

/*! \brief Walk through this channel, and destroy it.
 * \param channel Channel pointer
 */
void
channel_free(struct Channel *channel)
{
  invite_clear_list(&channel->invites);

  /* Free ban/exception/invex lists */
  channel_free_mask_list(&channel->banlist);
  channel_free_mask_list(&channel->exceptlist);
  channel_free_mask_list(&channel->invexlist);

  dlinkDelete(&channel->node, &channel_list);
  hash_del_channel(channel);

  assert(channel->hnextch == channel);

  assert(channel->node.prev == NULL);
  assert(channel->node.next == NULL);

  assert(dlink_list_length(&channel->members_local) == 0);
  assert(channel->members_local.head == NULL);
  assert(channel->members_local.tail == NULL);

  assert(dlink_list_length(&channel->members) == 0);
  assert(channel->members.head == NULL);
  assert(channel->members.tail == NULL);

  assert(dlink_list_length(&channel->invites) == 0);
  assert(channel->invites.head == NULL);
  assert(channel->invites.tail == NULL);

  assert(dlink_list_length(&channel->banlist) == 0);
  assert(channel->banlist.head == NULL);
  assert(channel->banlist.tail == NULL);

  assert(dlink_list_length(&channel->exceptlist) == 0);
  assert(channel->exceptlist.head == NULL);
  assert(channel->exceptlist.tail == NULL);

  assert(dlink_list_length(&channel->invexlist) == 0);
  assert(channel->invexlist.head == NULL);
  assert(channel->invexlist.tail == NULL);

  xfree(channel);
}

/*!
 * \param channel Pointer to channel
 * \return String pointer "=" if public, "@" if secret else "*"
 */
static const char *
channel_pub_or_secret(const struct Channel *channel)
{
  if (SecretChannel(channel))
    return "@";
  if (PrivateChannel(channel))
    return "*";
  return "=";
}

/*! \brief lists all names on given channel
 * \param client   Pointer to client struct requesting names
 * \param channel  Pointer to channel block
 */
void
channel_send_namereply(struct Client *client, struct Channel *channel)
{
  dlink_node *node;
  char buf[IRCD_BUFSIZE + 1];
  char *bufptr = buf;
  size_t masklen = 0;
  bool is_member = member_find_link(client, channel) != NULL;
  bool multi_prefix = HasCap(client, CAP_MULTI_PREFIX) != 0;
  bool uhnames = HasCap(client, CAP_UHNAMES) != 0;

  assert(IsClient(client));

  if (PubChannel(channel) || is_member == true)
  {
    /* :me.name 353 client->name @ channel->name :+nick1 @nick2 %nick3 ...\r\n */
    /* 1       23456            789             01                        2 3  */
    size_t len = strlen(me.name) + strlen(client->name) + channel->name_len + 13;

    DLINK_FOREACH(node, channel->members.head)
    {
      const struct ChannelMember *member = node->data;

      if (HasUMode(member->client, UMODE_INVISIBLE) && is_member == false)
        continue;

      if (uhnames == true)
        masklen = strlen(member->client->name) + strlen(member->client->username) +
                  strlen(member->client->host) + 3;  /* +3 for ! + @ + space */
      else
        masklen = strlen(member->client->name) + 1;  /* +1 for space */

      masklen += member_get_prefix_len(member, multi_prefix);

      if ((bufptr - buf) + masklen + len > sizeof(buf))
      {
        *(bufptr - 1) = '\0';
        sendto_one_numeric(client, &me, RPL_NAMREPLY,
                           channel_pub_or_secret(channel), channel->name, buf);
        bufptr = buf;
      }

      if (uhnames == true)
        bufptr += snprintf(bufptr, sizeof(buf) - (bufptr - buf), "%s%s!%s@%s ",
                           member_get_prefix(member, multi_prefix),
                           member->client->name, member->client->username,
                           member->client->host);
      else
        bufptr += snprintf(bufptr, sizeof(buf) - (bufptr - buf), "%s%s ",
                           member_get_prefix(member, multi_prefix),
                           member->client->name);
    }

    if (bufptr != buf)
    {
      *(bufptr - 1) = '\0';
      sendto_one_numeric(client, &me, RPL_NAMREPLY,
                         channel_pub_or_secret(channel), channel->name, buf);
    }
  }

  sendto_one_numeric(client, &me, RPL_ENDOFNAMES, channel->name);
}

/* member_get_prefix()
 *
 * inputs       - pointer to struct ChannelMember
 *              - YES if we can combine different flags
 * output       - string either @, +, % or "" depending on whether
 *                chanop, voiced or user
 * side effects -
 *
 * NOTE: Returned string is usually a static buffer
 * (like in client_get_name)
 */
const char *
member_get_prefix(const struct ChannelMember *member, bool combine)
{
  static char buf[CMEMBER_STATUS_FLAGS_LEN + 1];  /* +1 for \0 */
  char *bufptr = buf;

  if (member->flags & CHFL_CHANOP)
  {
    if (combine == false)
      return "@";
    *bufptr++ = '@';
  }

  if (member->flags & CHFL_HALFOP)
  {
    if (combine == false)
      return "%";
    *bufptr++ = '%';
  }

  if (member->flags & CHFL_VOICE)
    *bufptr++ = '+';
  *bufptr = '\0';

  return buf;
}

size_t
member_get_prefix_len(const struct ChannelMember *member, bool combine)
{
  size_t len = 0;

  if (member->flags & CHFL_CHANOP)
  {
    if (combine == false)
      return 1;
    ++len;
  }

  if (member->flags & CHFL_HALFOP)
  {
    if (combine == false)
      return 1;
    ++len;
  }

  if (member->flags & CHFL_VOICE)
  {
    if (combine == false)
      return 1;
    ++len;
  }

  return len;
}

/*!
 * \param client Pointer to Client to check
 * \param list   Pointer to ban list to search
 * \return true if ban found for given n!u\@h mask, false otherwise
 */
static bool
ban_matches(struct Client *client, struct Channel *channel, struct Ban *ban)
{
  /* Is a matching extban, call custom match handler */
  if (ban->extban & extban_matching_mask())
  {
    struct Extban *extban = extban_find_flag(ban->extban & extban_matching_mask());
    if (extban == NULL)
      return false;

    if (extban->matches == NULL || extban->matches(client, channel, ban) == EXTBAN_NO_MATCH)
      return false;

    return true;
  }

  if (match(ban->name, client->name) == 0 && match(ban->user, client->username) == 0)
  {
    switch (ban->type)
    {
      case HM_HOST:
        if (match(ban->host, client->realhost) == 0 ||
            match(ban->host, client->sockhost) == 0 || match(ban->host, client->host) == 0)
          return true;
        break;
      case HM_IPV6:
      case HM_IPV4:
        if (address_compare(&client->ip, &ban->addr, false, false, ban->bits) == true)
          return true;
        break;
      default:
        assert(0);
    }
  }

  return false;
}

bool
find_bmask(struct Client *client, struct Channel *channel, const dlink_list *list, struct Extban *extban)
{
  dlink_node *node;

  DLINK_FOREACH(node, list->head)
  {
    struct Ban *ban = node->data;

    /* Looking for a specific type of extban? */
    if (extban)
    {
      if (!(ban->extban & extban->flag))
        continue;
    }
    else
    {
      /*
       * Acting extbans have their own time they act and are not general purpose bans,
       * so skip them unless we are hunting them.
       */
      if (ban->extban & extban_acting_mask())
        continue;
    }

    bool matches = ban_matches(client, channel, ban);
    if (matches == false)
      continue;

    return true;
  }

  return false;
}

/*!
 * \param channel Pointer to channel block
 * \param client  Pointer to client to check access fo
 * \return false if not banned, true otherwise
 */
bool
is_banned(struct Channel *channel, struct Client *client)
{
  if (find_bmask(client, channel, &channel->banlist, NULL) == true)
    return find_bmask(client, channel, &channel->exceptlist, NULL) == false;
  return false;
}

/*! Tests if a client can join a certain channel
 * \param client Pointer to client attempting to join
 * \param channel  Pointer to channel
 * \param key      Key sent by client attempting to join if present
 * \return ERR_BANNEDFROMCHAN, ERR_INVITEONLYCHAN, ERR_CHANNELISFULL
 *         or 0 if allowed to join.
 */
static int
can_join(struct Client *client, struct Channel *channel, const char *key)
{
  if (HasCMode(channel, MODE_SECUREONLY) && !HasUMode(client, UMODE_SECURE))
    return ERR_SECUREONLYCHAN;

  if (HasCMode(channel, MODE_REGONLY) && !HasUMode(client, UMODE_REGISTERED))
    return ERR_NEEDREGGEDNICK;

  if (HasCMode(channel, MODE_OPERONLY) && !HasUMode(client, UMODE_OPER))
    return ERR_OPERONLYCHAN;

  if (HasCMode(channel, MODE_INVITEONLY))
    if (invite_find(channel, client) == NULL)
      if (find_bmask(client, channel, &channel->invexlist, NULL) == false)
        return ERR_INVITEONLYCHAN;

  if (channel->mode.key[0] && (key == NULL || strcmp(channel->mode.key, key)))
    return ERR_BADCHANNELKEY;

  if (channel->mode.limit && dlink_list_length(&channel->members) >=
      channel->mode.limit)
    return ERR_CHANNELISFULL;

  if (is_banned(channel, client) == true)
    return ERR_BANNEDFROMCHAN;

  return extban_join_can_join(channel, client, NULL);
}

bool
member_has_flags(const struct ChannelMember *member, const unsigned int flags)
{
  if (member && (member->flags & flags))
    return true;
  return false;
}

struct ChannelMember *
member_find_link(const struct Client *client, const struct Channel *channel)
{
  dlink_node *node;

  if (!IsClient(client))
    return NULL;

  /* Take the shortest of the two lists */
  if (dlink_list_length(&channel->members) < dlink_list_length(&client->channel))
  {
    DLINK_FOREACH(node, channel->members.head)
      if (((struct ChannelMember *)node->data)->client == client)
        return node->data;
  }
  else
  {
    DLINK_FOREACH(node, client->channel.head)
      if (((struct ChannelMember *)node->data)->channel == channel)
        return node->data;
  }

  return NULL;
}

/*! Checks if a message contains control codes
 * \param message The actual message string the client wants to send
 * \return true if the message does contain any control codes, false otherwise
 */
static bool
msg_has_ctrls(const char *message)
{
  const unsigned char *p = (const unsigned char *)message;

  for (; *p; ++p)
  {
    if (*p > 31 || *p == 1)
      continue;  /* No control code or CTCP */

    if (*p == 27)  /* Escape */
    {
      /* ISO 2022 charset shift sequence */
      if (*(p + 1) == '$' ||
          *(p + 1) == '(')
      {
        ++p;
        continue;
      }
    }

    return true;  /* Control code */
  }

  return false;  /* No control code found */
}

/*! Tests if a client can send to a channel
 * \param channel Pointer to Channel struct
 * \param client  Pointer to Client struct
 * \param member  Pointer to Membership struct (can be NULL)
 * \param message The actual message string the client wants to send
 * \return CAN_SEND_OPV if op or voiced on channel\n
 *         CAN_SEND_NONOP if can send to channel but is not an op\n
 *         ERR_CANNOTSENDTOCHAN or ERR_NEEDREGGEDNICK if they cannot send to channel\n
 */
int
can_send(struct Channel *channel, struct Client *client,
         struct ChannelMember *member, const char *message, bool notice)
{
  const struct ResvItem *resv;

  if (IsServer(client) || HasFlag(client, FLAGS_SERVICE))
    return CAN_SEND_OPV;

  if (MyConnect(client) && !HasFlag(client, FLAGS_EXEMPTRESV))
    if (!(HasUMode(client, UMODE_OPER) && HasOFlag(client, OPER_FLAG_JOIN_RESV)))
      if ((resv = resv_find(channel->name, match)) && resv_exempt_find(client, resv) == false)
        return ERR_CANNOTSENDTOCHAN;

  if (HasCMode(channel, MODE_NOCTRL) && msg_has_ctrls(message) == true)
    return ERR_NOCTRLSONCHAN;

  if (HasCMode(channel, MODE_NOCTCP))
    if (*message == '\001' && strncmp(message + 1, "ACTION ", 7))
      return ERR_NOCTCP;

  if (member || (member = member_find_link(client, channel)))
    if (member->flags & (CHFL_CHANOP | CHFL_HALFOP | CHFL_VOICE))
      return CAN_SEND_OPV;

  if (member == NULL && HasCMode(channel, MODE_NOPRIVMSGS))
    return ERR_CANNOTSENDTOCHAN;

  if (HasCMode(channel, MODE_MODERATED))
    return ERR_CANNOTSENDTOCHAN;

  if (HasCMode(channel, MODE_MODREG) && !HasUMode(client, UMODE_REGISTERED))
    return ERR_NEEDREGGEDNICK;

  if (HasCMode(channel, MODE_NONOTICE) && notice == true)
    return ERR_CANNOTSENDTOCHAN;

  /* Cache can send if banned */
  if (MyConnect(client))
  {
    if (member)
    {
      if (member->flags & CHFL_BAN_SILENCED)
        return ERR_CANNOTSENDTOCHAN;

      if (!(member->flags & CHFL_BAN_CHECKED))
      {
        if (is_banned(channel, client) == true)
        {
          member->flags |= (CHFL_BAN_CHECKED | CHFL_BAN_SILENCED);
          return ERR_CANNOTSENDTOCHAN;
        }

        member->flags |= CHFL_BAN_CHECKED;
      }
    }
    else if (is_banned(channel, client) == true)
      return ERR_CANNOTSENDTOCHAN;
  }

  return extban_mute_can_send(channel, client, member);
}

/*! \brief Updates the client's oper_warn_count_down, warns the
 *         IRC operators if necessary, and updates
 *         join_leave_countdown as needed.
 * \param client Pointer to struct Client to check
 * \param name   Channel name or NULL if this is a part.
 */
void
check_spambot_warning(struct Client *client, const char *name)
{
  if (GlobalSetOptions.spam_num &&
      (client->connection->join_leave_count >= GlobalSetOptions.spam_num))
  {
    if (client->connection->oper_warn_count_down)
      --client->connection->oper_warn_count_down;

    if (client->connection->oper_warn_count_down == 0)
    {
      client->connection->oper_warn_count_down = OPER_SPAM_COUNTDOWN;

      /* It's already known as a possible spambot */
      if (name)
        sendto_realops_flags(UMODE_FLOOD, L_ALL, SEND_NOTICE,
                             "User %s (%s@%s) trying to join %s is a possible spambot",
                             client->name, client->username,
                             client->host, name);
      else
        sendto_realops_flags(UMODE_FLOOD, L_ALL, SEND_NOTICE,
                             "User %s (%s@%s) is a possible spambot",
                             client->name, client->username,
                             client->host);
    }
  }
  else
  {
    unsigned int t_delta = event_base->time.sec_monotonic - client->connection->last_leave_time;
    if (t_delta > JOIN_LEAVE_COUNT_EXPIRE_TIME)
    {
      unsigned int decrement_count = (t_delta / JOIN_LEAVE_COUNT_EXPIRE_TIME);
      if (decrement_count > client->connection->join_leave_count)
        client->connection->join_leave_count = 0;
      else
        client->connection->join_leave_count -= decrement_count;
    }
    else
    {
      if ((event_base->time.sec_monotonic - client->connection->last_join_time) < GlobalSetOptions.spam_time)
        ++client->connection->join_leave_count;  /* It's a possible spambot */
    }

    if (name)
      client->connection->last_join_time = event_base->time.sec_monotonic;
    else
      client->connection->last_leave_time = event_base->time.sec_monotonic;
  }
}

/*! \brief Sets the channel topic for a certain channel
 * \param channel    Pointer to struct Channel
 * \param topic      The topic string
 * \param topic_info n!u\@h formatted string of the topic setter
 * \param topicts    Timestamp on the topic
 * \param local      Whether the topic is set by a local client
 */
void
channel_set_topic(struct Channel *channel, const char *topic,
                  const char *topic_info, uintmax_t topicts, bool local)
{
  if (local == true)
    strlcpy(channel->topic, topic, IRCD_MIN(sizeof(channel->topic), ConfigServerInfo.max_topic_length + 1));
  else
    strlcpy(channel->topic, topic, sizeof(channel->topic));

  strlcpy(channel->topic_info, topic_info, sizeof(channel->topic_info));
  channel->topic_time = topicts;
}

void
channel_do_join(struct Client *client, char *chan_list, char *key_list)
{
  char *p = NULL;
  const struct ResvItem *resv = NULL;
  const struct ClassItem *const class = class_get_ptr(&client->connection->confs);
  unsigned int flags = 0;

  assert(MyClient(client));

  for (const char *name = strtok_r(chan_list, ",", &p); name;
                   name = strtok_r(NULL,      ",", &p))
  {
    const char *key = NULL;

    /* If we have any more keys, take the first for this channel. */
    if (!EmptyString(key_list) && (key_list = strchr(key = key_list, ',')))
      *key_list++ = '\0';

    /* Empty keys are the same as no keys. */
    if (key && *key == '\0')
      key = NULL;

    if (channel_check_name(name, true) == false)
    {
      sendto_one_numeric(client, &me, ERR_BADCHANNAME, name);
      continue;
    }

    if (!HasFlag(client, FLAGS_EXEMPTRESV) &&
        !(HasUMode(client, UMODE_OPER) && HasOFlag(client, OPER_FLAG_JOIN_RESV)) &&
        ((resv = resv_find(name, match)) && resv_exempt_find(client, resv) == false))
    {
      sendto_one_numeric(client, &me, ERR_CHANBANREASON, name, resv->reason);
      sendto_realops_flags(UMODE_REJ, L_ALL, SEND_NOTICE,
                           "Forbidding reserved channel %s from user %s",
                           name, client_get_name(client, HIDE_IP));
      continue;
    }

    if (dlink_list_length(&client->channel) >=
        ((class->max_channels) ? class->max_channels : ConfigChannel.max_channels))
    {
      sendto_one_numeric(client, &me, ERR_TOOMANYCHANNELS, name);
      break;
    }

    struct Channel *channel = hash_find_channel(name);
    if (channel)
    {
      if (member_find_link(client, channel))
        continue;

      /* can_join() checks for +i, +l, key, bans, etc. */
      int ret = can_join(client, channel, key);
      if (ret)
      {
        sendto_one_numeric(client, &me, ret, channel->name);
        continue;
      }

      /*
       * This should never be the case unless there is some sort of
       * persistent channels.
       */
      if (dlink_list_length(&channel->members) == 0)
        flags = CHFL_CHANOP;
      else
        flags = 0;
    }
    else
    {
      flags = CHFL_CHANOP;
      channel = channel_make(name);
    }

    if (!HasUMode(client, UMODE_OPER))
      check_spambot_warning(client, channel->name);

    add_user_to_channel(channel, client, flags, true);

    /*
     * Set timestamp if appropriate, and propagate
     */
    if (flags == CHFL_CHANOP)
    {
      channel->creation_time = event_base->time.sec_real;
      AddCMode(channel, MODE_TOPICLIMIT);
      AddCMode(channel, MODE_NOPRIVMSGS);

      sendto_server(NULL, 0, 0, ":%s SJOIN %ju %s +nt :@%s",
                    me.id, channel->creation_time,
                    channel->name, client->id);

      /*
       * Notify all other users on the new channel
       */
      sendto_channel_local(NULL, channel, 0, CAP_EXTENDED_JOIN, 0, ":%s!%s@%s JOIN %s %s :%s",
                           client->name, client->username,
                           client->host, channel->name, client->account, client->info);
      sendto_channel_local(NULL, channel, 0, 0, CAP_EXTENDED_JOIN, ":%s!%s@%s JOIN :%s",
                           client->name, client->username,
                           client->host, channel->name);
      sendto_channel_local(NULL, channel, 0, 0, 0, ":%s MODE %s +nt",
                           me.name, channel->name);
    }
    else
    {
      sendto_server(NULL, 0, 0, ":%s JOIN %ju %s +",
                    client->id, channel->creation_time,
                    channel->name);

      sendto_channel_local(NULL, channel, 0, CAP_EXTENDED_JOIN, 0, ":%s!%s@%s JOIN %s %s :%s",
                           client->name, client->username,
                           client->host, channel->name, client->account, client->info);
      sendto_channel_local(NULL, channel, 0, 0, CAP_EXTENDED_JOIN, ":%s!%s@%s JOIN :%s",
                           client->name, client->username,
                           client->host, channel->name);
    }

    if (client->away[0])
      sendto_channel_local(client, channel, 0, CAP_AWAY_NOTIFY, 0,
                           ":%s!%s@%s AWAY :%s",
                           client->name, client->username,
                           client->host, client->away);

    struct Invite *invite = invite_find(channel, client);
    if (invite)
      invite_del(invite);

    if (channel->topic[0])
    {
      sendto_one_numeric(client, &me, RPL_TOPIC, channel->name, channel->topic);
      sendto_one_numeric(client, &me, RPL_TOPICWHOTIME, channel->name,
                         channel->topic_info, channel->topic_time);
    }

    channel_send_namereply(client, channel);

    client->connection->last_join_time = event_base->time.sec_monotonic;
  }
}

/*! \brief Removes a client from a specific channel
 * \param client Pointer to client to remove
 * \param name   Name of channel to remove from
 * \param reason Part reason to show
 */
static void
channel_part_one_client(struct Client *client, const char *name, const char *reason)
{
  struct Channel *channel = hash_find_channel(name);
  if (channel == NULL)
  {
    sendto_one_numeric(client, &me, ERR_NOSUCHCHANNEL, name);
    return;
  }

  struct ChannelMember *member = member_find_link(client, channel);
  if (member == NULL)
  {
    sendto_one_numeric(client, &me, ERR_NOTONCHANNEL, channel->name);
    return;
  }

  if (MyConnect(client) && !HasUMode(client, UMODE_OPER))
    check_spambot_warning(client, NULL);

  /*
   * Remove user from the old channel (if any). Only allow /part reasons in -m chans.
   */
  if (*reason && (!MyConnect(client) ||
      ((client->connection->created_monotonic +
        ConfigGeneral.anti_spam_exit_message_time) < event_base->time.sec_monotonic &&
       can_send(channel, client, member, reason, false) < 0)))
  {
    sendto_server(client, 0, 0, ":%s PART %s :%s",
                  client->id, channel->name, reason);
    sendto_channel_local(NULL, channel, 0, 0, 0, ":%s!%s@%s PART %s :%s",
                         client->name, client->username,
                         client->host, channel->name, reason);
  }
  else
  {
    sendto_server(client, 0, 0, ":%s PART %s",
                  client->id, channel->name);
    sendto_channel_local(NULL, channel, 0, 0, 0, ":%s!%s@%s PART %s",
                         client->name, client->username,
                         client->host, channel->name);
  }

  remove_user_from_channel(member);
}

void
channel_do_part(struct Client *client, char *channel, const char *reason)
{
  char *p = NULL;
  char buf[KICKLEN + 1] = "";  /* Essential that buf[0] = '\0' */

  assert(IsClient(client));

  if (!EmptyString(reason))
    strlcpy(buf, reason, sizeof(buf));

  for (const char *name = strtok_r(channel, ",", &p); name;
                   name = strtok_r(NULL,    ",", &p))
    channel_part_one_client(client, name, buf);
}
