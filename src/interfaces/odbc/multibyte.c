/*
 * Module :			multibyte.c
 *
 * Description:		Mlutibyte related additional function.
 *
 *					Create 2001-03-03 Eiji Tokuya
 *
 */		
#include <string.h>
#include "multibyte.h"

int multibyte_client_encoding ;	/* Multibyte Client Encoding. */
int multibyte_status ;		/* Multibyte Odds and ends character. */

unsigned char *multibyte_strchr(unsigned char *s,unsigned char c)
{
	int mb_st = 0 ,i = 0;
	while (!(mb_st == 0 && s[i] == c || s[i] == 0))
	{
		if (s[i] == 0)
			return (0);
		switch ( multibyte_client_encoding )
		{
			case SJIS:
			{
				if (mb_st < 2 && s[i] > 0x80 && !(s[i] > 0x9f && s[i] < 0xe0))
					mb_st = 2;
				else if (mb_st == 2)
						mb_st = 1;
					else
						mb_st = 0;
			}
			break;


/* Chinese Big5 Support. */
		case BIG5:
			{
				if ( mb_st < 2 && s[i] > 0xA0 )
						mb_st = 2;
				else if ( mb_st == 2 )
						mb_st = 1;
					else
						mb_st = 0;
			}
			break;
		default:
			{
				mb_st = 0;
			}
		}
		i++;
	}
#ifdef _DEBUG
	qlog("i = %d\n",i);
#endif
	return (s + i);
}

void multibyte_init(void)
{
	multibyte_status = 0;
}

unsigned char *check_client_encoding(unsigned char *str)
{
	if(strstr(str,"%27SJIS%27")||strstr(str,"'SJIS'")||strstr(str,"'sjis'"))
	{
		multibyte_client_encoding = SJIS;
		return ("SJIS");
	}
	if(strstr(str,"%27BIG5%27")||strstr(str,"'BIG5'")||strstr(str,"'big5'"))
	{
		multibyte_client_encoding = BIG5;
		return ("BIG5");
	}
	return ("OHTER");
}

/* 
 * Multibyte Status Function.
 *	Input	char
 *	Output	0	: 1 Byte Character.
 *			1	: MultibyteCharacter Last Byte.
 *			N	: MultibyteCharacter Fast or Middle Byte.
 */
int multibyte_char_check(unsigned char s)
{
	switch ( multibyte_client_encoding )
	{
/* Japanese Shift-JIS(CP932) Support. */
		case SJIS:
		{
			if ( multibyte_status < 2 && s > 0x80 && !(s > 0x9f && s < 0xE0))
				multibyte_status = 2;
			else if (multibyte_status == 2)
				multibyte_status = 1;
			else
				multibyte_status = 0;
		}
		break;

		
/* Chinese Big5(CP950) Support. */
	case BIG5:
		{
			if ( multibyte_status < 2 && s > 0xA0)
				multibyte_status = 2;
			else if (multibyte_status == 2)
				multibyte_status = 1;
			else
				multibyte_status = 0;
		}
		break;
	default:
		{
			multibyte_status = 0;
		}
	}
#ifdef _DEBUG
	qlog("multibyte_client_encoding = %d   s = 0x%02X   multibyte_stat = %d\n", multibyte_client_encoding, s, multibyte_status );
#endif
	return( multibyte_status );
}
