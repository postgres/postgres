/* this must be first: */
#include "postgres.h"

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


