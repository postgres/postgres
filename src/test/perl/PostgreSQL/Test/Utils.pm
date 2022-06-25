# Copyright (c) 2022, PostgreSQL Global Development Group

# Allow use of release 15+ Perl package name in older branches, by giving that
# package the same symbol table as the older package.

use strict;
use warnings;
BEGIN { *PostgreSQL::Test::Utils:: = \*TestLib::; }
use TestLib ();

1;
