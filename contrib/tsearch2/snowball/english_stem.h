/* $PostgreSQL: pgsql/contrib/tsearch2/snowball/english_stem.h,v 1.6 2006/03/11 04:38:30 momjian Exp $ */

/* This file was generated automatically by the Snowball to ANSI C compiler */

#ifdef __cplusplus
extern		"C"
{
#endif

extern struct SN_env *english_ISO_8859_1_create_env(void);
extern void english_ISO_8859_1_close_env(struct SN_env * z);

extern int	english_ISO_8859_1_stem(struct SN_env * z);

#ifdef __cplusplus
}

#endif
