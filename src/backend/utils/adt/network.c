/*
 *	PostgreSQL type definitions for the INET type.	This
 *	is for IP V4 CIDR notation, but prepared for V6: just
 *	add the necessary bits where the comments indicate.
 *
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/network.c,v 1.27 2000/11/25 21:30:54 tgl Exp $
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


static int32 network_cmp_internal(inet *a1, inet *a2);
static int v4bitncmp(unsigned long a1, unsigned long a2, int bits);
static bool v4addressOK(unsigned long a1, int bits);

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
	/* make sure any unused bits in a CIDR value are zeroed */
	MemSet(dst, 0, VARHDRSZ + sizeof(inet_struct));

	/* First, try for an IP V4 address: */
	ip_family(dst) = AF_INET;
	bits = inet_net_pton(ip_family(dst), src, &ip_v4addr(dst),
						 type ? ip_addrsize(dst) : -1);
	if ((bits < 0) || (bits > 32))
	{
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "invalid %s value '%s'",
			 type ? "CIDR" : "INET", src);
	}

	/*
	 * Error check: CIDR values must not have any bits set beyond the masklen.
	 * XXX this code is not IPV6 ready.
	 */
	if (type)
	{
		if (! v4addressOK(ip_v4addr(dst), bits))
			elog(ERROR, "invalid CIDR value '%s': has bits set to right of mask", src);
	}

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
 * Comparison is first on the common bits of the network part, then on
 * the length of the network part, and then on the whole unmasked address.
 * The effect is that the network part is the major sort key, and for
 * equal network parts we sort on the host part.  Note this is only sane
 * for CIDR if address bits to the right of the mask are guaranteed zero;
 * otherwise logically-equal CIDRs might compare different.
 */

static int32
network_cmp_internal(inet *a1, inet *a2)
{
	if (ip_family(a1) == AF_INET && ip_family(a2) == AF_INET)
	{
		int		order;

		order = v4bitncmp(ip_v4addr(a1), ip_v4addr(a2),
						  Min(ip_bits(a1), ip_bits(a2)));
		if (order != 0)
			return order;
		order = ((int) ip_bits(a1)) - ((int) ip_bits(a2));
		if (order != 0)
			return order;
		return v4bitncmp(ip_v4addr(a1), ip_v4addr(a2), 32);
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

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		/* force display of 32 bits, regardless of masklen... */
		if (inet_net_ntop(AF_INET, &ip_v4addr(ip), 32, tmp, sizeof(tmp)) == NULL)
			elog(ERROR, "unable to print host (%s)", strerror(errno));
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	/* Suppress /n if present (shouldn't happen now) */
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
network_show(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	text	   *ret;
	int			len;
	char		tmp[sizeof("255.255.255.255/32")];

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		/* force display of 32 bits, regardless of masklen... */
		if (inet_net_ntop(AF_INET, &ip_v4addr(ip), 32, tmp, sizeof(tmp)) == NULL)
			elog(ERROR, "unable to print host (%s)", strerror(errno));
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	/* Add /n if not present */
	if (strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(ip));
	}

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
	inet	   *dst;

	dst = (inet *) palloc(VARHDRSZ + sizeof(inet_struct));
	/* make sure any unused bits are zeroed */
	MemSet(dst, 0, VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		unsigned long mask = 0xffffffff;

		/* Shifting by 32 or more bits does not yield portable results,
		 * so don't try it.
		 */
		if (ip_bits(ip) < 32)
			mask >>= ip_bits(ip);
		else
			mask = 0;

		ip_v4addr(dst) = htonl(ntohl(ip_v4addr(ip)) | mask);
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	ip_type(dst) = 0;
	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) &ip_v4addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_network(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	inet	   *dst;

	dst = (inet *) palloc(VARHDRSZ + sizeof(inet_struct));
	/* make sure any unused bits are zeroed */
	MemSet(dst, 0, VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		unsigned long mask = 0xffffffff;

		/* Shifting by 32 or more bits does not yield portable results,
		 * so don't try it.
		 */
		if (ip_bits(ip) > 0)
			mask <<= (32 - ip_bits(ip));
		else
			mask = 0;

		ip_v4addr(dst) = htonl(ntohl(ip_v4addr(ip)) & mask);
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	ip_type(dst) = 1;
	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) &ip_v4addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_netmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	inet	   *dst;

	dst = (inet *) palloc(VARHDRSZ + sizeof(inet_struct));
	/* make sure any unused bits are zeroed */
	MemSet(dst, 0, VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		unsigned long mask = 0xffffffff;

		/* Shifting by 32 or more bits does not yield portable results,
		 * so don't try it.
		 */
		if (ip_bits(ip) > 0)
			mask <<= (32 - ip_bits(ip));
		else
			mask = 0;

		ip_v4addr(dst) = htonl(mask);

		ip_bits(dst) = 32;
	}
	else
		/* Go for an IPV6 address here, before faulting out: */
		elog(ERROR, "unknown address family (%d)", ip_family(ip));

	ip_family(dst) = ip_family(ip);
	ip_type(dst) = 0;
	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) &ip_v4addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);

	PG_RETURN_INET_P(dst);
}

/*
 *	Bitwise comparison for V4 addresses.  Add V6 implementation!
 */

static int
v4bitncmp(unsigned long a1, unsigned long a2, int bits)
{
	unsigned long mask;

	/* Shifting by 32 or more bits does not yield portable results,
	 * so don't try it.
	 */
	if (bits > 0)
		mask = (0xFFFFFFFFL << (32 - bits)) & 0xFFFFFFFFL;
	else
		mask = 0;
	a1 = ntohl(a1);
	a2 = ntohl(a2);
	if ((a1 & mask) < (a2 & mask))
		return (-1);
	else if ((a1 & mask) > (a2 & mask))
		return (1);
	return (0);
}

/*
 * Returns true if given address fits fully within the specified bit width.
 */
static bool
v4addressOK(unsigned long a1, int bits)
{
	unsigned long mask;

	/* Shifting by 32 or more bits does not yield portable results,
	 * so don't try it.
	 */
	if (bits > 0)
		mask = (0xFFFFFFFFL << (32 - bits)) & 0xFFFFFFFFL;
	else
		mask = 0;
	a1 = ntohl(a1);
	if ((a1 & mask) == a1)
		return true;
	return false;
}
