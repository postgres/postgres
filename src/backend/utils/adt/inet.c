/*
 *	PostgreSQL type definitions for the INET type.  This
 *	is for IP V4 CIDR notation, but prepared for V6: just
 *	add the necessary bits where the comments indicate.
 *
 *	$Id: inet.c,v 1.2 1998/10/08 02:08:44 momjian Exp $
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <postgres.h>
#include <utils/palloc.h>
#include <utils/builtins.h>
#include <utils/inet.h>

static int v4bitncmp(unsigned int a1, unsigned int a2, int bits);

/*
 *	Access macros.	Add IPV6 support.
 */

#define ip_addrsize(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->family == AF_INET ? 4 : -1)

#define ip_family(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->family)

#define ip_bits(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->bits)

#define ip_v4addr(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->addr.ipv4_addr)

/*
 *	IP address reader.
 */

inet *
inet_in(char *src)
{
	int			bits;
	inet	   *dst;

	dst = palloc(VARHDRSZ + sizeof(inet_struct));
	if (dst == NULL)
	{
		elog(ERROR, "unable to allocate memory in inet_in()");
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
inet_out(inet *src)
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
		elog(ERROR, "unable to allocate memory in inet_out()");
		return (NULL);
	}
	strcpy(dst, tmp);
	return (dst);
}

/*
 *	Boolean tests for magnitude.  Add V4/V6 testing!
 */

bool
inet_lt(inet *a1, inet *a2)
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
inet_le(inet *a1, inet *a2)
{
	return (inet_lt(a1, a2) || inet_eq(a1, a2));
}

bool
inet_eq(inet *a1, inet *a2)
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
inet_ge(inet *a1, inet *a2)
{
	return (inet_gt(a1, a2) || inet_eq(a1, a2));
}

bool
inet_gt(inet *a1, inet *a2)
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
inet_ne(inet *a1, inet *a2)
{
	return (!inet_eq(a1, a2));
}

bool
inet_sub(inet *a1, inet *a2)
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
inet_subeq(inet *a1, inet *a2)
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
inet_sup(inet *a1, inet *a2)
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
inet_supeq(inet *a1, inet *a2)
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
inet_cmp(inet *a1, inet *a2)
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

static int
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
