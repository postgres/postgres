/*
 * $Id: uconv2.c,v 1.1 2000/10/12 06:06:50 ishii Exp $
 */

#include "pg_wchar.h"

#include "UTF_to_EUC_JP.map"

static int compare1(const void *p1, const void *p2)
{
	unsigned int v1, v2;

	v1 = ((pg_utf_to_local *)p1)->code;
	v2 = ((pg_utf_to_local *)p2)->code;
	return(v1 - v2);
}

int
main()
{
  int i;
  FILE *fd;

  qsort(mapUTF_to_EUC_JP, sizeof(mapUTF_to_EUC_JP)/sizeof(pg_utf_to_local),
		sizeof(pg_utf_to_local),compare1);

  fd = fopen("EUC_JP_to_UTF.map", "w");
  fprintf(fd, "static pg_local_to_utf mapEUC_JP_to_UTF[] = {\n");
  for (i=0;i<sizeof(mapUTF_to_EUC_JP)/sizeof(pg_utf_to_local);i++) {
	fprintf(fd, "  {0x%08x, 0x%08x},\n",
			mapUTF_to_EUC_JP[i].code,
			mapUTF_to_EUC_JP[i].utf);
  }
  fprintf(fd, "};\n");

  fclose(fd);

  return(0);
}
