/*-------------------------------------------------------------------------
 *
 * oidcompos.h--
 *	  prototype file for the oid {char16,int4} composite type functions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: oidcompos.h,v 1.2 1997/09/07 05:02:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef OIDCOMPOS_H
#define OIDCOMPOS_H

/* oidint4.c */
OidInt4			oidint4in(char *o);
char		   *oidint4out(OidInt4 o);
bool			oidint4lt(OidInt4 o1, OidInt4 o2);
bool			oidint4le(OidInt4 o1, OidInt4 o2);
bool			oidint4eq(OidInt4 o1, OidInt4 o2);
bool			oidint4ge(OidInt4 o1, OidInt4 o2);
bool			oidint4gt(OidInt4 o1, OidInt4 o2);
bool			oidint4ne(OidInt4 o1, OidInt4 o2);
int				oidint4cmp(OidInt4 o1, OidInt4 o2);
OidInt4			mkoidint4(Oid v_oid, uint32 v_int4);

/* oidint2.c */
OidInt2			oidint2in(char *o);
char		   *oidint2out(OidInt2 o);
bool			oidint2lt(OidInt2 o1, OidInt2 o2);
bool			oidint2le(OidInt2 o1, OidInt2 o2);
bool			oidint2eq(OidInt2 o1, OidInt2 o2);
bool			oidint2ge(OidInt2 o1, OidInt2 o2);
bool			oidint2gt(OidInt2 o1, OidInt2 o2);
bool			oidint2ne(OidInt2 o1, OidInt2 o2);
int				oidint2cmp(OidInt2 o1, OidInt2 o2);
OidInt2			mkoidint2(Oid v_oid, uint16 v_int2);

/* oidname.c */
OidName			oidnamein(char *inStr);
char		   *oidnameout(OidName oidname);
bool			oidnamelt(OidName o1, OidName o2);
bool			oidnamele(OidName o1, OidName o2);
bool			oidnameeq(OidName o1, OidName o2);
bool			oidnamene(OidName o1, OidName o2);
bool			oidnamege(OidName o1, OidName o2);
bool			oidnamegt(OidName o1, OidName o2);
int				oidnamecmp(OidName o1, OidName o2);
OidName			mkoidname(Oid id, char *name);

#endif							/* OIDCOMPOS_H */
