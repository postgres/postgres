#!/bin/perl
#
# zap_ltv - attempt to restore a POSTGRES95 database afflicted with 
#           pg_log, pg_time, or pg_variable corruption to
#           minimal functionality
#
# Paul Walmsley <ccshag@cclabs.missouri.edu>
#
# Legalese:
#
# In no event shall Paul Walmsley be liable to any party for direct,
# indirect, special, incidental, or consequential damages, including
# lost profits, arising from the use of this software, even if Paul Walmsley
# has been advised of the possibility of such damage.  Paul Walmsley
# specifically disclaims any warranties, including, but not limited to,
# the implied warranties of merchantability and fitness for a particular
# purpose.  The software provided hereunder is on an "as is" basis, 
# and Paul Walmsley has no obligations to provide maintenance, support,
# updates, enhancements, or modifications.
#
# Thanks, Berkeley ;-)
 
print "This program should only be run if POSTGRES95 is not currently\n";
print "running on this system.  It should also not be run unless you \n";
print "are having seemingly unrecoverable problems with your POSTGRES95\n";
print "database related to pg_log, pg_time, or pg_variable corruption.\n\n";
print "This program replaces the existing pg_log, pg_time, and pg_variable\n";
print "files with \"clean\" copies of those files.  This will almost \n";
print "certainly result in duplicate IDs when any INSERTs are attempted,\n";
print "and probably has other side-effects as well.  Back up your databases\n"
print "and re-initdb from scratch after using this!\n\n";
print "This program will attempt to make a backup of your pg_time,\n";
print "pg_log, and pg_variable files (to pg_time.backup, pg_log.backup,\n";
print "and pg_variable.backup, respectively).\n\n";
print "This program bears no guarantees nor any warranties whatsoever.\n";
print "View the source for details.\n\n";
print "Press ENTER to zap your pg_log, pg_time, and pg_variable files:";

$trash=<STDIN>;

$pg_log_data=pack('xxxx@8192',0);
$pg_time_data=pack('xxxx@8192',0);
# next_tid, last_tid, and oid are pulled from a fresh initdb 
$pg_variable_data=pack('xxxxxxCCxxCCxxCC@8192',2,34,2,30,81,32);

if (length($pg_log_data)!=8192) { 
	die "pg_log_data must be exactly 8192 bytes long";
}
if (length($pg_time_data)!=8192) { 
        die "pg_time_data must be exactly 8192 bytes long";
}
if (length($pg_variable_data)!=8192) { 
        die "pg_variable_data must be exactly 8192 bytes long";
}


if (! -f 'pg_database') {
	die "This program must be run from your POSTGRES95 data directory.";
}

system('cp pg_log pg_log.backup');
open(PG_LOG,'>pg_log');
binmode(PG_LOG);
$written=syswrite(PG_LOG,$pg_log_data,8192);
close(PG_LOG);

if ($written!=8192) {
	die "pg_log write failed: $!";
}

$written=0;
system('cp pg_time pg_time.backup');
open(PG_TIME,'>pg_time');
binmode(PG_TIME);
$written=syswrite(PG_TIME,$pg_time_data,8192);
close(PG_TIME);

if ($written!=8192) {
        die "pg_time write failed: $!";
}

$written=0;
system('cp pg_variable pg_variable.backup');
open(PG_VARIABLE,'>pg_variable');
binmode(PG_VARIABLE);
$written=syswrite(PG_VARIABLE,$pg_variable_data,8192);
close(PG_VARIABLE);

if ($written!=8192) {
        die "pg_variable write failed: $!";
}

print "Done.\n";
