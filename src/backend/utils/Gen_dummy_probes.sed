#-------------------------------------------------------------------------
# sed script to create dummy probes.h file when dtrace is not available
#
# Copyright (c) 2008, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/backend/utils/Gen_dummy_probes.sed,v 1.1 2008/03/17 19:44:41 petere Exp $
#-------------------------------------------------------------------------

/^probe /!d
s/^probe \([^(]*\)\(.*\);/\1\2/
s/__/_/g
y/abcdefghijklmnopqrstuvwxyz/ABCDEFGHIJKLMNOPQRSTUVWXYZ/
s/^/#define TRACE_POSTGRESQL_/
s/(INT, INT)/(INT1, INT2)/
P
s/(.*$/_ENABLED() (0)/
