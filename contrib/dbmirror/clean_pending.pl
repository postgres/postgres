#!/usr/bin/perl
# clean_pending.pl
# This perl script removes entries from the pending,pendingKeys,
# pendingDeleteData tables that have already been mirrored to all hosts.
#
#
#
#    Written by Steven Singer (ssinger@navtechinc.com)
#    (c) 2001-2002 Navtech Systems Support Inc.
#    Released under the GNU Public License version 2. See COPYING.
#
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
##############################################################################
# $Id: clean_pending.pl,v 1.3 2003/05/14 03:25:55 tgl Exp $
##############################################################################



=head1 NAME

clean_pending.pl - A Perl script to remove old entries from the 
pending, pendingKeys, and pendingDeleteData tables.


=head1 SYNPOSIS


clean_pending.pl databasename


=head1 DESCRIPTION


This Perl script connects to the database specified as a command line argument
on the local system.  It uses a hard-coded username and password.
It then removes any entries from the pending, pendingDeleteData, and 
pendingKeys tables that have already been sent to all hosts in mirrorHosts.


=cut

BEGIN {
    # add in a global path to files
    #Ensure that Pg is in the path.
}


use strict;
use Pg;
if ($#ARGV != 0) {
   die "usage: clean_pending.pl configFile\n";
}

if( ! defined do $ARGV[0]) {
    die("Invalid Configuration file $ARGV[0]");
}

#connect to the database.

my $connectString = "host=$::masterHost dbname=$::masterDb user=$::masterUser password=$::masterPassword";

my $dbConn = Pg::connectdb($connectString);
unless($dbConn->status == PGRES_CONNECTION_OK) {
    printf("Can't connect to database\n");
    die;
}
my $result = $dbConn->exec("BEGIN");
unless($result->resultStatus == PGRES_COMMAND_OK) {
   die $dbConn->errorMessage;
}


#delete all transactions that have been sent to all mirrorhosts
#or delete everything if no mirror hosts are defined.
# Postgres takes the "SELECT COUNT(*) FROM "MirrorHost"  and makes it into
# an InitPlan.  EXPLAIN show's this.  
my $deletePendingQuery = 'DELETE FROM "Pending" WHERE (SELECT ';
$deletePendingQuery .= ' COUNT(*) FROM "MirroredTransaction" WHERE ';
$deletePendingQuery .= ' "XID"="Pending"."XID") = (SELECT COUNT(*) FROM ';
$deletePendingQuery .= ' "MirrorHost") OR (SELECT COUNT(*) FROM ';
$deletePendingQuery .= ' "MirrorHost") = 0';

my $result = $dbConn->exec($deletePendingQuery);
unless ($result->resultStatus == PGRES_COMMAND_OK ) {
    printf($dbConn->errorMessage);
    die;
}
$dbConn->exec("COMMIT");
$result = $dbConn->exec('VACUUM "Pending"');
unless ($result->resultStatus == PGRES_COMMAND_OK) {
   printf($dbConn->errorMessage);
}
$result = $dbConn->exec('VACUUM "PendingData"');
unless($result->resultStatus == PGRES_COMMAND_OK) {
   printf($dbConn->errorMessage);
}
$result = $dbConn->exec('VACUUM "MirroredTransaction"');
unless($result->resultStatus == PGRES_COMMAND_OK) {
  printf($dbConn->errorMessage);
}

