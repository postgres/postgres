/*
 *	PostgreSQL type definitions for MAC addresses.
 *
 *	$Id: mac.c,v 1.5 1998/10/08 02:08:45 momjian Exp $
 */

#include <stdio.h>
#include <string.h>

#include <postgres.h>
#include <utils/palloc.h>
#include <utils/builtins.h>
#include <utils/inet.h>

manufacturer manufacturers[] = {
  {0x00, 0x00, 0x0C, "Cisco"},
  {0x00, 0x00, 0x0E, "Fujitsu"},
  {0x00, 0x00, 0x0F, "NeXT"},
  {0x00, 0x00, 0x10, "Sytek"},
  {0x00, 0x00, 0x1D, "Cabletron"},
  {0x00, 0x00, 0x20, "DIAB"},
  {0x00, 0x00, 0x22, "Visual Technology"},
  {0x00, 0x00, 0x2A, "TRW"},
  {0x00, 0x00, 0x32, "GPT Limited"},
  {0x00, 0x00, 0x5A, "S & Koch"},
  {0x00, 0x00, 0x5E, "IANA"},
  {0x00, 0x00, 0x65, "Network General"},
  {0x00, 0x00, 0x6B, "MIPS"},
  {0x00, 0x00, 0x77, "MIPS"},
  {0x00, 0x00, 0x7A, "Ardent"},
  {0x00, 0x00, 0x89, "Cayman Systems"},
  {0x00, 0x00, 0x93, "Proteon"},
  {0x00, 0x00, 0x9F, "Ameristar Technology"},
  {0x00, 0x00, 0xA2, "Wellfleet"},
  {0x00, 0x00, 0xA3, "Network Application Technology"},
  {0x00, 0x00, 0xA6, "Network General"},
  {0x00, 0x00, 0xA7, "NCD"},
  {0x00, 0x00, 0xA9, "Network Systems"},
  {0x00, 0x00, 0xAA, "Xerox"},
  {0x00, 0x00, 0xB3, "CIMLinc"},
  {0x00, 0x00, 0xB7, "Dove Fastnet"},
  {0x00, 0x00, 0xBC, "Allen-Bradley"},
  {0x00, 0x00, 0xC0, "Western Digital"},
  {0x00, 0x00, 0xC5, "Farallon"},
  {0x00, 0x00, 0xC6, "Hewlett-Packard"},
  {0x00, 0x00, 0xC8, "Altos"},
  {0x00, 0x00, 0xC9, "Emulex"},
  {0x00, 0x00, 0xD7, "Dartmouth College"},
  {0x00, 0x00, 0xD8, "3Com (?)"},
  {0x00, 0x00, 0xDD, "Gould"},
  {0x00, 0x00, 0xDE, "Unigraph"},
  {0x00, 0x00, 0xE2, "Acer Counterpoint"},
  {0x00, 0x00, 0xEF, "Alantec"},
  {0x00, 0x00, 0xFD, "High Level Hardware"},
  {0x00, 0x01, 0x02, "BBN internal usage"},
  {0x00, 0x20, 0xAF, "3Com"},
  {0x00, 0x17, 0x00, "Kabel"},
  {0x00, 0x80, 0x64, "Wyse Technology"},
  {0x00, 0x80, 0x2B, "IMAC (?)"},
  {0x00, 0x80, 0x2D, "Xylogics, Inc."},
  {0x00, 0x80, 0x8C, "Frontier Software Development"},
  {0x00, 0x80, 0xC2, "IEEE 802.1 Committee"},
  {0x00, 0x80, 0xD3, "Shiva"},
  {0x00, 0xAA, 0x00, "Intel"},
  {0x00, 0xDD, 0x00, "Ungermann-Bass"},
  {0x00, 0xDD, 0x01, "Ungermann-Bass"},
  {0x02, 0x07, 0x01, "Racal InterLan"},
  {0x02, 0x04, 0x06, "BBN internal usage"},
  {0x02, 0x60, 0x86, "Satelcom MegaPac"},
  {0x02, 0x60, 0x8C, "3Com"},
  {0x02, 0xCF, 0x1F, "CMC"},
  {0x08, 0x00, 0x02, "3Com"},
  {0x08, 0x00, 0x03, "ACC"},
  {0x08, 0x00, 0x05, "Symbolics"},
  {0x08, 0x00, 0x08, "BBN"},
  {0x08, 0x00, 0x09, "Hewlett-Packard"},
  {0x08, 0x00, 0x0A, "Nestar Systems"},
  {0x08, 0x00, 0x0B, "Unisys"},
  {0x08, 0x00, 0x11, "Tektronix"},
  {0x08, 0x00, 0x14, "Excelan"},
  {0x08, 0x00, 0x17, "NSC"},
  {0x08, 0x00, 0x1A, "Data General"},
  {0x08, 0x00, 0x1B, "Data General"},
  {0x08, 0x00, 0x1E, "Apollo"},
  {0x08, 0x00, 0x20, "Sun"},
  {0x08, 0x00, 0x22, "NBI"},
  {0x08, 0x00, 0x25, "CDC"},
  {0x08, 0x00, 0x26, "Norsk Data"},
  {0x08, 0x00, 0x27, "PCS Computer Systems GmbH"},
  {0x08, 0x00, 0x28, "Texas Instruments"},
  {0x08, 0x00, 0x2B, "DEC"},
  {0x08, 0x00, 0x2E, "Metaphor"},
  {0x08, 0x00, 0x2F, "Prime Computer"},
  {0x08, 0x00, 0x36, "Intergraph"},
  {0x08, 0x00, 0x37, "Fujitsu-Xerox"},
  {0x08, 0x00, 0x38, "Bull"},
  {0x08, 0x00, 0x39, "Spider Systems"},
  {0x08, 0x00, 0x41, "DCA Digital Comm. Assoc."},
  {0x08, 0x00, 0x45, "Xylogics (?)"},
  {0x08, 0x00, 0x46, "Sony"},
  {0x08, 0x00, 0x47, "Sequent"},
  {0x08, 0x00, 0x49, "Univation"},
  {0x08, 0x00, 0x4C, "Encore"},
  {0x08, 0x00, 0x4E, "BICC"},
  {0x08, 0x00, 0x56, "Stanford University"},
  {0x08, 0x00, 0x58, "DECsystem 20 (?)"},
  {0x08, 0x00, 0x5A, "IBM"},
  {0x08, 0x00, 0x67, "Comdesign"},
  {0x08, 0x00, 0x68, "Ridge"},
  {0x08, 0x00, 0x69, "Silicon Graphics"},
  {0x08, 0x00, 0x6E, "Concurrent"},
  {0x08, 0x00, 0x75, "DDE"},
  {0x08, 0x00, 0x7C, "Vitalink"},
  {0x08, 0x00, 0x80, "XIOS"},
  {0x08, 0x00, 0x86, "Imagen/QMS"},
  {0x08, 0x00, 0x87, "Xyplex"},
  {0x08, 0x00, 0x89, "Kinetics"},
  {0x08, 0x00, 0x8B, "Pyramid"},
  {0x08, 0x00, 0x8D, "XyVision"},
  {0x08, 0x00, 0x90, "Retix Inc"},
  {0x48, 0x44, 0x53, "HDS (?)"},
  {0x80, 0x00, 0x10, "AT&T"},
  {0xAA, 0x00, 0x00, "DEC"},
  {0xAA, 0x00, 0x01, "DEC"},
  {0xAA, 0x00, 0x02, "DEC"},
  {0xAA, 0x00, 0x03, "DEC"},
  {0xAA, 0x00, 0x04, "DEC"},
  {0x00, 0x00, 0x00, NULL}
};

/*
 *	Utility macros used for sorting and comparing:
 */

#define hibits(addr) \
  ((unsigned long)((addr->a<<16)|(addr->b<<8)|(addr->c)))

#define lobits(addr) \
  ((unsigned long)((addr->c<<16)|(addr->e<<8)|(addr->f)))

/*
 *	MAC address reader.  Accepts several common notations.
 */

macaddr    *
macaddr_in(char *str)
{
	int			a,
				b,
				c,
				d,
				e,
				f;
	macaddr    *result;
	int			count;

	if (strlen(str) > 0)
	{

		count = sscanf(str, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%x-%x-%x-%x-%x-%x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%2x%2x%2x:%2x%2x%2x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%2x%2x%2x-%2x%2x%2x", &a, &b, &c, &d, &e, &f);
		if (count != 6)
			count = sscanf(str, "%2x%2x.%2x%2x.%2x%2x", &a, &b, &c, &d, &e, &f);

		if (count != 6)
		{
			elog(ERROR, "macaddr_in: error in parsing \"%s\"", str);
			return (NULL);
		}

		if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
			(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
			(e < 0) || (e > 255) || (f < 0) || (f > 255))
		{
			elog(ERROR, "macaddr_in: illegal address \"%s\"", str);
			return (NULL);
		}
	}
	else
	{
		a = b = c = d = e = f = 0;		/* special case for missing
										 * address */
	}

	result = (macaddr *) palloc(sizeof(macaddr));

	result->a = a;
	result->b = b;
	result->c = c;
	result->d = d;
	result->e = e;
	result->f = f;

	return (result);
}

/*
 *	MAC address output function.  Fixed format.
 */

char *
macaddr_out(macaddr *addr)
{
	char	   *result;

	if (addr == NULL)
		return (NULL);

	result = (char *) palloc(32);

	if ((hibits(addr) > 0) || (lobits(addr) > 0))
	{
		sprintf(result, "%02x:%02x:%02x:%02x:%02x:%02x",
				addr->a, addr->b, addr->c, addr->d, addr->e, addr->f);
	}
	else
	{
		result[0] = 0;			/* special case for missing address */
	}
	return (result);
}

/*
 *	Boolean tests.
 */

bool
macaddr_lt(macaddr *a1, macaddr *a2)
{
	return ((hibits(a1) < hibits(a2)) ||
			((hibits(a1) == hibits(a2)) && lobits(a1) < lobits(a2)));
};

bool
macaddr_le(macaddr *a1, macaddr *a2)
{
	return ((hibits(a1) < hibits(a2)) ||
			((hibits(a1) == hibits(a2)) && lobits(a1) <= lobits(a2)));
};

bool
macaddr_eq(macaddr *a1, macaddr *a2)
{
	return ((hibits(a1) == hibits(a2)) && (lobits(a1) == lobits(a2)));
};

bool
macaddr_ge(macaddr *a1, macaddr *a2)
{
	return ((hibits(a1) > hibits(a2)) ||
			((hibits(a1) == hibits(a2)) && lobits(a1) >= lobits(a2)));
};

bool
macaddr_gt(macaddr *a1, macaddr *a2)
{
	return ((hibits(a1) > hibits(a2)) ||
			((hibits(a1) == hibits(a2)) && lobits(a1) > lobits(a2)));
};

bool
macaddr_ne(macaddr *a1, macaddr *a2)
{
	return ((hibits(a1) != hibits(a2)) || (lobits(a1) != lobits(a2)));
};

/*
 *	Comparison function for sorting:
 */

int4
macaddr_cmp(macaddr *a1, macaddr *a2)
{
	if (hibits(a1) < hibits(a2))
		return -1;
	else if (hibits(a1) > hibits(a2))
		return 1;
	else if (lobits(a1) < lobits(a2))
		return -1;
	else if (lobits(a1) > lobits(a2))
		return 1;
	else
		return 0;
}

/*
 *	The special manufacturer fetching function.  See "mac.h".
 */

text *
macaddr_manuf(macaddr *addr)
{
	manufacturer *manuf;
	int			length;
	text	   *result;

	for (manuf = manufacturers; manuf->name != NULL; manuf++)
	{
		if ((manuf->a == addr->a) &&
			(manuf->b == addr->b) &&
			(manuf->c == addr->c))
			break;
	}
	if (manuf->name == NULL)
	{
		result = palloc(VARHDRSZ + 1);
		memset(result, 0, VARHDRSZ + 1);
		VARSIZE(result) = VARHDRSZ + 1;
	}
	else
	{
		length = strlen(manuf->name) + 1;
		result = palloc(length + VARHDRSZ);
		memset(result, 0, length + VARHDRSZ);
		VARSIZE(result) = length + VARHDRSZ;
		memcpy(VARDATA(result), manuf->name, length);
	}
	return result;
}
