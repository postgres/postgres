/* $Id: inet_aton.c,v 1.5 2003/06/12 08:15:29 momjian Exp $
 *
 *	This inet_aton() function was taken from the GNU C library and
 *	incorporated into Postgres for those systems which do not have this
 *	routine in their standard C libraries.
 *
 *	The function was been extracted whole from the file inet_aton.c in
 *	Release 5.3.12 of the Linux C library, which is derived from the
 *	GNU C library, by Bryan Henderson in October 1996.	The copyright
 *	notice from that file is below.
 */

/*
 * Copyright (c) 1983, 1990, 1993
 *		The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *		This product includes software developed by the University of
 *		California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.  */

#include "c.h"

#if !defined(_MSC_VER) && !defined(__BORLANDC__)
#include <netinet/in.h>
#include <ctype.h>
#endif

/*
 * Check whether "cp" is a valid ascii representation
 * of an Internet address and convert to a binary address.
 * Returns 1 if the address is valid, 0 if not.
 * This replaces inet_addr, the return value from which
 * cannot distinguish between failure and a local broadcast address.
 */
int
inet_aton(const char *cp, struct in_addr * addr)
{
	unsigned int val;
	int			base,
				n;
	char		c;
	u_int		parts[4];
	u_int	   *pp = parts;

	for (;;)
	{
		/*
		 * Collect number up to ``.''. Values are specified as for C:
		 * 0x=hex, 0=octal, other=decimal.
		 */
		val = 0;
		base = 10;
		if (*cp == '0')
		{
			if (*++cp == 'x' || *cp == 'X')
				base = 16, cp++;
			else
				base = 8;
		}
		while ((c = *cp) != '\0')
		{
			if (isdigit((unsigned char) c))
			{
				val = (val * base) + (c - '0');
				cp++;
				continue;
			}
			if (base == 16 && isxdigit((unsigned char) c))
			{
				val = (val << 4) +
					(c + 10 - (islower((unsigned char) c) ? 'a' : 'A'));
				cp++;
				continue;
			}
			break;
		}
		if (*cp == '.')
		{
			/*
			 * Internet format: a.b.c.d a.b.c	(with c treated as
			 * 16-bits) a.b		(with b treated as 24 bits)
			 */
			if (pp >= parts + 3 || val > 0xff)
				return 0;
			*pp++ = val, cp++;
		}
		else
			break;
	}

	/*
	 * Check for trailing junk.
	 */
	while (*cp)
		if (!isspace((unsigned char) *cp++))
			return 0;

	/*
	 * Concoct the address according to the number of parts specified.
	 */
	n = pp - parts + 1;
	switch (n)
	{

		case 1:			/* a -- 32 bits */
			break;

		case 2:			/* a.b -- 8.24 bits */
			if (val > 0xffffff)
				return 0;
			val |= parts[0] << 24;
			break;

		case 3:			/* a.b.c -- 8.8.16 bits */
			if (val > 0xffff)
				return 0;
			val |= (parts[0] << 24) | (parts[1] << 16);
			break;

		case 4:			/* a.b.c.d -- 8.8.8.8 bits */
			if (val > 0xff)
				return 0;
			val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8);
			break;
	}
	if (addr)
		addr->s_addr = htonl(val);
	return 1;
}
