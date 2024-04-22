/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 2001-2024 ircd-hybrid development team
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

/*! \file hostmask.h
 * \brief A header for the hostmask code.
 */

#ifndef INCLUDED_hostmask_h
#define INCLUDED_hostmask_h

enum hostmask_type
{
  HM_HOST,
  HM_IPV4,
  HM_IPV6
};

extern uint32_t hash_ipv4(const struct irc_ssaddr *, int);
extern uint32_t hash_ipv6(const struct irc_ssaddr *, int);
extern uint32_t hash_text(const char *);
extern uint32_t get_mask_hash(const char *);
extern int parse_netmask(const char *, struct irc_ssaddr *, int *);
extern void address_mask(struct irc_ssaddr *, int);
extern bool address_compare(const void *, const void *, bool, bool, int);
extern bool match_ipv6(const struct irc_ssaddr *, const struct irc_ssaddr *, int);
extern bool match_ipv4(const struct irc_ssaddr *, const struct irc_ssaddr *, int);
#endif  /* INCLUDED_hostmask_h */
