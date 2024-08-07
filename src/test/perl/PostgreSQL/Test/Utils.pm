# Copyright (c) 2022, PostgreSQL Global Development Group

# Allow use of release 15+ Perl package name in older branches, by giving that
# package the same symbol table as the older package.

use strict;
use warnings;
BEGIN { *PostgreSQL::Test::Utils:: = \*TestLib::; }
use TestLib ();

# There's no runtime requirement for the following package declaration, but it
# convinces the RPM Package Manager that this file provides the Perl package
# in question.  Perl v5.10.1 segfaults if a declaration of the to-be-aliased
# package precedes the aliasing itself, hence the abnormal placement.
package PostgreSQL::Test::Utils;

1;
