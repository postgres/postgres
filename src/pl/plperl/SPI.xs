/* system stuff */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <setjmp.h>

/* postgreSQL stuff */
#include "executor/spi.h"
#include "commands/trigger.h"
#include "utils/elog.h"
#include "utils/builtins.h"
#include "fmgr.h"
#include "access/heapam.h"

#include "tcop/tcopprot.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"

/* perl stuff */
/*
 * Evil Code Alert
 *
 * both posgreSQL and perl try to do 'the right thing'
 * and provide union semun if the platform doesn't define
 * it in a system header.
 * psql uses HAVE_UNION_SEMUN
 * perl uses HAS_UNION_SEMUN
 * together, they cause compile errors.
 * If we need it, the psql headers above will provide it.
 * So we tell perl that we have it.
 */
#ifndef HAS_UNION_SEMUN
#define HAS_UNION_SEMUN
#endif

#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "eloglvl.h"



MODULE = SPI PREFIX = elog_

PROTOTYPES: ENABLE
VERSIONCHECK: DISABLE

void
elog_elog(level, message)
	int level
	char* message
	CODE:
	if (level > 0)
		return;
	else
		elog(level, message);


int
elog_NOIND()

int
elog_DEBUG()

int
elog_ERROR()

int
elog_NOTICE()
