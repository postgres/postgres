/*
 * $Id: uconv.c,v 1.1 2000/10/12 06:06:50 ishii Exp $
 */

#include "pg_wchar.h"

/*
 * convert UCS-2 to UTF-8
 * returns number of bytes of a UTF-8, that is atmost 3.
 */
static int
pg_ucs2utf(const unsigned short ucs, unsigned char *utf)
{
	int len;

	if (ucs <= 0x007f)
	{
		*utf = ucs;
		len = 1;
	}
	else if (ucs > 0x007f && ucs <= 0x07ff)
	{
		*utf++ = (ucs >> 6) | 0xc0;
		*utf = (ucs & 0x003f) | 0x80;
		len = 2;
	}
	else
	{
		*utf++ = (ucs >> 12) | 0xe0;
		*utf++ = ((ucs & 0x0fc0) >> 6) | 0x80;
		*utf = (ucs & 0x003f) | 0x80;
		len = 3;
	}
	return (len);
}

typedef struct
{
	unsigned short ucs;	/* UCS-2 */
	unsigned short code;		/* local code */
	unsigned char encoding;		/* encoding */
} ucs_to_local;

typedef struct
{
	unsigned short code;		/* local code */
	unsigned short ucs;	/* UCS-2 */
} local_to_ucs;

#include "ucs_to_iso8859.map"
#include "iso88592.rev"
#include "iso88593.rev"
#include "iso88594.rev"
#include "iso88595.rev"

#define X0208 0
#define X0212 1
#include "ucs_to_jis.map"

int
main()
{
  int i,j;
  int l;
  unsigned int euc;
  unsigned char u[4];
  FILE *fd;

  printf("static pg_utf_to_local mapISO8859[] = {\n");
  for (i=0;i<sizeof(mapISO8859)/sizeof(ucs_to_local);i++) {
	if (mapISO8859[i].encoding > LATIN5)
	  continue;
	l = pg_ucs2utf(mapISO8859[i].ucs, u);
	printf("  {0x");
	for(j=0;j<l;j++) {
	  printf("%02x", u[j]);
	}
	printf(", 0x%04x, %s},\n",
		   mapISO8859[i].code|0x80, 
		   pg_get_enc_ent(mapISO8859[i].encoding)->name);
  }
  printf("};\n");

  printf("\nstatic pg_local_to_utf ISO8859_2[] = {\n");
  for (i=0;i<sizeof(revISO8859_2)/sizeof(local_to_ucs);i++) {
	l = pg_ucs2utf(revISO8859_2[i].ucs, u);
	printf(" {0x%04x, ", revISO8859_2[i].code|0x80);
	printf("0x");
	for(j=0;j<l;j++) {
	  printf("%02x", u[j]);
	}
	printf("},\n");
  }
  printf("};\n");
  
  printf("\nstatic pg_local_to_utf ISO8859_3[] = {\n");
  for (i=0;i<sizeof(revISO8859_3)/sizeof(local_to_ucs);i++) {
	l = pg_ucs2utf(revISO8859_3[i].ucs, u);
	printf(" {0x%04x, ", revISO8859_3[i].code|0x80);
	printf("0x");
	for(j=0;j<l;j++) {
	  printf("%02x", u[j]);
	}
	printf("},\n");
  }
  printf("};\n");

  printf("\nstatic pg_local_to_utf ISO8859_4[] = {\n");
  for (i=0;i<sizeof(revISO8859_4)/sizeof(local_to_ucs);i++) {
	l = pg_ucs2utf(revISO8859_4[i].ucs, u);
	printf(" {0x%04x, ", revISO8859_4[i].code|0x80);
	printf("0x");
	for(j=0;j<l;j++) {
	  printf("%02x", u[j]);
	}
	printf("},\n");
  }
  printf("};\n");

  printf("\nstatic pg_local_to_utf ISO8859_5[] = {\n");
  for (i=0;i<sizeof(revISO8859_5)/sizeof(local_to_ucs);i++) {
	l = pg_ucs2utf(revISO8859_5[i].ucs, u);
	printf(" {0x%04x, ", revISO8859_5[i].code|0x80);
	printf("0x");
	for(j=0;j<l;j++) {
	  printf("%02x", u[j]);
	}
	printf("},\n");
  }
  printf("};\n");

  fd = fopen("UTF_to_EUC_JP.map", "w");
  fprintf(fd, "static pg_utf_to_local mapUTF_to_EUC_JP[] = {\n");
  for (i=0;i<sizeof(mapJIS)/sizeof(ucs_to_local);i++) {
	l = pg_ucs2utf(mapJIS[i].ucs, u);
	fprintf(fd, "  {0x");
	for(j=0;j<l;j++) {
	  fprintf(fd, "%02x", u[j]);
	}
	if (mapJIS[i].encoding == X0208)
	{
		euc = mapJIS[i].code|0x8080;
	}
	else
	{
		euc = SS3 << 16 | mapJIS[i].code | 0x8080;
	}
	fprintf(fd, ", 0x%04x, %s},\n",
		   euc,
		   "EUC_JP");
  }
  fprintf(fd, "};\n");

  fclose(fd);

  return(0);
}
