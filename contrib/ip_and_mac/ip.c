/*
 *	PostgreSQL type definitions for IP addresses.
 *
 *	$Id: ip.c,v 1.3 1998/02/26 04:27:37 momjian Exp $
 */

#include <stdio.h>

#include <postgres.h>
#include <utils/palloc.h>

/*
 *	This is the internal storage format for IP addresses:
 */

typedef struct ipaddr
{
	uint32		address;
	int16		width;
}			ipaddr;

/*
 *	Various forward declarations:
 */

ipaddr	   *ipaddr_in(char *str);
char	   *ipaddr_out(ipaddr * addr);

bool		ipaddr_lt(ipaddr * a1, ipaddr * a2);
bool		ipaddr_le(ipaddr * a1, ipaddr * a2);
bool		ipaddr_eq(ipaddr * a1, ipaddr * a2);
bool		ipaddr_ge(ipaddr * a1, ipaddr * a2);
bool		ipaddr_gt(ipaddr * a1, ipaddr * a2);

bool		ipaddr_ne(ipaddr * a1, ipaddr * a2);

int4		ipaddr_cmp(ipaddr * a1, ipaddr * a2);

bool		ipaddr_in_net(ipaddr * a1, ipaddr * a2);
ipaddr	   *ipaddr_mask(ipaddr * a);
ipaddr	   *ipaddr_bcast(ipaddr * a);

/*
 *	Build a mask of a given width:
 */

unsigned long
build_mask(unsigned char bits)
{
	unsigned long mask = 0;
	int			i;

	for (i = 0; i < bits; i++)
		mask = (mask >> 1) | 0x80000000;
	return mask;
}

/*
 *	IP address reader.	Note how the count returned by sscanf()
 *	is used to determine whether the mask size was specified.
 */

ipaddr *
ipaddr_in(char *str)
{
	int			a,
				b,
				c,
				d,
				w;
	ipaddr	   *result;
	int			count;

	if (strlen(str) > 0)
	{

		count = sscanf(str, "%d.%d.%d.%d/%d", &a, &b, &c, &d, &w);

		if (count < 4)
		{
			elog(ERROR, "ipaddr_in: error in parsing \"%s\"", str);
			return (NULL);
		}

		if (count == 4)
			w = 32;

		if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
			(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
			(w < 0) || (w > 32))
		{
			elog(ERROR, "ipaddr_in: illegal address \"%s\"", str);
			return (NULL);
		}
	}
	else
	{
		a = b = c = d = w = 0;	/* special case for missing address */
	}

	result = (ipaddr *) palloc(sizeof(ipaddr));

	result->address = (uint32) ((a << 24) | (b << 16) | (c << 8) | d);
	result->address &= build_mask(w);
	result->width = w;

	return (result);
}

/*
 *	IP address output function.  Note mask size specification
 *	generated only for subnets, not for plain host addresses.
 */

char *
ipaddr_out(ipaddr * addr)
{
	char	   *result;

	if (addr == NULL)
		return (NULL);

	result = (char *) palloc(32);

	if (addr->address > 0)
	{
		if (addr->width == 32)
			sprintf(result, "%d.%d.%d.%d",
					(addr->address >> 24) & 0xff,
					(addr->address >> 16) & 0xff,
					(addr->address >> 8) & 0xff,
					addr->address & 0xff);
		else
			sprintf(result, "%d.%d.%d.%d/%d",
					(addr->address >> 24) & 0xff,
					(addr->address >> 16) & 0xff,
					(addr->address >> 8) & 0xff,
					addr->address & 0xff,
					addr->width);
	}
	else
	{
		result[0] = 0;			/* special case for missing address */
	}
	return (result);
}

/*
 *	Boolean tests for magnitude.
 */

bool
ipaddr_lt(ipaddr * a1, ipaddr * a2)
{
	return (a1->address < a2->address);
};

bool
ipaddr_le(ipaddr * a1, ipaddr * a2)
{
	return (a1->address <= a2->address);
};

bool
ipaddr_eq(ipaddr * a1, ipaddr * a2)
{
	return (a1->address == a2->address);
};

bool
ipaddr_ge(ipaddr * a1, ipaddr * a2)
{
	return (a1->address >= a2->address);
};

bool
ipaddr_gt(ipaddr * a1, ipaddr * a2)
{
	return (a1->address > a2->address);
};

bool
ipaddr_ne(ipaddr * a1, ipaddr * a2)
{
	return (a1->address != a2->address);
};

/*
 *	Comparison function for sorting:
 */

int4
ipaddr_cmp(ipaddr * a1, ipaddr * a2)
{
	if (a1->address < a2->address)
		return -1;
	else if (a1->address > a2->address)
		return 1;
	else
		return 0;
}

/*
 *	Test whether an address is within a given subnet:
 */

bool
ipaddr_in_net(ipaddr * a1, ipaddr * a2)
{
	uint32		maskbits;

	if (a1->width < a2->width)
		return FALSE;
	if ((a1->width == 32) && (a2->width == 32))
		return ipaddr_eq(a1, a2);
	maskbits = build_mask(a2->width);
	if ((a1->address & maskbits) == (a2->address & maskbits))
		return TRUE;
	return FALSE;
}

/*
 *	Pick out just the mask of a network:
 */

ipaddr *
ipaddr_mask(ipaddr * a)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	result->address = build_mask(a->width);
	result->width = 32;

	return result;
}

/*
 *	Return the broadcast address of a network:
 */

ipaddr *
ipaddr_bcast(ipaddr * a)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	result->address = a->address;
	result->address |= (build_mask(32 - a->width) >> a->width);
	result->width = 32;

	return result;
}

/*
 *	eof
 */
