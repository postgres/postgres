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
	elog(level, message);


int
elog_DEBUG()

int
elog_LOG()

int
elog_INFO()

int
elog_NOTICE()

int
elog_WARNING()

int
elog_ERROR()


