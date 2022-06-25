# Copyright (c) 2022, PostgreSQL Global Development Group

# Allow use of release 15+ Perl package name in older branches, by giving that
# package the same symbol table as the older package.

package PostgreSQL::Test::Utils;

use strict;
use warnings;

use TestLib;
BEGIN { *PostgreSQL::Test::Utils:: = \*TestLib::; }

use Exporter 'import';

1;
