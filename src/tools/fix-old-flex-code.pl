#!/usr/bin/perl -w
#----------------------------------------------------------------------
#
# fix-old-flex-code.pl
#
# flex versions before 2.5.36, with certain option combinations, produce
# code that causes an "unused variable" warning.  That's annoying, so
# let's suppress it by inserting a dummy reference to the variable.
# (That's exactly what 2.5.36 and later do ...)
#
# Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/tools/fix-old-flex-code.pl
#
#----------------------------------------------------------------------

use strict;
use warnings;

# Get command line argument.
usage() if $#ARGV != 0;
my $filename = shift;

# Suck in the whole file.
local $/ = undef;
my $cfile;
open($cfile, '<', $filename) || die "opening $filename for reading: $!";
my $ccode = <$cfile>;
close($cfile);

# No need to do anything if it's not flex 2.5.x for x < 36.
exit 0 if $ccode !~ m/^#define YY_FLEX_MAJOR_VERSION 2$/m;
exit 0 if $ccode !~ m/^#define YY_FLEX_MINOR_VERSION 5$/m;
exit 0 if $ccode !~ m/^#define YY_FLEX_SUBMINOR_VERSION (\d+)$/m;
exit 0 if $1 >= 36;

# Apply the desired patch.
$ccode =~
s|(struct yyguts_t \* yyg = \(struct yyguts_t\*\)yyscanner; /\* This var may be unused depending upon options. \*/
.*?)
	return yy_is_jam \? 0 : yy_current_state;
|$1
	(void) yyg;
	return yy_is_jam ? 0 : yy_current_state;
|s;

# Write the modified file back out.
open($cfile, '>', $filename) || die "opening $filename for writing: $!";
print $cfile $ccode;
close($cfile);

exit 0;


sub usage
{
	die <<EOM;
Usage: fix-old-flex-code.pl c-file-name

fix-old-flex-code.pl modifies a flex output file to suppress
an unused-variable warning that occurs with older flex versions.

Report bugs to <pgsql-bugs\@postgresql.org>.
EOM
}
