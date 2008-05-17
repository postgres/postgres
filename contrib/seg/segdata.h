/*
 * $PostgreSQL: pgsql/contrib/seg/segdata.h,v 1.5 2008/05/17 01:28:22 adunstan Exp $ 
 */
typedef struct SEG
{
	float4		lower;
	float4		upper;
	char		l_sigd;
	char		u_sigd;
	char		l_ext;
	char		u_ext;
}	SEG;
