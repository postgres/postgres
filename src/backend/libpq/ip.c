/*-------------------------------------------------------------------------
 *
 * ip.c
 *	  IPv6-aware network access.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/libpq/ip.c,v 1.23.2.2 2004/11/08 01:54:58 tgl Exp $
 *
 * This file and the IPV6 implementation were initially provided by
 * Nigel Kukard <nkukard@lbsd.net>, Linux Based Systems Design
 * http://www.lbsd.net.
 *
 *-------------------------------------------------------------------------
 */

/* This is intended to be used in both frontend and backend, so use c.h */
#include "c.h"

#if !defined(_MSC_VER) && !defined(__BORLANDC__)

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

#endif /* !defined(_MSC_VER) && !defined(__BORLANDC__) */

#include "libpq/ip.h"


static int rangeSockAddrAF_INET(const struct sockaddr_in * addr,
					 const struct sockaddr_in * netaddr,
					 const struct sockaddr_in * netmask);

#ifdef HAVE_IPV6
static int rangeSockAddrAF_INET6(const struct sockaddr_in6 * addr,
					  const struct sockaddr_in6 * netaddr,
					  const struct sockaddr_in6 * netmask);
#endif

#ifdef	HAVE_UNIX_SOCKETS
static int getaddrinfo_unix(const char *path,
				 const struct addrinfo * hintsp,
				 struct addrinfo ** result);

static int getnameinfo_unix(const struct sockaddr_un * sa, int salen,
				 char *node, int nodelen,
				 char *service, int servicelen,
				 int flags);
#endif


/*
 *	getaddrinfo_all - get address info for Unix, IPv4 and IPv6 sockets
 */
int
getaddrinfo_all(const char *hostname, const char *servname,
				const struct addrinfo *hintp, struct addrinfo **result)
{
	/* not all versions of getaddrinfo() zero *result on failure */
	*result = NULL;

#ifdef HAVE_UNIX_SOCKETS
	if (hintp != NULL && hintp->ai_family == AF_UNIX)
		return getaddrinfo_unix(servname, hintp, result);
#endif

	/* NULL has special meaning to getaddrinfo */
	return getaddrinfo((!hostname || hostname[0] == '\0') ? NULL : hostname,
					   servname, hintp, result);
}


/*
 *	freeaddrinfo_all - free addrinfo structures for IPv4, IPv6, or Unix
 *
 * Note: the ai_family field of the original hint structure must be passed
 * so that we can tell whether the addrinfo struct was built by the system's
 * getaddrinfo() routine or our own getaddrinfo_unix() routine.  Some versions
 * of getaddrinfo() might be willing to return AF_UNIX addresses, so it's
 * not safe to look at ai_family in the addrinfo itself.
 */
void
freeaddrinfo_all(int hint_ai_family, struct addrinfo * ai)
{
#ifdef HAVE_UNIX_SOCKETS
	if (hint_ai_family == AF_UNIX)
	{
		/* struct was built by getaddrinfo_unix (see getaddrinfo_all) */
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
	{
		/* struct was built by getaddrinfo() */
		if (ai != NULL)
			freeaddrinfo(ai);
	}
}


/*
 *	getnameinfo_all - get name info for Unix, IPv4 and IPv6 sockets
 *
 * The API of this routine differs from the standard getnameinfo() definition
 * in two ways: first, the addr parameter is declared as sockaddr_storage
 * rather than struct sockaddr, and second, the node and service fields are
 * guaranteed to be filled with something even on failure return.
 */
int
getnameinfo_all(const struct sockaddr_storage * addr, int salen,
				char *node, int nodelen,
				char *service, int servicelen,
				int flags)
{
	int			rc;

#ifdef HAVE_UNIX_SOCKETS
	if (addr && addr->ss_family == AF_UNIX)
		rc = getnameinfo_unix((const struct sockaddr_un *) addr, salen,
							  node, nodelen,
							  service, servicelen,
							  flags);
	else
#endif
		rc = getnameinfo((const struct sockaddr *) addr, salen,
						 node, nodelen,
						 service, servicelen,
						 flags);

	if (rc != 0)
	{
		if (node)
			StrNCpy(node, "???", nodelen);
		if (service)
			StrNCpy(service, "???", servicelen);
	}

	return rc;
}


#if defined(HAVE_UNIX_SOCKETS)

/* -------
 *	getaddrinfo_unix - get unix socket info using IPv6-compatible API
 *
 *	Bugs: only one addrinfo is set even though hintsp is NULL or
 *		  ai_socktype is 0
 *		  AI_CANONNAME is not supported.
 * -------
 */
static int
getaddrinfo_unix(const char *path, const struct addrinfo * hintsp,
				 struct addrinfo ** result)
{
	struct addrinfo hints;
	struct addrinfo *aip;
	struct sockaddr_un *unp;

	*result = NULL;

	MemSet(&hints, 0, sizeof(hints));

	if (strlen(path) >= sizeof(unp->sun_path))
		return EAI_FAIL;

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
		return EAI_FAIL;
	}

	aip = calloc(1, sizeof(struct addrinfo));
	if (aip == NULL)
		return EAI_MEMORY;

	unp = calloc(1, sizeof(struct sockaddr_un));
	if (unp == NULL)
	{
		free(aip);
		return EAI_MEMORY;
	}

	aip->ai_family = AF_UNIX;
	aip->ai_socktype = hints.ai_socktype;
	aip->ai_protocol = hints.ai_protocol;
	aip->ai_next = NULL;
	aip->ai_canonname = NULL;
	*result = aip;

	unp->sun_family = AF_UNIX;
	aip->ai_addr = (struct sockaddr *) unp;
	aip->ai_addrlen = sizeof(struct sockaddr_un);

	strcpy(unp->sun_path, path);

#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN
	unp->sun_len = sizeof(struct sockaddr_un);
#endif

	return 0;
}

/*
 * Convert an address to a hostname.
 */
static int
getnameinfo_unix(const struct sockaddr_un * sa, int salen,
				 char *node, int nodelen,
				 char *service, int servicelen,
				 int flags)
{
	int			ret = -1;

	/* Invalid arguments. */
	if (sa == NULL || sa->sun_family != AF_UNIX ||
		(node == NULL && service == NULL))
		return EAI_FAIL;

	/* We don't support those. */
	if ((node && !(flags & NI_NUMERICHOST))
		|| (service && !(flags & NI_NUMERICSERV)))
		return EAI_FAIL;

	if (node)
	{
		ret = snprintf(node, nodelen, "%s", "[local]");
		if (ret == -1 || ret > nodelen)
			return EAI_MEMORY;
	}

	if (service)
	{
		ret = snprintf(service, servicelen, "%s", sa->sun_path);
		if (ret == -1 || ret > servicelen)
			return EAI_MEMORY;
	}

	return 0;
}

#endif   /* HAVE_UNIX_SOCKETS */


/*
 * rangeSockAddr - is addr within the subnet specified by netaddr/netmask ?
 *
 * Note: caller must already have verified that all three addresses are
 * in the same address family; and AF_UNIX addresses are not supported.
 */
int
rangeSockAddr(const struct sockaddr_storage * addr,
			  const struct sockaddr_storage * netaddr,
			  const struct sockaddr_storage * netmask)
{
	if (addr->ss_family == AF_INET)
		return rangeSockAddrAF_INET((struct sockaddr_in *) addr,
									(struct sockaddr_in *) netaddr,
									(struct sockaddr_in *) netmask);
#ifdef HAVE_IPV6
	else if (addr->ss_family == AF_INET6)
		return rangeSockAddrAF_INET6((struct sockaddr_in6 *) addr,
									 (struct sockaddr_in6 *) netaddr,
									 (struct sockaddr_in6 *) netmask);
#endif
	else
		return 0;
}

static int
rangeSockAddrAF_INET(const struct sockaddr_in * addr,
					 const struct sockaddr_in * netaddr,
					 const struct sockaddr_in * netmask)
{
	if (((addr->sin_addr.s_addr ^ netaddr->sin_addr.s_addr) &
		 netmask->sin_addr.s_addr) == 0)
		return 1;
	else
		return 0;
}


#ifdef HAVE_IPV6
static int
rangeSockAddrAF_INET6(const struct sockaddr_in6 * addr,
					  const struct sockaddr_in6 * netaddr,
					  const struct sockaddr_in6 * netmask)
{
	int			i;

	for (i = 0; i < 16; i++)
	{
		if (((addr->sin6_addr.s6_addr[i] ^ netaddr->sin6_addr.s6_addr[i]) &
			 netmask->sin6_addr.s6_addr[i]) != 0)
			return 0;
	}

	return 1;
}

#endif

/*
 *	SockAddr_cidr_mask - make a network mask of the appropriate family
 *	  and required number of significant bits
 *
 * The resulting mask is placed in *mask, which had better be big enough.
 *
 * Return value is 0 if okay, -1 if not.
 */
int
SockAddr_cidr_mask(struct sockaddr_storage * mask, char *numbits, int family)
{
	long		bits;
	char	   *endptr;

	bits = strtol(numbits, &endptr, 10);

	if (*numbits == '\0' || *endptr != '\0')
		return -1;

	switch (family)
	{
		case AF_INET:
			{
				struct sockaddr_in mask4;
				long		maskl;

				if (bits < 0 || bits > 32)
					return -1;
				/* avoid "x << 32", which is not portable */
				if (bits > 0)
					maskl = (0xffffffffUL << (32 - (int) bits))
						& 0xffffffffUL;
				else
					maskl = 0;
				mask4.sin_addr.s_addr = htonl(maskl);
				memcpy(mask, &mask4, sizeof(mask4));
				break;
			}

#ifdef HAVE_IPV6
		case AF_INET6:
			{
				struct sockaddr_in6 mask6;
				int			i;

				if (bits < 0 || bits > 128)
					return -1;
				for (i = 0; i < 16; i++)
				{
					if (bits <= 0)
						mask6.sin6_addr.s6_addr[i] = 0;
					else if (bits >= 8)
						mask6.sin6_addr.s6_addr[i] = 0xff;
					else
					{
						mask6.sin6_addr.s6_addr[i] =
							(0xff << (8 - (int) bits)) & 0xff;
					}
					bits -= 8;
				}
				memcpy(mask, &mask6, sizeof(mask6));
				break;
			}
#endif
		default:
			return -1;
	}

	mask->ss_family = family;
	return 0;
}


#ifdef HAVE_IPV6

/*
 * promote_v4_to_v6_addr --- convert an AF_INET addr to AF_INET6, using
 *		the standard convention for IPv4 addresses mapped into IPv6 world
 *
 * The passed addr is modified in place; be sure it is large enough to
 * hold the result!  Note that we only worry about setting the fields
 * that rangeSockAddr will look at.
 */
void
promote_v4_to_v6_addr(struct sockaddr_storage * addr)
{
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	uint32		ip4addr;

	memcpy(&addr4, addr, sizeof(addr4));
	ip4addr = ntohl(addr4.sin_addr.s_addr);

	memset(&addr6, 0, sizeof(addr6));

	addr6.sin6_family = AF_INET6;

	addr6.sin6_addr.s6_addr[10] = 0xff;
	addr6.sin6_addr.s6_addr[11] = 0xff;
	addr6.sin6_addr.s6_addr[12] = (ip4addr >> 24) & 0xFF;
	addr6.sin6_addr.s6_addr[13] = (ip4addr >> 16) & 0xFF;
	addr6.sin6_addr.s6_addr[14] = (ip4addr >> 8) & 0xFF;
	addr6.sin6_addr.s6_addr[15] = (ip4addr) & 0xFF;

	memcpy(addr, &addr6, sizeof(addr6));
}

/*
 * promote_v4_to_v6_mask --- convert an AF_INET netmask to AF_INET6, using
 *		the standard convention for IPv4 addresses mapped into IPv6 world
 *
 * This must be different from promote_v4_to_v6_addr because we want to
 * set the high-order bits to 1's not 0's.
 *
 * The passed addr is modified in place; be sure it is large enough to
 * hold the result!  Note that we only worry about setting the fields
 * that rangeSockAddr will look at.
 */
void
promote_v4_to_v6_mask(struct sockaddr_storage * addr)
{
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	uint32		ip4addr;
	int			i;

	memcpy(&addr4, addr, sizeof(addr4));
	ip4addr = ntohl(addr4.sin_addr.s_addr);

	memset(&addr6, 0, sizeof(addr6));

	addr6.sin6_family = AF_INET6;

	for (i = 0; i < 12; i++)
		addr6.sin6_addr.s6_addr[i] = 0xff;

	addr6.sin6_addr.s6_addr[12] = (ip4addr >> 24) & 0xFF;
	addr6.sin6_addr.s6_addr[13] = (ip4addr >> 16) & 0xFF;
	addr6.sin6_addr.s6_addr[14] = (ip4addr >> 8) & 0xFF;
	addr6.sin6_addr.s6_addr[15] = (ip4addr) & 0xFF;

	memcpy(addr, &addr6, sizeof(addr6));
}

#endif /* HAVE_IPV6 */
