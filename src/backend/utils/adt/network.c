/*
 *	PostgreSQL type definitions for the INET and CIDR types.
 *
 *	$Header: /cvsroot/pgsql/src/backend/utils/adt/network.c,v 1.47.2.1 2003/12/01 18:50:29 tgl Exp $
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
static int	bitncmp(void *l, void *r, int n);
static bool addressOK(unsigned char *a, int bits, int family);
static int	ip_addrsize(inet *inetptr);

/*
 *	Access macros.
 */

#define ip_family(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->family)

#define ip_bits(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->bits)

#define ip_type(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->type)

#define ip_addr(inetptr) \
	(((inet_struct *)VARDATA(inetptr))->ip_addr)

#define ip_maxbits(inetptr) \
	(ip_family(inetptr) == PGSQL_AF_INET ? 32 : 128)

/*
 * Return the number of bytes of storage needed for this data type.
 */
static int
ip_addrsize(inet *inetptr)
{
	switch (ip_family(inetptr))
	{
		case PGSQL_AF_INET:
			return 4;
		case PGSQL_AF_INET6:
			return 16;
		default:
			return -1;
	}
}

/* Common input routine */
static inet *
network_in(char *src, int type)
{
	int			bits;
	inet	   *dst;

	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	/*
	 * First, check to see if this is an IPv6 or IPv4 address.	IPv6
	 * addresses will have a : somewhere in them (several, in fact) so if
	 * there is one present, assume it's V6, otherwise assume it's V4.
	 */

	if (strchr(src, ':') != NULL)
		ip_family(dst) = PGSQL_AF_INET6;
	else
		ip_family(dst) = PGSQL_AF_INET;

	bits = inet_net_pton(ip_family(dst), src, ip_addr(dst),
						 type ? ip_addrsize(dst) : -1);
	if ((bits < 0) || (bits > ip_maxbits(dst)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		/* translator: first %s is inet or cidr */
				 errmsg("invalid input syntax for type %s: \"%s\"",
						type ? "cidr" : "inet", src)));

	/*
	 * Error check: CIDR values must not have any bits set beyond the
	 * masklen.
	 */
	if (type)
	{
		if (!addressOK(ip_addr(dst), bits, ip_family(dst)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid cidr value: \"%s\"", src),
					 errdetail("Value has bits set to right of mask.")));
	}

	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) ip_addr(dst) - (char *) VARDATA(dst))
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
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];
	char	   *dst;
	int			len;

	dst = inet_net_ntop(ip_family(src), ip_addr(src), ip_bits(src),
						tmp, sizeof(tmp));
	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* For CIDR, add /n if not present */
	if (ip_type(src) && strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(src));
	}

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
	if (ip_family(addr) != PGSQL_AF_INET &&
		ip_family(addr) != PGSQL_AF_INET6)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid address family in external \"inet\" value")));
	bits = pq_getmsgbyte(buf);
	if (bits < 0 || bits > ip_maxbits(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid bits in external \"inet\" value")));
	ip_bits(addr) = bits;
	ip_type(addr) = pq_getmsgbyte(buf);
	if (ip_type(addr) != 0 && ip_type(addr) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid type in external \"inet\" value")));
	nb = pq_getmsgbyte(buf);
	if (nb != ip_addrsize(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid length in external \"inet\" value")));
	VARATT_SIZEP(addr) = VARHDRSZ
		+ ((char *) ip_addr(addr) - (char *) VARDATA(addr))
		+ ip_addrsize(addr);

	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		addrptr[i] = pq_getmsgbyte(buf);

	/*
	 * Error check: CIDR values must not have any bits set beyond the
	 * masklen.
	 */
	if (ip_type(addr))
	{
		if (!addressOK(ip_addr(addr), bits, ip_family(addr)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid external \"cidr\" value"),
					 errdetail("Value has bits set to right of mask.")));
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
	addrptr = (char *) ip_addr(addr);
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

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	/* clone the original data */
	dst = (inet *) palloc(VARSIZE(src));
	memcpy(dst, src, VARSIZE(src));

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
	if (ip_family(a1) == ip_family(a2))
	{
		int			order;

		order = bitncmp(ip_addr(a1), ip_addr(a2),
						Min(ip_bits(a1), ip_bits(a2)));
		if (order != 0)
			return order;
		order = ((int) ip_bits(a1)) - ((int) ip_bits(a2));
		if (order != 0)
			return order;
		return bitncmp(ip_addr(a1), ip_addr(a2), ip_maxbits(a1));
	}

	return ip_family(a1) - ip_family(a2);
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

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) > ip_bits(a2)
				 && bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_subeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) >= ip_bits(a2)
				 && bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_sup(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) < ip_bits(a2)
				 && bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_supeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_P(0);
	inet	   *a2 = PG_GETARG_INET_P(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) <= ip_bits(a2)
				 && bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
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
	char	   *ptr;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	/* force display of max bits, regardless of masklen... */
	if (inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
					  tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

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
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	if (inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
					  tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* Add /n if not present (which it won't be) */
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
network_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	text	   *ret;
	char	   *dst;
	int			len;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	if (ip_type(ip))
		dst = inet_cidr_ntop(ip_family(ip), ip_addr(ip),
							 ip_bits(ip), tmp, sizeof(tmp));
	else
		dst = inet_net_ntop(ip_family(ip), ip_addr(ip),
							ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

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
network_family(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);

	switch (ip_family(ip))
	{
		case PGSQL_AF_INET:
			PG_RETURN_INT32(4);
			break;
		case PGSQL_AF_INET6:
			PG_RETURN_INT32(6);
			break;
		default:
			PG_RETURN_INT32(0);
			break;
	}
}

Datum
network_broadcast(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == PGSQL_AF_INET)
		maxbytes = 4;
	else
		maxbytes = 16;

	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	for (byte = 0; byte < maxbytes; byte++)
	{
		if (bits >= 8)
		{
			mask = 0x00;
			bits -= 8;
		}
		else if (bits == 0)
			mask = 0xff;
		else
		{
			mask = 0xff >> bits;
			bits = 0;
		}

		b[byte] = a[byte] | mask;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	ip_type(dst) = 0;
	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) ip_addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_network(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	byte = 0;
	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = a[byte] & mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	ip_type(dst) = 1;
	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) ip_addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_netmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	bits = ip_bits(ip);
	b = ip_addr(dst);

	byte = 0;
	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	ip_type(dst) = 0;
	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) ip_addr(dst) - (char *) VARDATA(dst))
		+ ip_addrsize(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_hostmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_P(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(VARHDRSZ + sizeof(inet_struct));

	if (ip_family(ip) == PGSQL_AF_INET)
		maxbytes = 4;
	else
		maxbytes = 16;

	bits = ip_maxbits(ip) - ip_bits(ip);
	b = ip_addr(dst);

	byte = maxbytes - 1;
	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff >> (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte--;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	ip_type(dst) = 0;
	VARATT_SIZEP(dst) = VARHDRSZ
		+ ((char *) ip_addr(dst) - (char *) VARDATA(dst))
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
				int			len;
				double		res;
				int			i;

				/*
				 * Note that we don't use the full address here.
				 */
				if (ip_family(ip) == PGSQL_AF_INET)
					len = 4;
				else
					len = 5;

				res = ip_family(ip);
				for (i = 0; i < len; i++)
				{
					res *= 256;
					res += ip_addr(ip)[i];
				}
				return res;

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
	elog(ERROR, "unsupported type: %u", typid);
	return 0;
}

/*
 * int
 * bitncmp(l, r, n)
 *		compare bit masks l and r, for n bits.
 * return:
 *		-1, 1, or 0 in the libc tradition.
 * note:
 *		network byte order assumed.  this means 192.5.5.240/28 has
 *		0x11110000 in its fourth octet.
 * author:
 *		Paul Vixie (ISC), June 1996
 */
static int
bitncmp(void *l, void *r, int n)
{
	u_int		lb,
				rb;
	int			x,
				b;

	b = n / 8;
	x = memcmp(l, r, b);
	if (x)
		return (x);

	lb = ((const u_char *) l)[b];
	rb = ((const u_char *) r)[b];
	for (b = n % 8; b > 0; b--)
	{
		if ((lb & 0x80) != (rb & 0x80))
		{
			if (lb & 0x80)
				return (1);
			return (-1);
		}
		lb <<= 1;
		rb <<= 1;
	}
	return (0);
}

static bool
addressOK(unsigned char *a, int bits, int family)
{
	int			byte;
	int			nbits;
	int			maxbits;
	int			maxbytes;
	unsigned char mask;

	if (family == PGSQL_AF_INET)
	{
		maxbits = 32;
		maxbytes = 4;
	}
	else
	{
		maxbits = 128;
		maxbytes = 16;
	}
	Assert(bits <= maxbits);

	if (bits == maxbits)
		return true;

	byte = (bits + 7) / 8;
	nbits = bits % 8;
	mask = 0xff;
	if (bits != 0)
		mask >>= nbits;

	while (byte < maxbytes)
	{
		if ((a[byte] & mask) != 0)
			return false;
		mask = 0xff;
		byte++;
	}

	return true;
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
 * however, masklen has to be set to its max btis, since
 * 192.168.0.255/24 is considered less than 192.168.0.255/32
 *
 * inet_set_masklen() hacked to max out the masklength to 128 for IPv6
 * and 32 for IPv4 when given '-1' as argument.
 */
Datum
network_scan_last(Datum in)
{
	return DirectFunctionCall2(inet_set_masklen,
							   DirectFunctionCall1(network_broadcast, in),
							   Int32GetDatum(-1));
}
