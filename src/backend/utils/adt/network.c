/*
 *	PostgreSQL type definitions for the INET type.	This
 *	is for IP V4 CIDR notation, but prepared for V6: just
 *	add the necessary bits where the comments indicate.
 *
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/network.c,v 1.24 2000/08/03 23:07:46 tgl Exp $
 *
 *	Jon Postel RIP 16 Oct 1998
 */

#include "postgres.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "utils/builtins.h"
#include "utils/inet.h"


static int	v4bitncmp(unsigned int a1, unsigned int a2, int bits);
static int32 network_cmp_internal(inet *a1, inet *a2);

/*
 *	Access macros.	Add IPV6 support.
 */

#define ip_addrsize(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->family == AF_INET ? 4 : -1)

#define ip_family(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->family)

#define ip_bits(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->bits)

#define ip_type(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->type)

#define ip_v4addr(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->addr.ipv4_addr)

/* Common input routine */
static inet *
network_in(char *src, int type)
{
	int			bits;
	inet	   *dst;

	dst = (inet *) palloc(VARHDRSZ + sizeof(inet_struct));

	/* First, try for an IP V4 address: */
	ip_family(dst) = AF_INET;
	bits = inet_net_pton(ip_family(dst), src, &ip_v4addr(dst),
						 type ? ip_addrsize(dst) : -1);
	if ((bits < 0) || (bits > 32))
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "could not parse \"%s\"", src);

	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) &ip_v4addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);
	ip_bits(dst) = bits;
	ip_type(dst) = type;

	return dst;
}

/* INET address reader.  */
Datum
inet_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, 0));
}

/* CIDR address reader.  */
Datum
cidr_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, 1));
}

/*
 *	INET address output function.
 */
Datum
inet_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_P(0);
	char		tmp[sizeof("255.255.255.255/32")];
	char	   *dst;

	if (ip_family(src) == AF_INET)
	{
		/* It's an IP V4 address: */
		if (ip_type(src))
			dst = inet_cidr_ntop(AF_INET, &ip_v4addr(src), ip_bits(src),
								 tmp, sizeof(tmp));
		else
			dst = inet_net_ntop(AF_INET, &ip_v4addr(src), ip_bits(src),
								tmp, sizeof(tmp));

		if (dst == NULL)
			elog(ERROR, "unable to print address (%s)", strerror(errno));
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(src));

	PG_RETURN_CSTRING(pstrdup(tmp));
}


/* share code with INET case */
Datum
cidr_out(PG_FUNCTION_ARGS)
{
	return inet_out(fcinfo);
}


/*
 *	Basic comparison function for sorting and inet/cidr comparisons.
 *
 * XXX this ignores bits to the right of the mask.  That's probably
 * correct for CIDR, almost certainly wrong for INET.  We need to have
 * two sets of comparator routines, not just one.  Note that suggests
 * that CIDR and INET should not be considered binary-equivalent by
 * the parser?
 */

static int32
network_cmp_internal(inet *a1, inet *a2)
{
	if (ip_family(a1) == AF_INET && ip_family(a2) == AF_INET)
	{
		int		order = v4bitncmp(ip_v4addr(a1), ip_v4addr(a2),
								  Min(ip_bits(a1), ip_bits(a2)));

		if (order != 0)
			return order;
		return ((int32) ip_bits(a1)) - ((int32) ip_bits(a2));
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		return 0;				/* keep compiler quiet */
	}
}

Datum
network_cmp(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	PG_RETURN_INT32(network_cmp_internal(a1, a2));
}

/*
 *	Boolean ordering tests.
 */
Datum
network_lt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) < 0);
}

Datum
network_le(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) <= 0);
}

Datum
network_eq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) == 0);
}

Datum
network_ge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) >= 0);
}

Datum
network_gt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) > 0);
}

Datum
network_ne(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) != 0);
}

/*
 *	Boolean network-inclusion tests.
 */
Datum
network_sub(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		PG_RETURN_BOOL(ip_bits(a1) > ip_bits(a2)
			&& v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a2)) == 0);
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		PG_RETURN_BOOL(false);
	}
}

Datum
network_subeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		PG_RETURN_BOOL(ip_bits(a1) >= ip_bits(a2)
			&& v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a2)) == 0);
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		PG_RETURN_BOOL(false);
	}
}

Datum
network_sup(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		PG_RETURN_BOOL(ip_bits(a1) < ip_bits(a2)
			&& v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a1)) == 0);
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		PG_RETURN_BOOL(false);
	}
}

Datum
network_supeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	if ((ip_family(a1) == AF_INET) && (ip_family(a2) == AF_INET))
	{
		PG_RETURN_BOOL(ip_bits(a1) <= ip_bits(a2)
			&& v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), ip_bits(a1)) == 0);
	}
	else
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "cannot compare address families %d and %d",
			 ip_family(a1), ip_family(a2));
		PG_RETURN_BOOL(false);
	}
}

/*
 * Extract data from a network datatype.
 */
Datum
network_host(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	text	   *ret;
	int			len;
	char	   *ptr,
				tmp[sizeof("255.255.255.255/32")];

	if (ip_type(ip))
		elog(ERROR, "CIDR type has no host part");

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		if (inet_net_ntop(AF_INET, &ip_v4addr(ip), 32, tmp, sizeof(tmp)) == NULL)
			elog(ERROR, "unable to print host (%s)", strerror(errno));
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	/* Suppress /n if present */
	if ((ptr = strchr(tmp, '/')) != NULL)
		*ptr = '\0';

	/* Return string as a text datum */
	len = strlen(tmp);
	ret = (text *) palloc(len + VARHDRSZ);
	VARATT_SIZEP(ret) = len + VARHDRSZ;
	memcpy(VARDATA(ret), tmp, len);
	PG_RETURN_TEXT_P(ret);
}

Datum
network_masklen(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);

	PG_RETURN_INT32(ip_bits(ip));
}

Datum
network_broadcast(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	text	   *ret;
	int			len;
	char	   *ptr,
				tmp[sizeof("255.255.255.255/32")];

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		int			addr;
		unsigned long mask = 0xffffffff;

		if (ip_bits(ip) < 32)
			mask >>= ip_bits(ip);
		addr = htonl(ntohl(ip_v4addr(ip)) | mask);

		if (inet_net_ntop(AF_INET, &addr, 32, tmp, sizeof(tmp)) == NULL)
			elog(ERROR, "unable to print address (%s)", strerror(errno));
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	/* Suppress /n if present */
	if ((ptr = strchr(tmp, '/')) != NULL)
		*ptr = '\0';

	/* Return string as a text datum */
	len = strlen(tmp);
	ret = (text *) palloc(len + VARHDRSZ);
	VARATT_SIZEP(ret) = len + VARHDRSZ;
	memcpy(VARDATA(ret), tmp, len);
	PG_RETURN_TEXT_P(ret);
}

Datum
network_network(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	text	   *ret;
	int			len;
	char		tmp[sizeof("255.255.255.255/32")];

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		int			addr = htonl(ntohl(ip_v4addr(ip)) & (0xffffffff << (32 - ip_bits(ip))));

		if (inet_cidr_ntop(AF_INET, &addr, ip_bits(ip), tmp, sizeof(tmp)) == NULL)
			elog(ERROR, "unable to print network (%s)", strerror(errno));
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	/* Return string as a text datum */
	len = strlen(tmp);
	ret = (text *) palloc(len + VARHDRSZ);
	VARATT_SIZEP(ret) = len + VARHDRSZ;
	memcpy(VARDATA(ret), tmp, len);
	PG_RETURN_TEXT_P(ret);
}

Datum
network_netmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	text	   *ret;
	int			len;
	char	   *ptr,
				tmp[sizeof("255.255.255.255/32")];

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		int			addr = htonl(ip_bits(ip) ?
				   (-1 << (32 - ip_bits(ip))) & 0xffffffff : 0x00000000);

		if (inet_net_ntop(AF_INET, &addr, 32, tmp, sizeof(tmp)) == NULL)
			elog(ERROR, "unable to print netmask (%s)", strerror(errno));
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	/* Suppress /n if present */
	if ((ptr = strchr(tmp, '/')) != NULL)
		*ptr = '\0';

	/* Return string as a text datum */
	len = strlen(tmp);
	ret = (text *) palloc(len + VARHDRSZ);
	VARATT_SIZEP(ret) = len + VARHDRSZ;
	memcpy(VARDATA(ret), tmp, len);
	PG_RETURN_TEXT_P(ret);
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
