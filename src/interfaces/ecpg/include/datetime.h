/* $PostgreSQL: pgsql/src/interfaces/ecpg/include/datetime.h,v 1.17 2009/06/11 14:49:13 momjian Exp $ */

#ifndef _ECPG_DATETIME_H
#define _ECPG_DATETIME_H

#include <ecpg_informix.h>

#ifndef _ECPGLIB_H				/* source created by ecpg which defines these
								 * symbols */
typedef timestamp dtime_t;
typedef interval intrvl_t;
#endif   /* ndef _ECPGLIB_H */

#endif   /* ndef _ECPG_DATETIME_H */
