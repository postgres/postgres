/*-------------------------------------------------------------------------
 *
 * ip.c
 *	  IPv6-aware network access.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/ip.c,v 1.8 2003/06/08 17:42:59 tgl Exp $
 *
 * This file and the IPV6 implementation were initially provided by
 * Nigel Kukard <nkukard@lbsd.net>, Linux Based Systems Design
 * http://www.lbsd.net.
 *
 *-------------------------------------------------------------------------
 */

/* This is intended to be used in both frontend and backend, so use c.h */
#include "c.h"

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#include <sys/file.h>

#include "libpq/ip.h"


static int   rangeSockAddrAF_INET(const SockAddr *addr,
								  const SockAddr *netaddr,
								  const SockAddr *netmask);

#ifdef HAVE_IPV6
static int   rangeSockAddrAF_INET6(const SockAddr *addr,
								   const SockAddr *netaddr,
								   const SockAddr *netmask);
static void  convSockAddr6to4(const SockAddr *src, SockAddr *dst);
#endif

#ifdef HAVE_UNIX_SOCKETS
static int getaddrinfo_unix(const char *path, const struct addrinfo *hintsp,
							struct addrinfo **result);
#endif


/*
 *	getaddrinfo2 - get address info for Unix, IPv4 and IPv6 sockets
 */
int
getaddrinfo2(const char *hostname, const char *servname,
			 const struct addrinfo *hintp, struct addrinfo **result)
{
#ifdef HAVE_UNIX_SOCKETS
	if (hintp != NULL && hintp->ai_family == AF_UNIX)
		return getaddrinfo_unix(servname, hintp, result);
#endif

	/* NULL has special meaning to getaddrinfo */
	return getaddrinfo((!hostname || hostname[0] == '\0') ? NULL : hostname,
					   servname, hintp, result);
}


/*
 *	freeaddrinfo2 - free addrinfo structures for IPv4, IPv6, or Unix
 */
void
freeaddrinfo2(struct addrinfo *ai)
{
	if (ai != NULL)
	{
#ifdef HAVE_UNIX_SOCKETS
		if (ai->ai_family == AF_UNIX)
		{
			while (ai != NULL)
			{
				struct addrinfo *p = ai;

				ai = ai->ai_next;
				free(p->ai_addr);
				free(p);
			}
		}
		else
#endif   /* HAVE_UNIX_SOCKETS */
			freeaddrinfo(ai);
	}
}


#if defined(HAVE_UNIX_SOCKETS)
/* -------
 *	getaddrinfo_unix - get unix socket info using IPv6-compatible API
 *
 *	Bug:  only one addrinfo is set even though hintsp is NULL or
 *		  ai_socktype is 0
 *		  AI_CANONNAME is not supported.
 * -------
 */
static int
getaddrinfo_unix(const char *path, const struct addrinfo *hintsp,
				 struct addrinfo **result)
{
	struct addrinfo hints;
	struct addrinfo *aip;
	struct sockaddr_un *unp;

	MemSet(&hints, 0, sizeof(hints));

	if (hintsp == NULL)
	{
		hints.ai_family = AF_UNIX;
		hints.ai_socktype = SOCK_STREAM;
	}
	else
		memcpy(&hints, hintsp, sizeof(hints));

	if (hints.ai_socktype == 0)
		hints.ai_socktype = SOCK_STREAM;

	if (hints.ai_family != AF_UNIX)
	{
		/* shouldn't have been called */
		return EAI_ADDRFAMILY;
	}

	aip = calloc(1, sizeof(struct addrinfo));
	if (aip == NULL)
		return EAI_MEMORY;

	aip->ai_family = AF_UNIX;
	aip->ai_socktype = hints.ai_socktype;
	aip->ai_protocol = hints.ai_protocol;
	aip->ai_next = NULL;
	aip->ai_canonname = NULL;
	*result = aip;

	unp = calloc(1, sizeof(struct sockaddr_un));
	if (aip == NULL)
		return EAI_MEMORY;

	unp->sun_family = AF_UNIX;
	aip->ai_addr = (struct sockaddr *) unp;
	aip->ai_addrlen = sizeof(struct sockaddr_un);

	if (strlen(path) >= sizeof(unp->sun_path))
		return EAI_SERVICE;
	strcpy(unp->sun_path, path);

#if SALEN
	unp->sun_len = sizeof(struct sockaddr_un);
#endif   /* SALEN */

	return 0;
}
#endif   /* HAVE_UNIX_SOCKETS */

/* ----------
 * SockAddr_ntop - set IP address string from SockAddr
 *
 * parameters...  sa	: SockAddr union
 *				  dst	: buffer for address string
 *				  cnt	: sizeof dst
 *				  v4conv: non-zero: if address is IPv4 mapped IPv6 address then
 *						  convert to IPv4 address.
 * returns... pointer to dst
 * if sa.sa_family is not AF_INET or AF_INET6 dst is set as empy string.
 * ----------
 */
char *
SockAddr_ntop(const SockAddr *sa, char *dst, size_t cnt, int v4conv)
{
	switch (sa->sa.sa_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
			inet_ntop(AF_INET, &sa->in.sin_addr, dst, cnt);
#else
			StrNCpy(dst, inet_ntoa(sa->in.sin_addr), cnt);
#endif
			break;
#ifdef HAVE_IPV6
		case AF_INET6:
			inet_ntop(AF_INET6, &sa->in6.sin6_addr, dst, cnt);
			if (v4conv && IN6_IS_ADDR_V4MAPPED(&sa->in6.sin6_addr))
				strcpy(dst, dst + 7);
			break;
#endif
		default:
			dst[0] = '\0';
			break;
	}
	return dst;
}


/*
 *	SockAddr_pton - IPv6 pton
 */
int
SockAddr_pton(SockAddr *sa, const char *src)
{
	int			family = AF_INET;

#ifdef HAVE_IPV6
	if (strchr(src, ':'))
		family = AF_INET6;
#endif

	sa->sa.sa_family = family;

	switch (family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
			return inet_pton(AF_INET, src, &sa->in.sin_addr);
#else
			return inet_aton(src, &sa->in.sin_addr);
#endif

#ifdef HAVE_IPV6
		case AF_INET6:
			return inet_pton(AF_INET6, src, &sa->in6.sin6_addr);
			break;
#endif
		default:
			return -1;
	}
}


/*
 *	isAF_INETx - check to see if sa is AF_INET or AF_INET6
 */
int
isAF_INETx(const int family)
{
	if (family == AF_INET
#ifdef HAVE_IPV6
		|| family == AF_INET6
#endif
		)
		return 1;
	else
		return 0;
}


int
rangeSockAddr(const SockAddr *addr, const SockAddr *netaddr,
			  const SockAddr *netmask)
{
	if (addr->sa.sa_family == AF_INET)
		return rangeSockAddrAF_INET(addr, netaddr, netmask);
#ifdef HAVE_IPV6
	else if (addr->sa.sa_family == AF_INET6)
		return rangeSockAddrAF_INET6(addr, netaddr, netmask);
#endif
	else
		return 0;
}

static int
rangeSockAddrAF_INET(const SockAddr *addr, const SockAddr *netaddr,
					 const SockAddr *netmask)
{
	if (addr->sa.sa_family != AF_INET ||
		netaddr->sa.sa_family != AF_INET ||
		netmask->sa.sa_family != AF_INET)
		return 0;
	if (((addr->in.sin_addr.s_addr ^ netaddr->in.sin_addr.s_addr) &
		 netmask->in.sin_addr.s_addr) == 0)
		return 1;
	else
		return 0;
}


#ifdef HAVE_IPV6

static int
rangeSockAddrAF_INET6(const SockAddr *addr, const SockAddr *netaddr,
					  const SockAddr *netmask)
{
	int			i;

	if (IN6_IS_ADDR_V4MAPPED(&addr->in6.sin6_addr))
	{
		SockAddr	addr4;

		convSockAddr6to4(addr, &addr4);
		if (rangeSockAddrAF_INET(&addr4, netaddr, netmask))
			return 1;
	}

	if (netaddr->sa.sa_family != AF_INET6 ||
		netmask->sa.sa_family != AF_INET6)
		return 0;

	for (i = 0; i < 16; i++)
	{
		if (((addr->in6.sin6_addr.s6_addr[i] ^ netaddr->in6.sin6_addr.s6_addr[i]) &
			 netmask->in6.sin6_addr.s6_addr[i]) != 0)
			return 0;
	}

	return 1;
}

static void
convSockAddr6to4(const SockAddr *src, SockAddr *dst)
{
	MemSet(dst, 0, sizeof(*dst));
	dst->in.sin_family = AF_INET;
	/* both src and dst are assumed to be in network byte order */
	dst->in.sin_port = src->in6.sin6_port;
	memcpy(&dst->in.sin_addr.s_addr,
		   ((char *) (&src->in6.sin6_addr.s6_addr)) + 12,
		   sizeof(struct in_addr));
}

#endif /* HAVE_IPV6 */
