/*
 *	PostgreSQL type definitions for the INET type.	This
 *	is for IP V4 CIDR notation, but prepared for V6: just
 *	add the necessary bits where the comments indicate.
 *
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/network.c,v 1.41 2003/05/13 18:03:07 tgl Exp $
 *
 *	Jon Postel RIP 16 Oct 1998
 */

#include "postgres.h"

#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/inet.h"


static Datum text_network(text *src, int type);
static int32 network_cmp_internal(inet *a1, inet *a2);
static int	v4bitncmp(unsigned long a1, unsigned long a2, int bits);
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

	/* make sure any unused bits in a CIDR value are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

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
	 * Error check: CIDR values must not have any bits set beyond the
	 * masklen. XXX this code is not IPV6 ready.
	 */
	if (type)
	{
		if (!v4addressOK(ip_v4addr(dst), bits))
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
	int			len;

	if (ip_family(src) == AF_INET)
	{
		/* It's an IP V4 address: */

		/*
		 * Use inet style for both inet and cidr, since we don't want
		 * abbreviated CIDR style here.
		 */
		dst = inet_net_ntop(AF_INET, &ip_v4addr(src), ip_bits(src),
							tmp, sizeof(tmp));
		if (dst == NULL)
			elog(ERROR, "unable to print address (%s)", strerror(errno));
		/* For CIDR, add /n if not present */
		if (ip_type(src) && strchr(tmp, '/') == NULL)
		{
			len = strlen(tmp);
			snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(src));
		}
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
 *		inet_recv			- converts external binary format to inet
 *
 * The external representation is (one byte apiece for)
 * family, bits, type, address length, address in network byte order.
 */
Datum
inet_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	inet	   *addr;
	char	   *addrptr;
	int			bits;
	int			nb,
				i;

	/* make sure any unused bits in a CIDR value are zeroed */
	addr = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	ip_family(addr) = pq_getmsgbyte(buf);
	if (ip_family(addr) != AF_INET)
		elog(ERROR, "Invalid family in external inet");
	bits = pq_getmsgbyte(buf);
	if (bits < 0 || bits > 32)
		elog(ERROR, "Invalid bits in external inet");
	ip_bits(addr) = bits;
	ip_type(addr) = pq_getmsgbyte(buf);
	if (ip_type(addr) != 0 && ip_type(addr) != 1)
		elog(ERROR, "Invalid type in external inet");
	nb = pq_getmsgbyte(buf);
	if (nb != ip_addrsize(addr))
		elog(ERROR, "Invalid length in external inet");

	VARATT_SIZEP(addr) = VARHDRSZ
		+ ((char *) &ip_v4addr(addr) - (char *) VARDATA(addr))
		+ ip_addrsize(addr);

	addrptr = (char *) &ip_v4addr(addr);
	for (i = 0; i < nb; i++)
		addrptr[i] = pq_getmsgbyte(buf);

	/*
	 * Error check: CIDR values must not have any bits set beyond the
	 * masklen. XXX this code is not IPV6 ready.
	 */
	if (ip_type(addr))
	{
		if (!v4addressOK(ip_v4addr(addr), bits))
			elog(ERROR, "invalid external CIDR value: has bits set to right of mask");
	}

	PG_RETURN_INET_P(addr);
}

/* share code with INET case */
Datum
cidr_recv(PG_FUNCTION_ARGS)
{
	return inet_recv(fcinfo);
}

/*
 *		inet_send			- converts inet to binary format
 */
Datum
inet_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_P(0);
	StringInfoData buf;
	char	   *addrptr;
	int			nb,
				i;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, ip_family(addr));
	pq_sendbyte(&buf, ip_bits(addr));
	pq_sendbyte(&buf, ip_type(addr));
	nb = ip_addrsize(addr);
	if (nb < 0)
		nb = 0;
	pq_sendbyte(&buf, nb);
	addrptr = (char *) &ip_v4addr(addr);
	for (i = 0; i < nb; i++)
		pq_sendbyte(&buf, addrptr[i]);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* share code with INET case */
Datum
cidr_send(PG_FUNCTION_ARGS)
{
	return inet_send(fcinfo);
}


static Datum
text_network(text *src, int type)
{
	int			len = VARSIZE(src) - VARHDRSZ;

	char	   *str = palloc(len + 1);

	memcpy(str, VARDATA(src), len);
	*(str + len) = '\0';

	PG_RETURN_INET_P(network_in(str, type));
}

Datum
text_cidr(PG_FUNCTION_ARGS)
{
	return text_network(PG_GETARG_TEXT_P(0), 1);
}

Datum
text_inet(PG_FUNCTION_ARGS)
{
	return text_network(PG_GETARG_TEXT_P(0), 0);
}

Datum
inet_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_P(0);
	int			bits = PG_GETARG_INT32(1);
	inet	   *dst;

	if ((bits < 0) || (bits > 32))		/* no support for v6 yet */
		elog(ERROR, "set_masklen - invalid value '%d'", bits);

	/* clone the original data */
	dst = (inet *) palloc(VARHDRSZ + sizeof(inet_struct));
	memcpy(dst, src, VARHDRSZ + sizeof(inet_struct));

	ip_bits(dst) = bits;

	PG_RETURN_INET_P(dst);
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
		int			order;

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
		/* Add /n if not present (which it won't be) */
		if (strchr(tmp, '/') == NULL)
		{
			len = strlen(tmp);
			snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(ip));
		}
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
network_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	text	   *ret;
	char	   *dst;
	int			len;
	char		tmp[sizeof("255.255.255.255/32")];

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		if (ip_type(ip))
			dst = inet_cidr_ntop(AF_INET, &ip_v4addr(ip), ip_bits(ip),
								 tmp, sizeof(tmp));
		else
			dst = inet_net_ntop(AF_INET, &ip_v4addr(ip), ip_bits(ip),
								tmp, sizeof(tmp));

		if (dst == NULL)
			elog(ERROR, "unable to print address (%s)", strerror(errno));
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

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		unsigned long mask = 0xffffffff;

		/*
		 * Shifting by 32 or more bits does not yield portable results, so
		 * don't try it.
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

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		unsigned long mask = 0xffffffff;

		/*
		 * Shifting by 32 or more bits does not yield portable results, so
		 * don't try it.
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

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		unsigned long mask = 0xffffffff;

		/*
		 * Shifting by 32 or more bits does not yield portable results, so
		 * don't try it.
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

Datum
network_hostmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	inet	   *dst;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == AF_INET)
	{
		/* It's an IP V4 address: */
		unsigned long mask = 0xffffffff;

		/*
		 * Only shift if the mask len is < 32 bits ..
		 */

		if (ip_bits(ip) < 32)
			mask >>= ip_bits(ip);
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
 * Convert a value of a network datatype to an approximate scalar value.
 * This is used for estimating selectivities of inequality operators
 * involving network types.
 *
 * Currently, inet/cidr values are simply converted to the IPv4 address;
 * this will need more thought when IPv6 is supported too.	MAC addresses
 * are converted to their numeric equivalent as well (OK since we have a
 * double to play in).
 */
double
convert_network_to_scalar(Datum value, Oid typid)
{
	switch (typid)
	{
		case INETOID:
		case CIDROID:
			{
				inet	   *ip = DatumGetInetP(value);

				if (ip_family(ip) == AF_INET)
					return (double) ip_v4addr(ip);
				else
					/* Go for an IPV6 address here, before faulting out: */
					elog(ERROR, "unknown address family (%d)", ip_family(ip));
				break;
			}
		case MACADDROID:
			{
				macaddr    *mac = DatumGetMacaddrP(value);
				double		res;

				res = (mac->a << 16) | (mac->b << 8) | (mac->c);
				res *= 256 * 256 * 256;
				res += (mac->d << 16) | (mac->e << 8) | (mac->f);
				return res;
			}
	}

	/*
	 * Can't get here unless someone tries to use scalarltsel/scalargtsel
	 * on an operator with one network and one non-network operand.
	 */
	elog(ERROR, "convert_network_to_scalar: unsupported type %u", typid);
	return 0;
}


/*
 *	Bitwise comparison for V4 addresses.  Add V6 implementation!
 */

static int
v4bitncmp(unsigned long a1, unsigned long a2, int bits)
{
	unsigned long mask;

	/*
	 * Shifting by 32 or more bits does not yield portable results, so
	 * don't try it.
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

	/*
	 * Shifting by 32 or more bits does not yield portable results, so
	 * don't try it.
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


/*
 * These functions are used by planner to generate indexscan limits
 * for clauses a << b and a <<= b
 */

/* return the minimal value for an IP on a given network */
Datum
network_scan_first(Datum in)
{
	return DirectFunctionCall1(network_network, in);
}

/*
 * return "last" IP on a given network. It's the broadcast address,
 * however, masklen has to be set to 32, since
 * 192.168.0.255/24 is considered less than 192.168.0.255/32
 *
 * NB: this is not IPv6 ready ...
 */
Datum
network_scan_last(Datum in)
{
	return DirectFunctionCall2(inet_set_masklen,
							   DirectFunctionCall1(network_broadcast, in),
							   Int32GetDatum(32));
}
