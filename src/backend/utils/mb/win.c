/*
 * make KOI8->CP1251(win-1251) and CP1251(win-1251)->KOI8 translation table
 * from koi-win.tab.
 *
 * Tatsuo Ishii
 *
 * $Id: win.c,v 1.3 2001/02/10 02:31:27 tgl Exp $
 */

#include <stdio.h>


main()
{
	int			i;
	char		koitab[128],
				wintab[128];
	char		buf[4096];
	int			koi,
				win;

	for (i = 0; i < 128; i++)
		koitab[i] = wintab[i] = 0;

	while (fgets(buf, sizeof(buf), stdin) != NULL)
	{
		if (*buf == '#')
			continue;
		sscanf(buf, "%d %d", &koi, &win);
		if (koi < 128 || koi > 255 || win < 128 || win > 255)
		{
			fprintf(stderr, "invalid value %d\n", koi);
			exit(1);
		}
		koitab[koi - 128] = win;
		wintab[win - 128] = koi;
	}

	i = 0;
	printf("static char koi2win[] = {\n");
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
	printf("static char win2koi[] = {\n");
	while (i < 128)
	{
		int			j = 0;

		while (j < 8)
		{
			printf("0x%02x", wintab[i++]);
			j++;
			if (i >= 128)
				break;
			printf(", ");
		}
		printf("\n");
	}
	printf("};\n");
}
