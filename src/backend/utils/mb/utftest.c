/*
 * $Id: utftest.c,v 1.4 2000/10/12 06:06:50 ishii Exp $
 */
#include "conv.c"
#include "wchar.c"
#include "mbutils.c"

int
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
	unsigned char iso[1024];
	int			i;

	/* UTF8-->ISO8859-2 test */
	unsigned char utf_iso8859_2[] = {0x01, 0x00, 0x01, 0x02, 0x01, 0x55, 0x02, 0xdd, 0x00};

	printf("===== testing of pg_utf2wchar_with_len =====\n");

	for (i = 0; i < sizeof(utf) / sizeof(char *); i++)
	{
		pg_utf2wchar_with_len(utf[i], ucs, 128);
		p = ucs;
		while (*p)
		{
			printf("%04x ", *p);
			p++;
		}
		printf("\n");
	}

	printf("===== testing of utf_to_latin2 =====\n");
	utf_to_latin(utf_iso8859_2, iso, LATIN2, 128);
	for (i = 0; i < sizeof(iso) / sizeof(char *); i++)
	{
		printf("%04x ", iso[i]);
		if (iso[i] == 0x00)
			break;
	}
	printf("\n");

	return(0);
}
