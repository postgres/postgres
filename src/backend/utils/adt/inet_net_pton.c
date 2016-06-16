/*
 * Copyright (c) 2004 by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1996,1999 by Internet Software Consortium.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *	  src/backend/utils/adt/inet_net_pton.c
 */

#if defined(LIBC_SCCS) && !defined(lint)
static const char rcsid[] = "Id: inet_net_pton.c,v 1.4.2.3 2004/03/17 00:40:11 marka Exp $";
#endif

#include "postgres.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>

#include "utils/builtins.h" /* pgrminclude ignore */	/* needed on some
														 * platforms */
#include "utils/inet.h"


static int	inet_net_pton_ipv4(const char *src, u_char *dst);
static int	inet_cidr_pton_ipv4(const char *src, u_char *dst, size_t size);
static int	inet_net_pton_ipv6(const char *src, u_char *dst);
static int	inet_cidr_pton_ipv6(const char *src, u_char *dst, size_t size);


/*
 * int
 * inet_net_pton(af, src, dst, size)
 *	convert network number from presentation to network format.
 *	accepts hex octets, hex strings, decimal octets, and /CIDR.
 *	"size" is in bytes and describes "dst".
 * return:
 *	number of bits, either imputed classfully or specified with /CIDR,
 *	or -1 if some failure occurred (check errno).  ENOENT means it was
 *	not a valid network specification.
 * author:
 *	Paul Vixie (ISC), June 1996
 *
 * Changes:
 *	I added the inet_cidr_pton function (also from Paul) and changed
 *	the names to reflect their current use.
 *
 */
int
inet_net_pton(int af, const char *src, void *dst, size_t size)
{
	switch (af)
	{
		case PGSQL_AF_INET:
			return size == -1 ?
				inet_net_pton_ipv4(src, dst) :
				inet_cidr_pton_ipv4(src, dst, size);
		case PGSQL_AF_INET6:
			return size == -1 ?
				inet_net_pton_ipv6(src, dst) :
				inet_cidr_pton_ipv6(src, dst, size);
		default:
			errno = EAFNOSUPPORT;
			return (-1);
	}
}

/*
 * static int
 * inet_cidr_pton_ipv4(src, dst, size)
 *	convert IPv4 network number from presentation to network format.
 *	accepts hex octets, hex strings, decimal octets, and /CIDR.
 *	"size" is in bytes and describes "dst".
 * return:
 *	number of bits, either imputed classfully or specified with /CIDR,
 *	or -1 if some failure occurred (check errno).  ENOENT means it was
 *	not an IPv4 network specification.
 * note:
 *	network byte order assumed.  this means 192.5.5.240/28 has
 *	0b11110000 in its fourth octet.
 * author:
 *	Paul Vixie (ISC), June 1996
 */
static int
inet_cidr_pton_ipv4(const char *src, u_char *dst, size_t size)
{
	static const char xdigits[] = "0123456789abcdef";
	static const char digits[] = "0123456789";
	int			n,
				ch,
				tmp = 0,
				dirty,
				bits;
	const u_char *odst = dst;

	ch = *src++;
	if (ch == '0' && (src[0] == 'x' || src[0] == 'X')
		&& isxdigit((unsigned char) src[1]))
	{
		/* Hexadecimal: Eat nybble string. */
		if (size <= 0U)
			goto emsgsize;
		dirty = 0;
		src++;					/* skip x or X. */
		while ((ch = *src++) != '\0' && isxdigit((unsigned char) ch))
		{
			if (isupper((unsigned char) ch))
				ch = tolower((unsigned char) ch);
			n = strchr(xdigits, ch) - xdigits;
			assert(n >= 0 && n <= 15);
			if (dirty == 0)
				tmp = n;
			else
				tmp = (tmp << 4) | n;
			if (++dirty == 2)
			{
				if (size-- <= 0U)
					goto emsgsize;
				*dst++ = (u_char) tmp;
				dirty = 0;
			}
		}
		if (dirty)
		{						/* Odd trailing nybble? */
			if (size-- <= 0U)
				goto emsgsize;
			*dst++ = (u_char) (tmp << 4);
		}
	}
	else if (isdigit((unsigned char) ch))
	{
		/* Decimal: eat dotted digit string. */
		for (;;)
		{
			tmp = 0;
			do
			{
				n = strchr(digits, ch) - digits;
				assert(n >= 0 && n <= 9);
				tmp *= 10;
				tmp += n;
				if (tmp > 255)
					goto enoent;
			} while ((ch = *src++) != '\0' &&
					 isdigit((unsigned char) ch));
			if (size-- <= 0U)
				goto emsgsize;
			*dst++ = (u_char) tmp;
			if (ch == '\0' || ch == '/')
				break;
			if (ch != '.')
				goto enoent;
			ch = *src++;
			if (!isdigit((unsigned char) ch))
				goto enoent;
		}
	}
	else
		goto enoent;

	bits = -1;
	if (ch == '/' && isdigit((unsigned char) src[0]) && dst > odst)
	{
		/* CIDR width specifier.  Nothing can follow it. */
		ch = *src++;			/* Skip over the /. */
		bits = 0;
		do
		{
			n = strchr(digits, ch) - digits;
			assert(n >= 0 && n <= 9);
			bits *= 10;
			bits += n;
		} while ((ch = *src++) != '\0' && isdigit((unsigned char) ch));
		if (ch != '\0')
			goto enoent;
		if (bits > 32)
			goto emsgsize;
	}

	/* Firey death and destruction unless we prefetched EOS. */
	if (ch != '\0')
		goto enoent;

	/* If nothing was written to the destination, we found no address. */
	if (dst == odst)
		goto enoent;
	/* If no CIDR spec was given, infer width from net class. */
	if (bits == -1)
	{
		if (*odst >= 240)		/* Class E */
			bits = 32;
		else if (*odst >= 224)	/* Class D */
			bits = 8;
		else if (*odst >= 192)	/* Class C */
			bits = 24;
		else if (*odst >= 128)	/* Class B */
			bits = 16;
		else
			/* Class A */
			bits = 8;
		/* If imputed mask is narrower than specified octets, widen. */
		if (bits < ((dst - odst) * 8))
			bits = (dst - odst) * 8;

		/*
		 * If there are no additional bits specified for a class D address
		 * adjust bits to 4.
		 */
		if (bits == 8 && *odst == 224)
			bits = 4;
	}
	/* Extend network to cover the actual mask. */
	while (bits > ((dst - odst) * 8))
	{
		if (size-- <= 0U)
			goto emsgsize;
		*dst++ = '\0';
	}
	return (bits);

enoent:
	errno = ENOENT;
	return (-1);

emsgsize:
	errno = EMSGSIZE;
	return (-1);
}

/*
 * int
 * inet_net_pton(af, src, dst, *bits)
 *	convert network address from presentation to network format.
 *	accepts inet_pton()'s input for this "af" plus trailing "/CIDR".
 *	"dst" is assumed large enough for its "af".  "bits" is set to the
 *	/CIDR prefix length, which can have defaults (like /32 for IPv4).
 * return:
 *	-1 if an error occurred (inspect errno; ENOENT means bad format).
 *	0 if successful conversion occurred.
 * note:
 *	192.5.5.1/28 has a nonzero host part, which means it isn't a network
 *	as called for by inet_cidr_pton() but it can be a host address with
 *	an included netmask.
 * author:
 *	Paul Vixie (ISC), October 1998
 */
static int
inet_net_pton_ipv4(const char *src, u_char *dst)
{
	static const char digits[] = "0123456789";
	const u_char *odst = dst;
	int			n,
				ch,
				tmp,
				bits;
	size_t		size = 4;

	/* Get the mantissa. */
	while (ch = *src++, isdigit((unsigned char) ch))
	{
		tmp = 0;
		do
		{
			n = strchr(digits, ch) - digits;
			assert(n >= 0 && n <= 9);
			tmp *= 10;
			tmp += n;
			if (tmp > 255)
				goto enoent;
		} while ((ch = *src++) != '\0' && isdigit((unsigned char) ch));
		if (size-- == 0)
			goto emsgsize;
		*dst++ = (u_char) tmp;
		if (ch == '\0' || ch == '/')
			break;
		if (ch != '.')
			goto enoent;
	}

	/* Get the prefix length if any. */
	bits = -1;
	if (ch == '/' && isdigit((unsigned char) src[0]) && dst > odst)
	{
		/* CIDR width specifier.  Nothing can follow it. */
		ch = *src++;			/* Skip over the /. */
		bits = 0;
		do
		{
			n = strchr(digits, ch) - digits;
			assert(n >= 0 && n <= 9);
			bits *= 10;
			bits += n;
		} while ((ch = *src++) != '\0' && isdigit((unsigned char) ch));
		if (ch != '\0')
			goto enoent;
		if (bits > 32)
			goto emsgsize;
	}

	/* Firey death and destruction unless we prefetched EOS. */
	if (ch != '\0')
		goto enoent;

	/* Prefix length can default to /32 only if all four octets spec'd. */
	if (bits == -1)
	{
		if (dst - odst == 4)
			bits = 32;
		else
			goto enoent;
	}

	/* If nothing was written to the destination, we found no address. */
	if (dst == odst)
		goto enoent;

	/* If prefix length overspecifies mantissa, life is bad. */
	if ((bits / 8) > (dst - odst))
		goto enoent;

	/* Extend address to four octets. */
	while (size-- > 0)
		*dst++ = 0;

	return bits;

enoent:
	errno = ENOENT;
	return (-1);

emsgsize:
	errno = EMSGSIZE;
	return (-1);
}

static int
getbits(const char *src, int *bitsp)
{
	static const char digits[] = "0123456789";
	int			n;
	int			val;
	char		ch;

	val = 0;
	n = 0;
	while ((ch = *src++) != '\0')
	{
		const char *pch;

		pch = strchr(digits, ch);
		if (pch != NULL)
		{
			if (n++ != 0 && val == 0)	/* no leading zeros */
				return (0);
			val *= 10;
			val += (pch - digits);
			if (val > 128)		/* range */
				return (0);
			continue;
		}
		return (0);
	}
	if (n == 0)
		return (0);
	*bitsp = val;
	return (1);
}

static int
getv4(const char *src, u_char *dst, int *bitsp)
{
	static const char digits[] = "0123456789";
	u_char	   *odst = dst;
	int			n;
	u_int		val;
	char		ch;

	val = 0;
	n = 0;
	while ((ch = *src++) != '\0')
	{
		const char *pch;

		pch = strchr(digits, ch);
		if (pch != NULL)
		{
			if (n++ != 0 && val == 0)	/* no leading zeros */
				return (0);
			val *= 10;
			val += (pch - digits);
			if (val > 255)		/* range */
				return (0);
			continue;
		}
		if (ch == '.' || ch == '/')
		{
			if (dst - odst > 3) /* too many octets? */
				return (0);
			*dst++ = val;
			if (ch == '/')
				return (getbits(src, bitsp));
			val = 0;
			n = 0;
			continue;
		}
		return (0);
	}
	if (n == 0)
		return (0);
	if (dst - odst > 3)			/* too many octets? */
		return (0);
	*dst++ = val;
	return (1);
}

static int
inet_net_pton_ipv6(const char *src, u_char *dst)
{
	return inet_cidr_pton_ipv6(src, dst, 16);
}

#define NS_IN6ADDRSZ 16
#define NS_INT16SZ 2
#define NS_INADDRSZ 4

static int
inet_cidr_pton_ipv6(const char *src, u_char *dst, size_t size)
{
	static const char xdigits_l[] = "0123456789abcdef",
				xdigits_u[] = "0123456789ABCDEF";
	u_char		tmp[NS_IN6ADDRSZ],
			   *tp,
			   *endp,
			   *colonp;
	const char *xdigits,
			   *curtok;
	int			ch,
				saw_xdigit;
	u_int		val;
	int			digits;
	int			bits;

	if (size < NS_IN6ADDRSZ)
		goto emsgsize;

	memset((tp = tmp), '\0', NS_IN6ADDRSZ);
	endp = tp + NS_IN6ADDRSZ;
	colonp = NULL;
	/* Leading :: requires some special handling. */
	if (*src == ':')
		if (*++src != ':')
			goto enoent;
	curtok = src;
	saw_xdigit = 0;
	val = 0;
	digits = 0;
	bits = -1;
	while ((ch = *src++) != '\0')
	{
		const char *pch;

		if ((pch = strchr((xdigits = xdigits_l), ch)) == NULL)
			pch = strchr((xdigits = xdigits_u), ch);
		if (pch != NULL)
		{
			val <<= 4;
			val |= (pch - xdigits);
			if (++digits > 4)
				goto enoent;
			saw_xdigit = 1;
			continue;
		}
		if (ch == ':')
		{
			curtok = src;
			if (!saw_xdigit)
			{
				if (colonp)
					goto enoent;
				colonp = tp;
				continue;
			}
			else if (*src == '\0')
				goto enoent;
			if (tp + NS_INT16SZ > endp)
				goto enoent;
			*tp++ = (u_char) (val >> 8) & 0xff;
			*tp++ = (u_char) val & 0xff;
			saw_xdigit = 0;
			digits = 0;
			val = 0;
			continue;
		}
		if (ch == '.' && ((tp + NS_INADDRSZ) <= endp) &&
			getv4(curtok, tp, &bits) > 0)
		{
			tp += NS_INADDRSZ;
			saw_xdigit = 0;
			break;				/* '\0' was seen by inet_pton4(). */
		}
		if (ch == '/' && getbits(src, &bits) > 0)
			break;
		goto enoent;
	}
	if (saw_xdigit)
	{
		if (tp + NS_INT16SZ > endp)
			goto enoent;
		*tp++ = (u_char) (val >> 8) & 0xff;
		*tp++ = (u_char) val & 0xff;
	}
	if (bits == -1)
		bits = 128;

	endp = tmp + 16;

	if (colonp != NULL)
	{
		/*
		 * Since some memmove()'s erroneously fail to handle overlapping
		 * regions, we'll do the shift by hand.
		 */
		const int	n = tp - colonp;
		int			i;

		if (tp == endp)
			goto enoent;
		for (i = 1; i <= n; i++)
		{
			endp[-i] = colonp[n - i];
			colonp[n - i] = 0;
		}
		tp = endp;
	}
	if (tp != endp)
		goto enoent;

	/*
	 * Copy out the result.
	 */
	memcpy(dst, tmp, NS_IN6ADDRSZ);

	return (bits);

enoent:
	errno = ENOENT;
	return (-1);

emsgsize:
	errno = EMSGSIZE;
	return (-1);
}
