/*
 *
 * Multibyte library header ( psqlODBC Only )
 *
 */
#include "psqlodbc.h"

/* PostgreSQL client encoding */
#define SQL_ASCII			0	/* SQL/ASCII */
#define EUC_JP				1	/* EUC for Japanese */
#define EUC_CN				2	/* EUC for Chinese */
#define EUC_KR				3	/* EUC for Korean */
#define EUC_TW				4	/* EUC for Taiwan */
#define JOHAB				5
#define UTF8				6	/* Unicode UTF-8 */
#define MULE_INTERNAL		7	/* Mule internal code */
#define LATIN1				8	/* ISO-8859 Latin 1 */
#define LATIN2				9	/* ISO-8859 Latin 2 */
#define LATIN3				10	/* ISO-8859 Latin 3 */
#define LATIN4				11	/* ISO-8859 Latin 4 */
#define LATIN5				12	/* ISO-8859 Latin 5 */
#define LATIN6				13	/* ISO-8859 Latin 6 */
#define LATIN7				14	/* ISO-8859 Latin 7 */
#define LATIN8				15	/* ISO-8859 Latin 8 */
#define LATIN9				16	/* ISO-8859 Latin 9 */
#define LATIN10				17	/* ISO-8859 Latin 10 */
#define WIN1256				18	/* Arabic Windows */
#define TCVN				19	/* Vietnamese Windows */
#define WIN874				20	/* Thai Windows */
#define KOI8R				21	/* KOI8-R/U */
#define WIN1251				22	/* windows-1251 */
#define ALT					23	/* Alternativny Variant (MS-DOS CP866) */
#define ISO_8859_5			24	/* ISO-8859-5 */
#define ISO_8859_6			25	/* ISO-8859-6 */
#define ISO_8859_7			26	/* ISO-8859-7 */
#define ISO_8859_8			27	/* ISO-8859-8 */

#define SJIS				28	/* Shift JIS */
#define BIG5				29	/* Big5 */
#define GBK					30	/* GBK */
#define UHC					31  /* UHC */
#define WIN1250				32	/* windows-1250 */
#define OTHER				-1

#define MAX_CHARACTERSET_NAME	24
#define MAX_CHARACTER_LEN	6

/* OLD Type */
// extern int	multibyte_client_encoding;	/* Multibyte client encoding. */
// extern int	multibyte_status;	/* Multibyte charcter status. */
//
// void		multibyte_init(void);
// unsigned char *check_client_encoding(unsigned char *sql_string);
// int			multibyte_char_check(unsigned char s);
// unsigned char *multibyte_strchr(const unsigned char *string, unsigned int c);

/* New Type */

extern int PG_CCST;				/* Client Character StaTus */

extern int PG_SCSC;				/* Server Character Set (Code) */
extern int PG_CCSC;				/* Client Character Set (Code) */
extern unsigned char *PG_SCSS;	/* Server Character Set (String) */
extern unsigned char *PG_CCSS;	/* Client Character Set (String) */

extern void CC_lookup_characterset(ConnectionClass *self);

extern int pg_CS_stat(int stat,unsigned int charcter,int characterset_code);
extern int pg_CS_code(const unsigned char *stat_string);
extern unsigned char *pg_CS_name(const int code);

typedef struct pg_CS
{
	unsigned char *name;
	int code;
}pg_CS;
extern pg_CS CS_Table[];

extern int pg_mbslen(const unsigned char *string);
extern unsigned char *pg_mbschr(const unsigned char *string, unsigned int character);
extern unsigned char *pg_mbsinc( const unsigned char *current );

/* Old Type Compatible */
#define multibyte_init() (PG_CCST = 0)
#define multibyte_char_check(X) pg_CS_stat(PG_CCST, (unsigned int) X, PG_CCSC)
#define multibyte_strchr(X,Y) pg_mbschr(X,Y)
#define check_client_encoding(X) pg_CS_name(PG_CCSC = pg_CS_code(X))
