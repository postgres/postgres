/*
 * rmgrdesc.h
 *
 * pg_xlogdump resource managers declaration
 *
 * contrib/pg_xlogdump/rmgrdesc.h
 */
#ifndef RMGRDESC_H
#define RMGRDESC_H

#include "lib/stringinfo.h"

typedef struct RmgrDescData
{
	const char *rm_name;
	void		(*rm_desc) (StringInfo buf, XLogRecord *record);
} RmgrDescData;

extern const RmgrDescData RmgrDescTable[];

#endif   /* RMGRDESC_H */
