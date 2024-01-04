#-------------------------------------------------------------------------
# sed script to postprocess dtrace output
#
# Copyright (c) 2008-2024, PostgreSQL Global Development Group
#
# src/backend/utils/postprocess_dtrace.sed
#-------------------------------------------------------------------------

# We editorialize on dtrace's output to the extent of changing the macro
# names (from POSTGRESQL_foo to TRACE_POSTGRESQL_foo) and changing any
# "char *" arguments to "const char *".

s/POSTGRESQL_/TRACE_POSTGRESQL_/g
s/( *char \*/(const char */g
s/, *char \*/, const char */g
