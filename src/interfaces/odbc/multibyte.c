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
#ifndef	TRUE
#define	TRUE	1
#endif

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
	{ "GB18030",	GB18030 },
	{ "OTHER", OTHER }
};

#ifdef NOT_USED
static int
pg_ismb(int characterset_code)
{
	int i=0,MB_CHARACTERSET[]={EUC_JP,EUC_CN,EUC_KR,EUC_TW,UTF8,MULE_INTERNAL,SJIS,BIG5,GBK,UHC,JOHAB};

	while (MB_CHARACTERSET[i] != characterset_code || OTHER != MB_CHARACTERSET[i] )
	{
		i++;
	}
	return (MB_CHARACTERSET[i]);
}
#endif

int
pg_CS_code(const unsigned char *characterset_string)
{
	int i = 0, c = -1;
  	unsigned len = 0;
	for(i = 0; CS_Table[i].code != OTHER; i++)
	{
		if (strstr(characterset_string,CS_Table[i].name))
		{
                  	if(strlen(CS_Table[i].name) >= len)
                        {
                         	len = strlen(CS_Table[i].name);
                         	c = CS_Table[i].code;
                        }

		}
	}
	if (c < 0)
		c = i;
	return (c);
}

unsigned char *
pg_CS_name(int characterset_code)
{
	int i;
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
				else if ((stat == 3 ||
					stat < 2) &&
					character > 0xa0)
					stat = 2;
				else if (stat == 2)
					stat = 1;
				else
					stat = 0;
			}
			break;
			/*Chinese GB18030 support.Added by Bill Huang <bhuang@redhat.com> <bill_huanghb@ybb.ne.jp>*/
		case GB18030:
			{
				if (stat < 2 && character > 0x80)
					stat = 2;
				else if (stat = 2)
					if (character >= 0x30 && character <= 0x39)
						stat = 3;
					else
						stat = 1;
				else if (stat = 3)
					if (character >= 0x30 && character <= 0x39)
						stat = 1;
					else
						stat = 3;
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
pg_mbschr(int csc, const unsigned char *string, unsigned int character)
{
	int			mb_st = 0;
	const unsigned char *s, *rs = NULL;

	for(s = string; *s ; s++) 
	{
		mb_st = pg_CS_stat(mb_st, (unsigned char) *s, csc);
		if (mb_st == 0 && (*s == character))
		{
			rs = s;
			break;
		}
	}
	return (rs);
}

int
pg_mbslen(int csc, const unsigned char *string)
{
	unsigned char *s;
	int len, cs_stat;
	for (len = 0, cs_stat = 0, s = (unsigned char *) string; *s != 0; s++)
	{
		cs_stat = pg_CS_stat(cs_stat,(unsigned int) *s, csc);
		if (cs_stat < 2)
			len++;
	}
	return len;
}

unsigned char *
pg_mbsinc(int csc, const unsigned char *current )
{
	int mb_stat = 0;
	if (*current != 0)
	{
		mb_stat = (int) pg_CS_stat(mb_stat, *current, csc);
		if (mb_stat == 0)
			mb_stat = 1;
		return ((unsigned char *) current + mb_stat);
	}
	else
		return NULL;
}

static char *
CC_lookup_cs_new(ConnectionClass *self)
{
	char		*encstr = NULL;
	QResultClass	*res;

	res = CC_send_query(self, "select pg_client_encoding()", NULL, CLEAR_RESULT_ON_ABORT);
	if (res)
	{
		char 	*enc = QR_get_value_backend_row(res, 0, 0);

		if (enc)
			encstr = strdup(enc);
		QR_Destructor(res);
	}
	return encstr;
}
static char *
CC_lookup_cs_old(ConnectionClass *self)
{
	char		*encstr = NULL;
	HSTMT		hstmt;
	RETCODE		result;

	result = PGAPI_AllocStmt(self, &hstmt);
	if ((result != SQL_SUCCESS) && (result != SQL_SUCCESS_WITH_INFO))
		return encstr;

	result = PGAPI_ExecDirect(hstmt, "Show Client_Encoding", SQL_NTS);
	if (result == SQL_SUCCESS_WITH_INFO)
	{
		char sqlState[8], errormsg[128], enc[32];

		if (PGAPI_Error(NULL, NULL, hstmt, sqlState, NULL, errormsg,
			sizeof(errormsg), NULL) == SQL_SUCCESS &&
		    sscanf(errormsg, "%*s %*s %*s %*s %*s %s", enc) > 0)
			encstr = strdup(enc);
	}
	PGAPI_FreeStmt(hstmt, SQL_DROP);
	return encstr;
}

void
CC_lookup_characterset(ConnectionClass *self)
{
	char		*encstr;
	static char *func = "CC_lookup_characterset";

	mylog("%s: entering...\n", func);
	if (PG_VERSION_LT(self, 7.2))
		encstr = CC_lookup_cs_old(self);
	else
		encstr = CC_lookup_cs_new(self);
	if (self->client_encoding)
		free(self->client_encoding);
#ifndef	UNICODE_SUPPORT
#ifdef	WIN32
	else
	{
		const char *wenc = NULL;
		switch (GetACP())
		{
			case 932:
				wenc = "SJIS";
				break;
			case 936:
				wenc = "GBK";
				break;
			case 949:
				wenc = "UHC";
				break;
			case 950:
				wenc = "BIG5";
				break;
		}
		if (wenc && stricmp(encstr, wenc))
		{
			QResultClass	*res;
			char		query[64];

			sprintf(query, "set client_encoding to '%s'", wenc);
			res = CC_send_query(self, query, NULL, CLEAR_RESULT_ON_ABORT);
			if (res)
			{
				self->client_encoding = strdup(wenc);
				QR_Destructor(res);
				free(encstr);
				return;
			}
		}
	}
#endif /* WIN32 */
#endif /* UNICODE_SUPPORT */
	if (encstr)
	{
		self->client_encoding = encstr;
		self->ccsc = pg_CS_code(encstr);
		qlog("    [ Client encoding = '%s' (code = %d) ]\n", self->client_encoding, self->ccsc);
		if (stricmp(pg_CS_name(self->ccsc), encstr))
		{
			qlog(" Client encoding = '%s' and %s\n", self->client_encoding, pg_CS_name(self->ccsc));
			self->errornumber = CONN_VALUE_OUT_OF_RANGE;  
			self->errormsg = "client encoding mismatch"; 
		}
	}
	else
	{
		self->ccsc = SQL_ASCII;
		self->client_encoding = NULL;
	}
}

void encoded_str_constr(encoded_str *encstr, int ccsc, const char *str)
{
	encstr->ccsc = ccsc;
	encstr->encstr = str;
	encstr->pos = -1;
	encstr->ccst = 0;
}
int encoded_nextchar(encoded_str *encstr)
{
	int	chr;

	chr = encstr->encstr[++encstr->pos]; 
	encstr->ccst = pg_CS_stat(encstr->ccst, (unsigned int) chr, encstr->ccsc);
	return chr; 
}
int encoded_byte_check(encoded_str *encstr, int abspos)
{
	int	chr;

	chr = encstr->encstr[encstr->pos = abspos]; 
	encstr->ccst = pg_CS_stat(encstr->ccst, (unsigned int) chr, encstr->ccsc);
	return chr; 
}
