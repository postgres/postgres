/*-------------------------------------------------------------------------
 *
 * ip.h
 *	  Definitions for IPv6-aware network access.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * $Id: ip.h,v 1.7 2003/06/12 07:00:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef IP_H
#define IP_H

#include "getaddrinfo.h"
#include "libpq/pqcomm.h"


extern int   getaddrinfo2(const char *hostname, const char *servname,
						  const struct addrinfo *hintp,
						  struct addrinfo **result);
extern void  freeaddrinfo2(int hint_ai_family, struct addrinfo *ai);

extern char *SockAddr_ntop(const SockAddr *sa, char *dst, size_t cnt,
						   int v4conv);
extern int   SockAddr_pton(SockAddr *sa, const char *src);

extern int   isAF_INETx(const int family);
extern int   rangeSockAddr(const SockAddr *addr, const SockAddr *netaddr,
						   const SockAddr *netmask);

#endif /* IP_H */
