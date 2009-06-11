/*
 * $PostgreSQL: pgsql/contrib/seg/segdata.h,v 1.6 2009/06/11 14:48:52 momjian Exp $
 */
typedef struct SEG
{
	float4		lower;
	float4		upper;
	char		l_sigd;
	char		u_sigd;
	char		l_ext;
	char		u_ext;
} SEG;
