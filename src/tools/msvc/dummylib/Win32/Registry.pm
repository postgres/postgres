
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

package Win32::Registry;

use strict;
use warnings;

use vars qw($HKEY_LOCAL_MACHINE);

use Exporter ();
our (@EXPORT, @ISA);
@ISA    = qw(Exporter);
@EXPORT = qw($HKEY_LOCAL_MACHINE);

1;
