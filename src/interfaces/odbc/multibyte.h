/*
 *
 * Multibyte library header ( psqlODBC Only )
 *
 *
 */

/* PostgreSQL client encoding */
#define SQL_ASCII		0		/* SQL/ASCII */
#define EUC_JP			1		/* EUC for Japanese */
#define EUC_CN			2		/* EUC for Chinese */
#define EUC_KR			3		/* EUC for Korean */
#define EUC_TW			4		/* EUC for Taiwan */
#define UNICODE			5		/* Unicode UTF-8 */
#define MULE_INTERNAL	6		/* Mule internal code */
#define LATIN1			7		/* ISO-8859 Latin 1 */
#define LATIN2			8		/* ISO-8859 Latin 2 */
#define LATIN3			9		/* ISO-8859 Latin 3 */
#define LATIN4			10		/* ISO-8859 Latin 4 */
#define LATIN5			11		/* ISO-8859 Latin 5 */
#define LATIN6			12		/* ISO-8859 Latin 6 */
#define LATIN7			13		/* ISO-8859 Latin 7 */
#define LATIN8			14		/* ISO-8859 Latin 8 */
#define LATIN9			15		/* ISO-8859 Latin 9 */
#define KOI8			16		/* KOI8-R */
#define WIN				17		/* windows-1251 */
#define ALT				18		/* Alternativny Variant (MS-DOS CP866) */
#define SJIS			32		/* Shift JIS */
#define BIG5			33		/* Big5 */
#define WIN1250			34		/* windows-1250 */


extern int	multibyte_client_encoding;	/* Multibyte client encoding. */
extern int	multibyte_status;	/* Multibyte charcter status. */

void		multibyte_init(void);
unsigned char *check_client_encoding(unsigned char *str);
int			multibyte_char_check(unsigned char s);
unsigned char *multibyte_strchr(unsigned char *s, unsigned char c);
