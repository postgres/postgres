
/******************************************************************
 *
 *   The PostgreSQL modul for DateTime formating, inspire with
 *   Oracle TO_CHAR() / TO_DATE() routines.  
 *
 *   Copyright (c) 1999, Karel Zak "Zakkr" <zakkr@zf.jcu.cz>
 *
 *   This file is distributed under the GNU General Public License
 *   either version 2, or (at your option) any later version.
 *
 *
 *   NOTE:
 *	In this modul is _not_ used POSIX 'struct tm' type, but 
 *	PgSQL type, which has tm_mon based on one (_non_ zero) and
 *	year not based on 1900, but is used full year number.  
 *	Modul support AC / BC years.	 
 *
 ******************************************************************/

/*
#define DEBUG_TO_FROM_CHAR
#define DEBUG_elog_output	NOTICE
*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/time.h>
#include <unistd.h>

#include "postgres.h"
#include "utils/builtins.h"
           
#include "to-from_char.h"

#define MAX_NODE_SIZ		16	/* maximal length of one node */

#ifdef DEBUG_TO_FROM_CHAR
	#define NOTICE_TM {\
		elog(DEBUG_elog_output, "TM:\nsec %d\nyear %d\nmin %d\nwday %d\nhour %d\nyday %d\nmday %d\nnisdst %d\nmon %d\n",\
			tm->tm_sec, tm->tm_year,\
			tm->tm_min, tm->tm_wday, tm->tm_hour, tm->tm_yday,\
			tm->tm_mday, tm->tm_isdst,tm->tm_mon);\
	}		
#endif

/*------ 
 * (External) defined in PgSQL dt.c (datetime utils) 
 *------
 */
extern  char		*months[],		/* month abbreviation   */
			*days[];		/* full days		*/

/*------
 * Private definitions
 *------
 */
static	struct tm 	_tm, *tm = &_tm;

static char *months_full[]	= {
	"January", "February", "March", "April", "May", "June", "July", 
	"August", "September", "October", "November", "December", NULL        
};

/*------
 * AC / DC
 *------
 */
#define YEAR_ABS(_y)	(_y < 0 ? -(_y -1) : _y)
#define BC_STR		" BC"

/*------ 
 * Months in roman-numeral 
 * (Must be conversely for seq_search (in FROM_CHAR), because 
 *  'VIII' must be over 'V')   
 *------
 */
static char *rm_months[] = {
	"XII", "XI", "X", "IX", "VIII", "VII",
	"VI", "V", "IV", "III", "II", "I", NULL
};

/*------ 
 * Ordinal postfixes 
 *------
 */
static char *numTH[] = { "ST", "ND", "RD", "TH", NULL };
static char *numth[] = { "st", "nd", "rd", "th", NULL };

/*------ 
 * Flags: 
 *------
 */
#define TO_CHAR		1
#define FROM_CHAR 	2

#define ONE_UPPER	1		/* Name	*/
#define ALL_UPPER	2		/* NAME */
#define ALL_LOWER	3		/* name */

#define FULL_SIZ	0

#define MAX_MON_LEN	3
#define MAX_DY_LEN	3

#define TH_UPPER	1
#define TH_LOWER	2

/****************************************************************************
 * 			    Structs for format parsing 
 ****************************************************************************/

/*------
 * Format parser structs             
 *------
 */
typedef struct {
	char		*name;		/* suffix string		*/
	int		len,		/* suffix length		*/
			id,		/* used in node->suffix	*/
			type;		/* prefix / postfix 		*/
} KeySuffix;

typedef struct {
	char		*name;		/* keyword			*/
					/* action for keyword		*/
	int		len,		/* keyword length		*/	
			(*action)(),	
			id;		/* keyword id			*/
} KeyWord;

typedef struct {
	int		type;		/* node type 			*/
	KeyWord		*key;		/* if node type is KEYWORD 	*/
	int		character,	/* if node type is CHAR 	*/
			suffix;		/* keyword suffix 		*/		
} FormatNode;

#define NODE_TYPE_END		0
#define	NODE_TYPE_ACTION	1
#define NODE_TYPE_CHAR		2
#define NODE_LAST		3	/* internal option 	*/

#define SUFFTYPE_PREFIX		1
#define SUFFTYPE_POSTFIX	2


/*****************************************************************************
 *			KeyWords definition & action 
 *****************************************************************************/
 
static int dch_time(int arg, char *inout, int suf, int flag, FormatNode *node);
static int dch_date(int arg, char *inout, int suf, int flag, FormatNode *node);

/*------ 
 * Suffixes: 
 *------
 */
#define	DCH_S_FM	0x01
#define	DCH_S_TH	0x02
#define	DCH_S_th	0x04
#define	DCH_S_SP	0x08

/*------ 
 * Suffix tests 
 *------
 */
#define S_THth(_s)	(((_s & DCH_S_TH) || (_s & DCH_S_th)) ? 1 : 0)
#define S_TH(_s)	((_s & DCH_S_TH) ? 1 : 0)
#define S_th(_s)	((_s & DCH_S_th) ? 1 : 0)
#define S_TH_TYPE(_s)	((_s & DCH_S_TH) ? TH_UPPER : TH_LOWER)

#define S_FM(_s)	((_s & DCH_S_FM) ? 1 : 0)
#define S_SP(_s)	((_s & DCH_S_SP) ? 1 : 0)

/*------
 * Suffixes definition for TO / FROM CHAR
 *------
 */
static KeySuffix suff[] = {
	{	"FM",		2, DCH_S_FM,	SUFFTYPE_PREFIX	 },
	{	"TH",		2, DCH_S_TH,	SUFFTYPE_POSTFIX },		
	{	"th",		2, DCH_S_th,	SUFFTYPE_POSTFIX },		
	{	"SP",		2, DCH_S_SP,	SUFFTYPE_POSTFIX },
	/* last */
	{	NULL,		0, 0,		0		 }	
};

/*------
 *
 * The KeyWord field; alphabetic sorted, *BUT* strings alike is sorted
 *		  complicated -to-> easy: 
 *
 *	(example: "DDD","DD","Day","D" )	
 *
 * (this specific sort needs the algorithm for sequential search for strings,
 * which not has exact end; - How keyword is in "HH12blabla" ? - "HH" 
 * or "HH12"? You must first try "HH12", because "HH" is in string, but 
 * it is not good:-)   
 *
 * (!) Position for the keyword is simular as position in the enum I_poz (!)
 *
 * For fast search is used the KWindex[256], in this index is DCH_ enums for
 * each ASCII position or -1 if char is not used in the KeyWord. Search example 
 * for string "MM":
 * 	1)	see KWindex to KWindex[77] ('M'), 
 *	2)	take keywords position from KWindex[77]
 *	3)	run sequential search in keywords[] from position   
 *
 *------
 */

typedef enum { 
	DCH_CC,
	DCH_DAY,
	DCH_DDD,
	DCH_DD,
	DCH_DY,
	DCH_Day,
	DCH_Dy,
	DCH_D,
	DCH_HH24,
	DCH_HH12,
	DCH_HH,
	DCH_J,
	DCH_MI,
	DCH_MM,
	DCH_MONTH,
	DCH_MON,
	DCH_Month,
	DCH_Mon,
	DCH_Q,
	DCH_RM,
	DCH_SSSS,
	DCH_SS,
	DCH_WW,
	DCH_W,
	DCH_Y_YYY,
	DCH_YYYY,
	DCH_YYY,
	DCH_YY,
	DCH_Y,
	DCH_day,
	DCH_dy,
	DCH_month,
	DCH_mon,
	/* last */
	_DCH_last_
} I_poz;

static KeyWord keywords[] = {	
/*	keyword,	len, func.		I_poz	     is in KWindex */
							
{	"CC",           2, dch_date,	DCH_CC		},	/*C*/
{	"DAY",          3, dch_date,	DCH_DAY		},	/*D*/
{	"DDD",          3, dch_date,	DCH_DDD		},
{	"DD",           2, dch_date,	DCH_DD		},
{	"DY",           2, dch_date,	DCH_DY		},
{	"Day",		3, dch_date,	DCH_Day		},
{	"Dy",           2, dch_date,	DCH_Dy		},
{	"D",            1, dch_date,	DCH_D		},	
{	"HH24",		4, dch_time,	DCH_HH24	},	/*H*/
{	"HH12",		4, dch_time,	DCH_HH12	},
{	"HH",		2, dch_time,	DCH_HH		},
{	"J",            1, dch_date,	DCH_J	 	},	/*J*/	
{	"MI",		2, dch_time,	DCH_MI		},
{	"MM",          	2, dch_date,	DCH_MM		},
{	"MONTH",        5, dch_date,	DCH_MONTH	},
{	"MON",          3, dch_date,	DCH_MON		},
{	"Month",        5, dch_date,	DCH_Month	},
{	"Mon",          3, dch_date,	DCH_Mon		},
{	"Q",            1, dch_date,	DCH_Q		},	/*Q*/	
{	"RM",           2, dch_date,	DCH_RM	 	},	/*R*/
{	"SSSS",		4, dch_time,	DCH_SSSS	},	/*S*/
{	"SS",		2, dch_time,	DCH_SS		},
{	"WW",           2, dch_date,	DCH_WW		},	/*W*/
{	"W",            1, dch_date,	DCH_W	 	},
{	"Y,YYY",        5, dch_date,	DCH_Y_YYY	},	/*Y*/
{	"YYYY",         4, dch_date,	DCH_YYYY	},
{	"YYY",          3, dch_date,	DCH_YYY		},
{	"YY",           2, dch_date,	DCH_YY		},
{	"Y",            1, dch_date,	DCH_Y	 	},
{	"day",		3, dch_date,	DCH_day		},	/*d*/
{	"dy",           2, dch_date,	DCH_dy		},	
{	"month",        5, dch_date,	DCH_month	},	/*m*/	
{	"mon",          3, dch_date,	DCH_mon		},
/* last */
{	NULL,		0, NULL,	0 		}};


static int KWindex[256] = {
/*
0	1	2	3	4	5	6	7	8	9
*/
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	DCH_CC,	DCH_DAY,-1,	
-1,	-1,	DCH_HH24,-1,	DCH_J,	-1,	-1,	DCH_MI,	-1,	-1,
-1,	DCH_Q,	DCH_RM,	DCH_SSSS,-1,	-1,	-1,	DCH_WW,	-1,	DCH_Y_YYY,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
DCH_day,-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	DCH_month,	
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,	-1,
-1,	-1,	-1,	-1,	-1,	-1
};	


/*------
 * Fast sequential search, use index for selection data which
 * go to seq. cycle (it is very fast for non-wanted strings)
 * (can't be used binary search in format parsing)
 *------
 */
static KeyWord *index_seq_search(char *str, KeyWord *kw, int *index) 
{
	int	poz;

	if ( (poz =  *(index + *str)) > -1) {
	
		KeyWord		*k = kw+poz;
	
		do {
			if (! strncmp(str, k->name, k->len))
				return k;
			k++;
			if (!k->name)
				return (KeyWord *) NULL;		  
		} while(*str == *k->name);
	}
	return (KeyWord *) NULL;
}

static KeySuffix *suff_search(char *str, KeySuffix *suf, int type)
{
	KeySuffix	*s;
	
	for(s=suf; s->name != NULL; s++) {
		if (s->type != type)
			continue;
		
		if (!strncmp(str, s->name, s->len))
			return s;
	}
	return (KeySuffix *) NULL;
}

/*------
 * Format parser, search small keywords and keyword's suffixes, and make 
 * format-node tree. 
 *------
 */
#undef FUNC_NAME
#define FUNC_NAME	"parse_format"

static void parse_format(FormatNode *node, char *str, KeyWord *kw, 
			 KeySuffix *suf, int *index)
{
	KeySuffix	*s;
	FormatNode	*n;
	int		node_set=0,
			suffix,
			last=0;
	n = node;

	while(*str) {
		suffix=0;
		
		/* prefix */
		if ((s = suff_search(str, suf, SUFFTYPE_PREFIX)) != NULL) {
			suffix |= s->id;
			if (s->len)
				str += s->len;
		}
	
		/* keyword */
		if (*str && (n->key = index_seq_search(str, kw, index)) != NULL) {
			n->type = NODE_TYPE_ACTION;
			n->suffix = 0;
			node_set= 1;
			if (n->key->len)
				str += n->key->len;
			
			/* postfix */
			if (*str && (s = suff_search(str, suf, SUFFTYPE_POSTFIX)) != NULL) {
				suffix |= s->id;
				if (s->len)
					str += s->len;
			}
			
		} else if (*str) {
		/* special characters '\' and '"' */

			if (*str == '"' && last != '\\') {
				while(*(++str)) {
					if (*str == '"') {
						str++;
						break;
					}
					n->type = NODE_TYPE_CHAR;
					n->character = *str;
					n->key = (KeyWord *) NULL;
					n->suffix = 0;
					++n;
				}
				node_set = 0;
				suffix = 0;
				last = 0;
				
			} else if (*str && *str == '\\' && last!='\\' && *(str+1) =='"') {
				last = *str;
				str++;
			
			} else if (*str) {
				n->type = NODE_TYPE_CHAR;
				n->character = *str;
				n->key = (KeyWord *) NULL;
				node_set = 1;
				last = 0;
				str++;
			}
		}
		
		/* end */	
		if (node_set) {
			if (n->type == NODE_TYPE_ACTION) 
				n->suffix = suffix;	
			++n;
			n->suffix = 0;
			node_set = 0;
		}	
	}
	
	n->type = NODE_TYPE_END;
	n->suffix = 0;
	return;
}

/*------
 * Call keyword's function for each of (action) node in format-node tree 
 *------
 */
static char *node_action(FormatNode *node, char *inout, int flag)
{
	FormatNode	*n;
	char		*s;
	
	for(n=node, s=inout; n->type != NODE_TYPE_END; n++, s++) {
		if (n->type == NODE_TYPE_ACTION) {
			
			int 	len;
			
			/*
			 * Call node action function 
			 */
			len = n->key->action(n->key->id, s, n->suffix, flag, n); 	
			if (len > 0)
			s += len;
			
		} else {
		
			/*
			 * Remove to output char from input in TO_CHAR
			 */
			if (flag == TO_CHAR) 
				*s = n->character;
			
			else {				
				/*
				 * Skip blank space in FROM_CHAR's input 
				 */
				if (isspace(n->character)) {
					while(*s != '\0' && isspace(*(s+1)))
						++s;	
				}
			}
		}	
	}
	
	if (flag == TO_CHAR)
		*s = '\0';
	return inout;
}

/*****************************************************************************
 *			Private utils 
 *****************************************************************************/

/*------
 * Return ST/ND/RD/TH for simple (1..9) numbers 
 * type --> 0 upper, 1 lower
 *------	
 */	
static char *get_th(int num, int type)
{
	switch(num) {
		case 1:
			if (type==TH_UPPER) return numTH[0];
			return numth[0];
		case 2:
			if (type==TH_UPPER) return numTH[1];
			return numth[1];
		case 3:
			if (type==TH_UPPER) return numTH[2];
			return numth[2];		
	}
	if (type==TH_UPPER) return numTH[3];
	return numth[3];
}

/*------
 * Convert string-number to ordinal string-number 
 * type --> 0 upper, 1 lower	
 *------
 */
#undef 	FUNC_NAME
#define	FUNC_NAME	"str_numth"

static char *str_numth(char *dest, char *src, int type)
{
	int	len = strlen(src),
		num=0, f_num=0;	
	
	num = *(src+(len-1));
	if (num < 48 || num > 57)
		elog(ERROR, "%s: in '%s' is not number.", FUNC_NAME, src);
	
	num -= 48;
	if (num==1 || num==2) { 		/* 11 || 12 */
		f_num = atoi(src);
		if (abs(f_num)==11 || abs(f_num)==12)
			num=0;
	}
	sprintf(dest, "%s%s", src, get_th(num, type));
	return dest; 
}

/*------
 * Return length of integer writed in string 
 *-------
 */
static int int4len(int4 num)
{
 	char b[16];
 	
 	sprintf(b, "%d", num);
 	return strlen(b);
}

/*------
 * Convert string to upper-string
 *------
 */
static char *str_toupper(char *buff)
{	
	char	*p_buff=buff;

	while (*p_buff) {
		*p_buff = toupper((unsigned char) *p_buff);
		++p_buff;
	}
	return buff;
}

/*------
 * Check if in string is AC or BC (return: 0==none; -1==BC; 1==AC)  
 *------
 */
static int is_acdc(char *str, int *len)
{
	char	*p;
	
	for(p=str; *p != '\0'; p++) {
		if (isspace(*p))
			continue;
			
		if (*(p+1)) { 
			if (toupper(*p)=='B' && toupper(*(++p))=='C') {
			   	*len += (p - str) +1;
				return -1;   	
			} else if (toupper(*p)=='A' && toupper(*(++p))=='C') {
		   		*len += (p - str) +1;
				return 1;   	
		 	}
		} 
		return 0; 	
	}
	return 0;
} 
 

/*------
 * Sequential search with to upper/lower conversion
 *------
 */
static int seq_search(char *name, char **array, int type, int max, int *len)
{
	char	*p, *n, **a;
	int	last, i;
	
	*len = 0;
	
	if (!*name) 
		return -1;
	
        /* set first char */	
	if (type == ONE_UPPER || ALL_UPPER) 
		*name = toupper((unsigned char) *name);
	else if (type == ALL_LOWER)
		*name = tolower((unsigned char) *name);
		
	for(last=0, a=array; *a != NULL; a++) {
		
		/* comperate first chars */
		if (*name != **a)
			continue;
		
		for(i=1, p=*a+1, n=name+1; ; n++, p++, i++) {
			
			/* search fragment (max) only */
			if (max && i == max) {
				*len = i;
				return a - array;
			} 
			/* full size */
			if (*p=='\0') {
				*len = i;
				return a - array;
			}
			/* Not found in array 'a' */
			if (*n=='\0')
				break;
			
			/* 
			 * Convert (but convert new chars only)
			 */
			if (i > last) {
				if (type == ONE_UPPER || type == ALL_LOWER) 
					*n = tolower((unsigned char) *n);
				else if (type == ALL_UPPER)	
					*n = toupper((unsigned char) *n);
				last=i;
			}

#ifdef DEBUG_TO_FROM_CHAR			
			elog(DEBUG_elog_output, "N: %c, P: %c, A: %s (%s)", *n, *p, *a, name);
#endif
			
			if (*n != *p)
				break; 
		}  	
	}
		
	return -1;		
}


#ifdef DEBUG_TO_FROM_CHAR
/*-------
 * Call for debug and for KWindex checking; (Show ASCII char and defined 
 * keyword for each used position  
 *-------
 */	
static void dump_KWindex()
{
	int	i;
	
	for(i=0; i<255; i++) {
		if (KWindex[i] != -1)
			elog(NOTICE, "%c: %s, ", i, keywords[ KWindex[i] ].name);
	}		
}
#endif

/*****************************************************************************
 *			  	Master routines  
 *****************************************************************************/

/*
 * Spip TM / th in FROM_CHAR
 */
#define SKIP_THth(_suf)		(S_THth(_suf) ? 2 : 0)   

/*------
 * Master of TIME for TO_CHAR   - write (inout) formated string
 *                    FROM_CHAR - scan (inout) string by course of FormatNode 
 *------
 */
#undef FUNC_NAME
#define FUNC_NAME	"dch_time"

static int dch_time(int arg, char *inout, int suf, int flag, FormatNode *node) 
{
	char	*p_inout = inout;

	switch(arg) {
	case DCH_HH:	
	case DCH_HH12:
		if (flag == TO_CHAR) {
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, 
				tm->tm_hour==0 ? 12 :
				tm->tm_hour <13	? tm->tm_hour : tm->tm_hour-12);
			if (S_THth(suf)) 
				str_numth(p_inout, inout, 0);
			if (S_FM(suf) || S_THth(suf)) return strlen(p_inout)-1;
			else return 1;
		} else if (flag == FROM_CHAR) {
			if (S_FM(suf)) {
				sscanf(inout, "%d", &tm->tm_hour);
				return int4len((int4) tm->tm_hour)-1 + SKIP_THth(suf);
			} else {
				sscanf(inout, "%02d", &tm->tm_hour);
				return 1 + SKIP_THth(suf);
			}
				
		}
	case DCH_HH24:
		if (flag == TO_CHAR) {
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_hour);
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_FM(suf) || S_THth(suf)) return strlen(p_inout)-1;
			else return 1;
		} else if (flag == FROM_CHAR) {
			if (S_FM(suf)) {
				sscanf(inout, "%d", &tm->tm_hour);
				return int4len((int4) tm->tm_hour)-1 + SKIP_THth(suf);
			} else {
				sscanf(inout, "%02d", &tm->tm_hour);
				return 1 + SKIP_THth(suf);
			}
		}
	case DCH_MI:	
		if (flag == TO_CHAR) {
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_min);
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_FM(suf) || S_THth(suf)) return strlen(p_inout)-1;
			else return 1;
		} else if (flag == FROM_CHAR) {
			if (S_FM(suf)) {
				sscanf(inout, "%d", &tm->tm_min);
				return int4len((int4) tm->tm_min)-1 + SKIP_THth(suf);
			} else {
				sscanf(inout, "%02d", &tm->tm_min);
				return 1 + SKIP_THth(suf);
			}
		}
	case DCH_SS:	
		if (flag == TO_CHAR) {
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_sec);
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_FM(suf) || S_THth(suf)) return strlen(p_inout)-1;
			else return 1;
		} else if (flag == FROM_CHAR) {
			if (S_FM(suf)) {
				sscanf(inout, "%d", &tm->tm_sec);
				return int4len((int4) tm->tm_sec)-1 + SKIP_THth(suf);
			} else {
				sscanf(inout, "%02d", &tm->tm_sec);
				return 1 + SKIP_THth(suf);
			}
		}
	case DCH_SSSS:	
		if (flag == TO_CHAR) {
			sprintf(inout, "%d", tm->tm_hour	* 3600	+
				    tm->tm_min	* 60	+
				    tm->tm_sec);
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			return strlen(p_inout)-1;		
		}  else if (flag == FROM_CHAR) 
			elog(ERROR, "%s: SSSS is not supported", FUNC_NAME);
	}	
	return 0;	
}

#define CHECK_SEQ_SEARCH(_l, _s) {					\
	if (_l <= 0) { 							\
		elog(ERROR, "%s: bad value for %s", FUNC_NAME, _s);	\
	}								\
}

/*------
 * Master of DATE for TO_CHAR   - write (inout) formated string
 *                    FROM_CHAR - scan (inout) string by course of FormatNode 
 *------
 */
#undef FUNC_NAME
#define FUNC_NAME	"dch_date"

static int dch_date(int arg, char *inout, int suf, int flag, FormatNode *node)
{
	char	buff[MAX_NODE_SIZ], 		
		*p_inout;
	int	i, len;

	p_inout = inout;

	/*------
	 * In the FROM-char is not difference between "January" or "JANUARY" 
	 * or "january", all is before search convert to one-upper.    
	 * This convention is used for MONTH, MON, DAY, DY
	 *------
	 */
	if (flag == FROM_CHAR) {
		if (arg == DCH_MONTH || arg == DCH_Month || arg == DCH_month) {
	
			tm->tm_mon = seq_search(inout, months_full, ONE_UPPER, FULL_SIZ, &len);
			CHECK_SEQ_SEARCH(len, "MONTH/Month/month");
			++tm->tm_mon;
			if (S_FM(suf))	return len-1;
			else 		return 8;

		} else if (arg == DCH_MON || arg == DCH_Mon || arg == DCH_mon) {
		
			tm->tm_mon = seq_search(inout, months, ONE_UPPER, MAX_MON_LEN, &len);
			CHECK_SEQ_SEARCH(len, "MON/Mon/mon");
			++tm->tm_mon;
			return 2;
		
		} else if (arg == DCH_DAY || arg == DCH_Day || arg == DCH_day) {
		
			tm->tm_wday =  seq_search(inout, days, ONE_UPPER, FULL_SIZ, &len);
			CHECK_SEQ_SEARCH(len, "DAY/Day/day");
			if (S_FM(suf))	return len-1;
			else 		return 8;
			
		} else if (arg == DCH_DY || arg == DCH_Dy || arg == DCH_dy) {
			
			tm->tm_wday =  seq_search(inout, days, ONE_UPPER, MAX_DY_LEN, &len);
			CHECK_SEQ_SEARCH(len, "DY/Dy/dy");
			return 2;
			
		} 
	} 
	
	switch(arg) {
	case DCH_MONTH:
		strcpy(inout, months_full[ tm->tm_mon - 1]);		
		sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, str_toupper(inout));	
		if (S_FM(suf)) return strlen(p_inout)-1;
		else return 8;
	case DCH_Month:
		sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, months_full[ tm->tm_mon -1 ]);		
	        if (S_FM(suf)) return strlen(p_inout)-1;
		else return 8;
	case DCH_month:
		sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, months_full[ tm->tm_mon -1 ]);
		*inout = tolower(*inout);
	        if (S_FM(suf)) return strlen(p_inout)-1;
		else return 8;
	case DCH_MON:
		strcpy(inout, months[ tm->tm_mon -1 ]);		
		inout = str_toupper(inout);
		return 2;
	case DCH_Mon:
		strcpy(inout, months[ tm->tm_mon -1 ]);		
		return 2;     
	case DCH_mon:
		strcpy(inout, months[ tm->tm_mon -1 ]);		
		*inout = tolower(*inout);
		return 2;
	case DCH_MM:
		if (flag == TO_CHAR) {
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_mon );
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_FM(suf) || S_THth(suf)) 
				return strlen(p_inout)-1;
			else return 1;
		} else if (flag == FROM_CHAR) {
			if (S_FM(suf)) {
				sscanf(inout, "%d", &tm->tm_mon);
				return int4len((int4) tm->tm_mon)-1 + SKIP_THth(suf);
			} else {
				sscanf(inout, "%02d", &tm->tm_mon);
				return 1 + SKIP_THth(suf);
			}		
		}	
	case DCH_DAY:
		strcpy(inout, days[ tm->tm_wday ]); 			        
		sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, str_toupper(inout)); 
	        if (S_FM(suf)) return strlen(p_inout)-1;
		else return 8;	
	case DCH_Day:
		sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, days[ tm->tm_wday]);			
	       	if (S_FM(suf)) return strlen(p_inout)-1;
		else return 8;			
	case DCH_day:
		sprintf(inout, "%*s", S_FM(suf) ? 0 : -9, days[ tm->tm_wday]);			
		*inout = tolower(*inout);
	       	if (S_FM(suf)) return strlen(p_inout)-1;
		else return 8;
	case DCH_DY:	        
		strcpy(inout, days[ tm->tm_wday]);
		inout = str_toupper(inout);		
		return 2;
	case DCH_Dy:
		strcpy(inout, days[ tm->tm_wday]);			
		return 2;			
	case DCH_dy:
		strcpy(inout, days[ tm->tm_wday]);			
		*inout = tolower(*inout);
		return 2;
	case DCH_DDD:
		if (flag == TO_CHAR) {
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 3, tm->tm_yday);
	        	if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_FM(suf) || S_THth(suf)) 
				return strlen(p_inout)-1;
			else return 2;
		} else if (flag == FROM_CHAR) {	
			if (S_FM(suf)) {
				sscanf(inout, "%d", &tm->tm_yday);
				return int4len((int4) tm->tm_yday)-1 + SKIP_THth(suf);
			} else {
				sscanf(inout, "%03d", &tm->tm_yday);
				return 2 + SKIP_THth(suf);
			}	
		}
	case DCH_DD:
		if (flag == TO_CHAR) {	
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, tm->tm_mday);	
	        	if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_FM(suf) || S_THth(suf)) 
				return strlen(p_inout)-1;
			else return 1;
		} else if (flag == FROM_CHAR) {	
			if (S_FM(suf)) {
				sscanf(inout, "%d", &tm->tm_mday);
				return int4len((int4) tm->tm_mday)-1 + SKIP_THth(suf);
			} else {
				sscanf(inout, "%02d", &tm->tm_mday);
				return 1 + SKIP_THth(suf);
			}	
		}	
	case DCH_D:
		if (flag == TO_CHAR) {
			sprintf(inout, "%d", tm->tm_wday+1);
	        	if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_THth(suf)) 
				return 2;
	        	return 0;
	        } else if (flag == FROM_CHAR) {	
			sscanf(inout, "%1d", &tm->tm_wday);
			if(tm->tm_wday) --tm->tm_wday;
			return 0 + SKIP_THth(suf);
		} 
	case DCH_WW:
		if (flag == TO_CHAR) {
			sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2,  
				(tm->tm_yday - tm->tm_wday + 7) / 7);		
	        	if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_FM(suf) || S_THth(suf)) 
				return strlen(p_inout)-1;
			else return 1;
		}  else if (flag == FROM_CHAR)
			elog(ERROR, "%s: WW is not supported", FUNC_NAME);
	case DCH_Q:
		if (flag == TO_CHAR) {
			sprintf(inout, "%d", (tm->tm_mon-1)/3+1);		
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_THth(suf)) 
				return 2;
	        	return 0;
	        } else if (flag == FROM_CHAR)
			elog(ERROR, "%s: Q is not supported", FUNC_NAME);
	case DCH_CC:
		if (flag == TO_CHAR) {
			i = tm->tm_year/100 +1;
			if (i <= 99 && i >= -99)	
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 2, i);
			else
				sprintf(inout, "%d", i);			
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			return strlen(p_inout)-1;
		} else if (flag == FROM_CHAR)
			elog(ERROR, "%s: CC is not supported", FUNC_NAME);
	case DCH_Y_YYY:	
		if (flag == TO_CHAR) {
			i= YEAR_ABS(tm->tm_year) / 1000;
			sprintf(inout, "%d,%03d", i, YEAR_ABS(tm->tm_year) -(i*1000));
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (tm->tm_year < 0)
				strcat(inout, BC_STR);	
			return strlen(p_inout)-1;
		} else if (flag == FROM_CHAR) {
			int	cc, yy;
			sscanf(inout, "%d,%03d", &cc, &yy);
			tm->tm_year = (cc * 1000) + yy;
			
			if (!S_FM(suf) && tm->tm_year <= 9999 && tm->tm_year >= -9999)	
				len = 5; 
			else 					
				len = int4len((int4) tm->tm_year)+1;
			len +=  SKIP_THth(suf);	
			/* AC/BC */ 	
			if (is_acdc(inout+len, &len) < 0 && tm->tm_year > 0) 
				tm->tm_year = -(tm->tm_year);
			if (tm->tm_year < 0) 
				tm->tm_year = tm->tm_year+1; 
			return len-1;
		}	
	case DCH_YYYY:
		if (flag == TO_CHAR) {
			if (tm->tm_year <= 9999 && tm->tm_year >= -9998)
				sprintf(inout, "%0*d", S_FM(suf) ? 0 : 4,  YEAR_ABS(tm->tm_year));
			else
				sprintf(inout, "%d", YEAR_ABS(tm->tm_year));	
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (tm->tm_year < 0)
				strcat(inout, BC_STR);
			return strlen(p_inout)-1;
		} else if (flag == FROM_CHAR) {
			sscanf(inout, "%d", &tm->tm_year);
			if (!S_FM(suf) && tm->tm_year <= 9999 && tm->tm_year >= -9999)	
				len = 4;
			else 					
				len = int4len((int4) tm->tm_year);
			len +=  SKIP_THth(suf);
			/* AC/BC */ 	
			if (is_acdc(inout+len, &len) < 0 && tm->tm_year > 0) 
				tm->tm_year = -(tm->tm_year);
			if (tm->tm_year < 0) 
				tm->tm_year = tm->tm_year+1; 
			return len-1;
		}	
	case DCH_YYY:
		if (flag == TO_CHAR) {
			sprintf(buff, "%03d", YEAR_ABS(tm->tm_year));
			i=strlen(buff);
			strcpy(inout, buff+(i-3));
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_THth(suf)) return 4;
			return 2;
		} else if (flag == FROM_CHAR) {
			int	yy;
			sscanf(inout, "%03d", &yy);
			tm->tm_year = (tm->tm_year/1000)*1000 +yy;
			return 2 +  SKIP_THth(suf);
		}	
	case DCH_YY:
		if (flag == TO_CHAR) {
			sprintf(buff, "%02d", YEAR_ABS(tm->tm_year));
			i=strlen(buff);
			strcpy(inout, buff+(i-2));
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_THth(suf)) return 3;
			return 1;
		} else if (flag == FROM_CHAR) {
			int	yy;
			sscanf(inout, "%02d", &yy);
			tm->tm_year = (tm->tm_year/100)*100 +yy;
			return 1 +  SKIP_THth(suf);
		}	
	case DCH_Y:
		if (flag == TO_CHAR) {
			sprintf(buff, "%1d", YEAR_ABS(tm->tm_year));
			i=strlen(buff);
			strcpy(inout, buff+(i-1));
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_THth(suf)) return 2;
			return 0;
		} else if (flag == FROM_CHAR) {
			int	yy;
			sscanf(inout, "%1d", &yy);
			tm->tm_year = (tm->tm_year/10)*10 +yy;
			return 0 +  SKIP_THth(suf);
		}	
	case DCH_RM:
		if (flag == TO_CHAR) {
			sprintf(inout, "%*s", S_FM(suf) ? 0 : -4,   
				rm_months[ 12 - tm->tm_mon ]);
			if (S_FM(suf)) return strlen(p_inout)-1;
			else return 3;
		} else if (flag == FROM_CHAR) {
			tm->tm_mon = 11-seq_search(inout, rm_months, ALL_UPPER, FULL_SIZ, &len);
			CHECK_SEQ_SEARCH(len, "RM");
			++tm->tm_mon;
			if (S_FM(suf))	return len-1;
			else 		return 3;
		}	
	case DCH_W:
		if (flag == TO_CHAR) {
			sprintf(inout, "%d", (tm->tm_mday - tm->tm_wday +7) / 7 );
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			if (S_THth(suf)) return 2;
			return 0;
		} else if (flag == FROM_CHAR)
			elog(ERROR, "%s: W is not supported", FUNC_NAME);
	case DCH_J:
		if (flag == TO_CHAR) {
			sprintf(inout, "%d", date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));		
			if (S_THth(suf)) 
				str_numth(p_inout, inout, S_TH_TYPE(suf));
			return strlen(p_inout)-1;
		} else if (flag == FROM_CHAR)
			elog(ERROR, "%s: J is not supported", FUNC_NAME);	
	}	
	return 0;	
}

/****************************************************************************
 *				Public routines
 ***************************************************************************/

	
/********************************************************************* 
 *
 * to_char
 *
 * Syntax:
 *
 *	text *to_char(DateTime *dt, text *fmt)
 *
 * Purpose:
 *
 *	Returns string, with date and/or time, formated at 
 *      argument 'fmt'  		 
 *
 *********************************************************************/

#undef FUNC_NAME
#define FUNC_NAME	"to_char"
	
text 
*to_char(DateTime *dt, text *fmt)
{
	FormatNode		*tree;
	text 			*result;
	char			*pars_str;
	double          	fsec;
	char       		*tzn;
	int			len=0, tz;

	if ((!PointerIsValid(dt)) || (!PointerIsValid(fmt)))
		return NULL;
	
	len 	= VARSIZE(fmt) - VARHDRSZ; 
	
	if (!len) 
		return textin("");

	tm->tm_sec	=0;	tm->tm_year	=0;
	tm->tm_min	=0;	tm->tm_wday	=0;
	tm->tm_hour	=0;	tm->tm_yday	=0;
	tm->tm_mday	=1;	tm->tm_isdst	=0;
	tm->tm_mon	=1;

	if (DATETIME_IS_EPOCH(*dt))
	{
		datetime2tm(SetDateTime(*dt), NULL, tm, &fsec, NULL);
	} else if (DATETIME_IS_CURRENT(*dt)) {
		datetime2tm(SetDateTime(*dt), &tz, tm, &fsec, &tzn);
	} else {
		if (datetime2tm(*dt, &tz, tm, &fsec, &tzn) != 0)
			elog(ERROR, "s%: Unable to convert datetime to tm", FUNC_NAME);
	}

	/* In dt.c is j2day as static :-(((
	 	tm->tm_wday = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)); 
	   must j2day convert itself... 
	 */
	 
	tm->tm_wday  = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) + 1) % 7; 
	tm->tm_yday  = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(tm->tm_year, 1,1) +1;
	
	tree 	= (FormatNode *) palloc(len * sizeof(FormatNode) +1);	
	result	= (text *) palloc( len * MAX_NODE_SIZ + VARHDRSZ);	
	(tree + len)->type = NODE_LAST;	  
	
	pars_str = VARDATA(fmt);
	pars_str[ len ] = '\0';

	parse_format( tree, pars_str, keywords, suff, KWindex);

	node_action(tree, VARDATA(result), TO_CHAR);
	VARSIZE(result) = strlen(VARDATA(result)) + VARHDRSZ; 
	
	return result;
}


/********************************************************************* 
 *
 * from_char
 *
 * Syntax:
 *
 *	DateTime *from_char(text *date_str, text *fmt)
 *
 * Purpose:
 *
 *	Make DateTime from date_str which is formated at argument 'fmt'  	 
 *	( from_char is reverse to_char() )
 *
 *********************************************************************/

#undef FUNC_NAME
#define FUNC_NAME	"from_char"
	
DateTime 
*from_char(text *date_str, text *fmt)
{
	FormatNode		*tree;
	DateTime		*result;
	char			*pars_str;
	int			len=0,
				fsec=0,
				tz=0;

	if ((!PointerIsValid(date_str)) || (!PointerIsValid(fmt)))
		return NULL;
	
	tm->tm_sec	=0;	tm->tm_year	=0;
	tm->tm_min	=0;	tm->tm_wday	=0;
	tm->tm_hour	=0;	tm->tm_yday	=0;
	tm->tm_mday	=1;	tm->tm_isdst	=0;
	tm->tm_mon	=1;
	
	result = palloc(sizeof(DateTime));
	
	len 	= VARSIZE(fmt) - VARHDRSZ; 
	
	if (len) { 
		tree 	= (FormatNode *) palloc((len+1) * sizeof(FormatNode));	
		(tree + len)->type = NODE_LAST;	  
	
		pars_str = VARDATA(fmt);
		pars_str[ len ] = '\0';
		parse_format( tree, pars_str, keywords, suff, KWindex);
		VARDATA(date_str)[ VARSIZE(date_str) - VARHDRSZ ] = '\0';
		node_action(tree, VARDATA(date_str), FROM_CHAR);	
	}

#ifdef DEBUG_TO_FROM_CHAR
	NOTICE_TM; 
#endif
	if (IS_VALID_UTIME(tm->tm_year, tm->tm_mon, tm->tm_mday)) {

#ifdef USE_POSIX_TIME
		tm->tm_isdst = -1;
		tm->tm_year -= 1900;
			tm->tm_mon -= 1;

#ifdef DEBUG_TO_FROM_CHAR
		elog(NOTICE, "TO-FROM_CHAR: Call mktime()");
		NOTICE_TM;
#endif
		mktime(tm);
		tm->tm_year += 1900;
		tm->tm_mon += 1;

#if defined(HAVE_TM_ZONE)
		tz = -(tm->tm_gmtoff);	/* tm_gmtoff is Sun/DEC-ism */
#elif defined(HAVE_INT_TIMEZONE)

#ifdef __CYGWIN__
		tz = (tm->tm_isdst ? (_timezone - 3600) : _timezone);
#else
		tz = (tm->tm_isdst ? (timezone - 3600) : timezone);
#endif

#else
#error USE_POSIX_TIME is defined but neither HAVE_TM_ZONE or HAVE_INT_TIMEZONE are defined
#endif

#else		/* !USE_POSIX_TIME */
		tz = CTimeZone;
#endif
	} else {
		tm->tm_isdst = 0;
		tz = 0;
	}
#ifdef DEBUG_TO_FROM_CHAR
	NOTICE_TM; 
#endif
	if (tm2datetime(tm, fsec, &tz, result) != 0)
        	elog(ERROR, "%s: can't convert 'tm' to datetime.", FUNC_NAME);
	
	return result;
}

/********************************************************************* 
 *
 * to_date
 *
 * Syntax:
 *
 *	DateADT *to_date(text *date_str, text *fmt)
 *
 * Purpose:
 *
 *	Make Date from date_str which is formated at argument 'fmt'  	 
 *
 *********************************************************************/

#undef FUNC_NAME
#define FUNC_NAME	"to_date"
	
DateADT  
to_date(text *date_str, text *fmt)
{
	return datetime_date( from_char(date_str, fmt) );
}


/********************************************************************
 *
 * ordinal
 *
 * Syntax:
 *	
 *	text *ordinal(int4 num, text type)
 *
 * Purpose:
 *
 *	Add to number 'th' suffix and return this as text.
 *
 ********************************************************************/
 
#undef	FUNC_NAME	
#define FUNC_NAME	"ordinal"
 
text
*ordinal(int4 num, text *typ)
{
 	text	*result;
 	int	ttt=0;
 	
 	if (!PointerIsValid(typ))
		return NULL;
 	
 	VARDATA(typ)[ VARSIZE(typ) - VARHDRSZ ] = '\0';
 	
 	if (!strcmp("TH", VARDATA(typ)))
 		ttt = TH_UPPER;	
 	else if (!strcmp("th", VARDATA(typ)))
 		ttt = TH_LOWER;
 	else
 		elog(ERROR, "%s: bad type '%s' (allowed: 'TH' or 'th')", 
 			 FUNC_NAME, VARDATA(typ));
 
 	result = (text *) palloc(16);	/* ! not int8 ! */
 
 	sprintf(VARDATA(result), "%d", (int) num);
 	str_numth(VARDATA(result), VARDATA(result), ttt);

 	VARSIZE(result) = strlen(VARDATA(result)) + VARHDRSZ;

 	return result;
}
