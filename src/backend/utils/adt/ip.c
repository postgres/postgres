/*
 *	PostgreSQL type definitions for IP addresses.  This
 *	is for IP V4 CIDR notation, but prepared for V6: just
 *	add the necessary bits where the comments indicate.
 *
 *	$Id: ip.c,v 1.3 1998/10/04 15:35:10 momjian Exp $
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <stdio.h>
#include <errno.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <postgres.h>
#include <utils/palloc.h>
#include <utils/builtins.h>
#include <utils/mac.h>

/*
 *	Access macros.	Add IPV6 support.
 */

#define ip_addrsize(ipaddrptr) \
	(((ipaddr_struct *)VARDATA(ipaddrptr))->family == AF_INET ? 4 : -1)

#define ip_family(ipaddrptr) \
	(((ipaddr_struct *)VARDATA(ipaddrptr))->family)

#define ip_bits(ipaddrptr) \
	(((ipaddr_struct *)VARDATA(ipaddrptr))->bits)

#define ip_v4addr(ipaddrptr) \
	(((ipaddr_struct *)VARDATA(ipaddrptr))->addr.ipv4_addr)

/*
 *	IP address reader.
 */

ipaddr *
ipaddr_in(char *src)
{
	int			bits;
	ipaddr	   *dst;

	dst = palloc(VARHDRSZ + sizeof(ipaddr_struct));
	if (dst == NULL)
	{
		elog(ERROR, "unable to allocate memory in ipaddr_in()");
		return (NULL);
	}
	/* First, try for an IP V4 address: */
	ip_family(dst) = AF_INET;
	bits = inet_net_pton(ip_family(dst), src, &ip_v4addr(dst), ip_addrsize(dst));
	if ((bits < 0) || (bits > 32))
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "could not parse \"%s\"", src);
		pfree(dst);
		return (NULL);
	}
	VARSIZE(dst) = VARHDRSZ
		+ ((char *) &ip_v4addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);
	ip_bits(dst) = bits;
	return (dst);
}

/*
 *	IP address output function.
 */

char *
ipaddr_out(ipaddr *src)
{
	char	   *dst,
				tmp[sizeof("255.255.255.255/32")];

	if (ip_family(src) == AF_INET)
	{
		/* It's an IP V4 address: */
		if (inet_net_ntop(AF_INET, &ip_v4addr(src), ip_bits(src),
						  tmp, sizeof(tmp)) < 0)
		{
			elog(ERROR, "unable to print address (%s)", strerror(errno));
			return (NULL);
		}
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(src));
		return (NULL);
	}
	dst = palloc(strlen(tmp) + 1);
	if (dst == NULL)
	{
		elog(ERROR, "unable to allocate memory in ipaddr_out()");
		return (NULL);
	}
	strcpy(dst, tmp);
	return (dst);
}

/*
 *	Boolean tests for magnitude.  Add V4/V6 testing!
 */

bool
ipaddr_lt(ipaddr *a1, ipaddr *a2)
{
	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		int			order = v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a2));

		return ((order < 0) || ((order == 0) && (ip_bits(a1) < ip_bits(a2))));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return (FALSE);
	}
}

bool
ipaddr_le(ipaddr *a1, ipaddr *a2)
{
	return (ipaddr_lt(a1, a2) || ipaddr_eq(a1, a2));
}

bool
ipaddr_eq(ipaddr *a1, ipaddr *a2)
{
	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		return ((ip_bits(a1) == ip_bits(a2))
		 && (v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a1)) == 0));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return (FALSE);
	}
}

bool
ipaddr_ge(ipaddr *a1, ipaddr *a2)
{
	return (ipaddr_gt(a1, a2) || ipaddr_eq(a1, a2));
}

bool
ipaddr_gt(ipaddr *a1, ipaddr *a2)
{
	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		int			order = v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a2));

		return ((order > 0) || ((order == 0) && (ip_bits(a1) > ip_bits(a2))));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return (FALSE);
	}
}

bool
ipaddr_ne(ipaddr *a1, ipaddr *a2)
{
	return (!ipaddr_eq(a1, a2));
}

bool
ipaddr_sub(ipaddr *a1, ipaddr *a2)
{
	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		return ((ip_bits(a1) > ip_bits(a2))
		 && (v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a2)) == 0));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return (FALSE);
	}
}

bool
ipaddr_subeq(ipaddr *a1, ipaddr *a2)
{
	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		return ((ip_bits(a1) >= ip_bits(a2))
		 && (v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a2)) == 0));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return (FALSE);
	}
}

bool
ipaddr_sup(ipaddr *a1, ipaddr *a2)
{
	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		return ((ip_bits(a1) < ip_bits(a2))
		 && (v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a1)) == 0));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return (FALSE);
	}
}

bool
ipaddr_supeq(ipaddr *a1, ipaddr *a2)
{
	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		return ((ip_bits(a1) <= ip_bits(a2))
		 && (v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a1)) == 0));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return (FALSE);
	}
}

/*
 *	Comparison function for sorting.  Add V4/V6 testing!
 */

int4
ipaddr_cmp(ipaddr *a1, ipaddr *a2)
{
	if (ntohl(ip_v4addr(a1)) < ntohl(ip_v4addr(a2)))
		return (-1);
	else if (ntohl(ip_v4addr(a1)) > ntohl(ip_v4addr(a2)))
		return (1);
	return 0;
}

/*
 *	Bitwise comparison for V4 addresses.  Add V6 implementation!
 */

int
v4bitncmp(unsigned int a1, unsigned int a2, int bits)
{
	unsigned long mask = 0;
	int			i;

	for (i = 0; i < bits; i++)
		mask = (mask >> 1) | 0x80000000;
	a1 = ntohl(a1);
	a2 = ntohl(a2);
	if ((a1 & mask) < (a2 & mask))
		return (-1);
	else if ((a1 & mask) > (a2 & mask))
		return (1);
	return (0);
}
