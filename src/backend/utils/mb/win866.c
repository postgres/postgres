/*
 * make KOI8->CP866(ALT) and CP866(ALT)->KOI8 translation table
 * from koi-alt.tab.
 *
 * Tatsuo Ishii
 *
 * $PostgreSQL: pgsql/src/backend/utils/mb/win866.c,v 1.1 2005/03/07 04:30:52 momjian Exp $
 */

#include <stdio.h>


main()
{
	int			i;
	char		koitab[128],
				alttab[128];
	char		buf[4096];
	int			koi,
				alt;

	for (i = 0; i < 128; i++)
		koitab[i] = alttab[i] = 0;

	while (fgets(buf, sizeof(buf), stdin) != NULL)
	{
		if (*buf == '#')
			continue;
		sscanf(buf, "%d %d", &koi, &alt);
		if (koi < 128 || koi > 255 || alt < 128 || alt > 255)
		{
			fprintf(stderr, "invalid value %d\n", koi);
			exit(1);
		}
		koitab[koi - 128] = alt;
		alttab[alt - 128] = koi;
	}

	i = 0;
	printf("static char koi2alt[] = {\n");
	while (i < 128)
	{
		int			j = 0;

		while (j < 8)
		{
			printf("0x%02x", koitab[i++]);
			j++;
			if (i >= 128)
				break;
			printf(", ");
		}
		printf("\n");
	}
	printf("};\n");

	i = 0;
	printf("static char alt2koi[] = {\n");
	while (i < 128)
	{
		int			j = 0;

		while (j < 8)
		{
			printf("0x%02x", alttab[i++]);
			j++;
			if (i >= 128)
				break;
			printf(", ");
		}
		printf("\n");
	}
	printf("};\n");
}
