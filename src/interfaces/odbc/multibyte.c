/*--------
 * Module :			multibyte.c
 *
 * Description:		New Mlutibyte related additional function.
 *
 *					Create 2001-03-03 Eiji Tokuya
 *					New Create 2001-09-16 Eiji Tokuya
 *--------
 */

#include "multibyte.h"
#include "connection.h"
#include "pgapifunc.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

int PG_CCST;				/* Client Charcter Status */

int PG_SCSC;				/* Server Charcter Set (code) */
int PG_CCSC;				/* Client Charcter Set (code) */
unsigned char *PG_SCSS;	/* Server Charcter Set (string) */
unsigned char *PG_CCSS;	/* Client Charcter Set (string) */

pg_CS CS_Table[] =
{
	{ "SQL_ASCII",	SQL_ASCII },
	{ "EUC_JP",	EUC_JP },
	{ "EUC_CN",	EUC_CN },
	{ "EUC_KR",	EUC_KR },
	{ "EUC_TW",	EUC_TW },
	{ "JOHAB", JOHAB },
	{ "UNICODE",	UTF8 },
	{ "MULE_INTERNAL",MULE_INTERNAL },
	{ "LATIN1",	LATIN1 },
	{ "LATIN2",	LATIN2 },
	{ "LATIN3",	LATIN3 },
	{ "LATIN4",	LATIN4 },
	{ "LATIN5",	LATIN5 },
	{ "LATIN6", LATIN6 },
	{ "LATIN7", LATIN7 },
	{ "LATIN8", LATIN8 },
	{ "LATIN9", LATIN9 },
	{ "LATIN10", LATIN10 },
	{ "WIN1256", WIN1256 },
	{ "TCVN", TCVN },
	{ "WIN874", WIN874 },
	{ "KOI8",	KOI8R },
	{ "WIN",	WIN1251 },
	{ "ALT",	ALT },
	{ "ISO_8859_5", ISO_8859_5 },
	{ "ISO_8859_6", ISO_8859_6 },
	{ "ISO_8859_7", ISO_8859_7 },
	{ "ISO_8859_8", ISO_8859_8 },


	{ "SJIS",	SJIS },
	{ "BIG5",	BIG5 },
	{ "GBK", GBK },
	{ "UHC", UHC },
	{ "WIN1250",	WIN1250 },
	{ "OTHER", OTHER }
};

int
pg_ismb(int characterset_code)
{
	int i=0,MB_CHARACTERSET[]={EUC_JP,EUC_CN,EUC_KR,EUC_TW,UTF8,MULE_INTERNAL,SJIS,BIG5,GBK,UHC,JOHAB};

	while (MB_CHARACTERSET[i] != characterset_code || OTHER != MB_CHARACTERSET[i] )
	{
		i++;
	}
	return (MB_CHARACTERSET[i]);
}

int
pg_CS_code(const unsigned char *characterset_string)
{
	int i = 0, c;
	for(i = 0; CS_Table[i].code != OTHER; i++)
	{
		if (strstr(characterset_string,CS_Table[i].name))
			c = CS_Table[i].code;
	}
	return (c);
}

unsigned char *
pg_CS_name(const int characterset_code)
{
	int i = 0;
	for (i = 0; CS_Table[i].code != OTHER; i++)
	{
		if (CS_Table[i].code == characterset_code)
			return CS_Table[i].name;
	}
	return ("OTHER");
}

int
pg_CS_stat(int stat,unsigned int character,int characterset_code)
{
	if (character == 0)
		stat = 0;
	switch (characterset_code)
	{
		case UTF8:
			{
				if (stat < 2 &&
					character >= 0x80)
				{
					if (character >= 0xfc)
						stat = 6;
					else if (character >= 0xf8)
						stat = 5;
					else if (character >= 0xf0)
						stat = 4;
					else if (character >= 0xe0)
						stat = 3;
					else if (character >= 0xc0)
						stat = 2;
				}
				else if (stat > 2 &&
					character > 0x7f)
					stat--;
				else
					stat=0;
			}
			break;
/* Shift-JIS Support. */
			case SJIS:
			{
				if (stat < 2 &&
					character > 0x80 &&
					!(character > 0x9f &&
					character < 0xe0))
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;
/* Chinese Big5 Support. */
		case BIG5:
			{
				if (stat < 2 &&
					character > 0xA0)
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;
/* Chinese GBK Support. */
		case GBK:
			{
				if (stat < 2 &&
					character > 0x7F)
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;

/* Korian UHC Support. */
		case UHC:
			{
				if (stat < 2 &&
					character > 0x7F)
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;

/* EUC_JP Support */
		case EUC_JP:
			{
				if (stat < 3 && 
					character == 0x8f)	/* JIS X 0212 */
					stat = 3;
				else
				if (stat != 2 && 
					(character == 0x8e ||
					character > 0xa0))	/* Half Katakana HighByte & Kanji HighByte */
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;

/* EUC_CN, EUC_KR, JOHAB Support */
		case EUC_CN:
		case EUC_KR:
		case JOHAB:
			{
				if (stat < 2 &&
					character > 0xa0)
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;
		case EUC_TW:
			{
				if (stat < 4 &&
					character == 0x8e)
					stat = 4;
				else if (stat == 4 &&
					character > 0xa0)
					stat = 3;
				else if (stat == 3 ||
					stat < 2 &&
					character > 0xa0)
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;
		default:
			{
				stat = 0;
			}
			break;
	}
	return stat;
}


unsigned char *
pg_mbschr(const unsigned char *string, unsigned int character)
{
	int			mb_st = 0;
	unsigned char *s;
	s = (unsigned char *) string;

	for(;;) 
	{
		mb_st = pg_CS_stat(mb_st, (unsigned char) *s,PG_CCSC);
		if (mb_st == 0 && (*s == character || *s == 0))
			break;
		else
			s++;
	}
	return (s);
}

int
pg_mbslen(const unsigned char *string)
{
	unsigned char *s;
	int len, cs_stat;
	for (len = 0, cs_stat = 0, s = (unsigned char *) string; *s != 0; s++)
	{
		cs_stat = pg_CS_stat(cs_stat,(unsigned int) *s, PG_CCSC);
		if (cs_stat < 2)
			len++;
	}
	return len;
}

unsigned char *
pg_mbsinc(const unsigned char *current )
{
	int mb_stat = 0;
	if (*current != 0)
	{
		mb_stat = (int) pg_CS_stat(mb_stat, *current, PG_CCSC);
		if (mb_stat == 0)
			mb_stat = 1;
		return ((unsigned char *) current + mb_stat);
	}
	else
		return NULL;
}

void
CC_lookup_characterset(ConnectionClass *self)
{
	HSTMT		hstmt;
	StatementClass *stmt;
	RETCODE		result;
	static char *func = "CC_lookup_characterset";

	mylog("%s: entering...\n", func);
	PG_SCSS = malloc(MAX_CHARACTERSET_NAME);
	PG_CCSS = malloc(MAX_CHARACTERSET_NAME);

	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return;
	stmt = (StatementClass *) hstmt;

	result = PGAPI_ExecDirect(hstmt, "Show Client_Encoding", SQL_NTS);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}
	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return;
	stmt = (StatementClass *) hstmt;

	result = PGAPI_ExecDirect(hstmt, "Show Server_Encoding", SQL_NTS);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
	{
		PGAPI_FreeStmt(hstmt, SQL_DROP);
		return;
	}

	strcpy(PG_SCSS , pg_CS_name(PG_SCSC = pg_CS_code(PG_SCSS)));
	strcpy(PG_CCSS , pg_CS_name(PG_CCSC = pg_CS_code(PG_CCSS)));

	qlog("    [ Server encoding = '%s' (code = %d), Client encoding = '%s' (code = %d) ]\n", PG_SCSS, PG_SCSC, PG_CCSS, PG_CCSC);
}
