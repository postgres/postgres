/*
 * $PostgreSQL: pgsql/contrib/tablefunc/tablefunc.h,v 1.16 2009/06/11 14:48:52 momjian Exp $
 *
 *
 * tablefunc
 *
 * Sample to demonstrate C functions which return setof scalar
 * and setof composite.
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Nabil Sayegh <postgresql@e-trolley.de>
 *
 * Copyright (c) 2002-2009, PostgreSQL Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#ifndef TABLEFUNC_H
#define TABLEFUNC_H

#include "fmgr.h"

/*
 * External declarations
 */
extern Datum normal_rand(PG_FUNCTION_ARGS);
extern Datum crosstab(PG_FUNCTION_ARGS);
extern Datum crosstab_hash(PG_FUNCTION_ARGS);
extern Datum connectby_text(PG_FUNCTION_ARGS);
extern Datum connectby_text_serial(PG_FUNCTION_ARGS);

#endif   /* TABLEFUNC_H */
