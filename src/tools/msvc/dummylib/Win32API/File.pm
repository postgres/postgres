
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

package Win32API::File;

use strict;
use warnings;

use constant { SEM_FAILCRITICALERRORS => 1, SEM_NOGPFAULTERRORBOX => 2 };
sub SetErrormode { }
use Exporter;
our (@ISA, @EXPORT_OK, %EXPORT_TAGS);
@ISA         = qw(Exporter);
@EXPORT_OK   = qw(SetErrorMode SEM_FAILCRITICALERRORS SEM_NOGPFAULTERRORBOX);
%EXPORT_TAGS = (SEM_ => [qw(SEM_FAILCRITICALERRORS SEM_NOGPFAULTERRORBOX)]);

1;
