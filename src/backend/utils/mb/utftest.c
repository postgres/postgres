/*
 * testing of utf2wchar()
 * $Id: utftest.c,v 1.2 1998/09/01 04:33:23 momjian Exp $
 */
#include <regex/regex.h>
#include <regex/utils.h>
#include <regex/regex2.h>

#include <regex/pg_wchar.h>

main()
{
	/* Example 1 from RFC2044 */
	char		utf1[] = {0x41, 0xe2, 0x89, 0xa2, 0xce, 0x91, 0x2e, 0};

	/* Example 2 from RFC2044 */
	char		utf2[] = {0x48, 0x69, 0x20, 0x4d, 0x6f, 0x6d, 0x20, 0xe2, 0x98, 0xba, 0x21, 0};

	/* Example 3 from RFC2044 */
	char		utf3[] = {0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, 0xe8, 0xaa, 0x9e, 0};
	char	   *utf[] = {utf1, utf2, utf3};
	pg_wchar	ucs[128];
	pg_wchar   *p;
	int			i;

	for (i = 0; i < sizeof(utf) / sizeof(char *); i++)
	{
		pg_utf2wchar(utf[i], ucs);
		p = ucs;
		while (*p)
		{
			printf("%04x ", *p);
			p++;
		}
		printf("\n");
	}
}
