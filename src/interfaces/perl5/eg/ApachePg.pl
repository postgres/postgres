#!/usr/local/bin/perl

# demo script, tested with:
#  - PostgreSQL-6.3
#  - apache_1.3
#  - mod_perl-1.08
#  - perl5.004_04

use CGI;
use Pg;
use strict;

my $query = new CGI;

print  $query->header,
       $query->start_html(-title=>'A Simple Example'),
       $query->startform,
       "<CENTER><H3>Testing Module Pg</H3></CENTER>",
       "<P><CENTER><TABLE CELLPADDING=4 CELLSPACING=2 BORDER=1>",
       "<TR><TD>Enter conninfo string: </TD>",
           "<TD>", $query->textfield(-name=>'conninfo', -size=>40, -default=>'dbname=template1 host=localhost'), "</TD>",
       "</TR>",
       "<TR><TD>Enter select command: </TD>",
           "<TD>", $query->textfield(-name=>'cmd', -size=>40), "</TD>",
       "</TR>",
       "</TABLE></CENTER><P>",
       "<CENTER>", $query->submit(-value=>'Submit'), "</CENTER>",
       $query->endform;

if ($query->param) {

    my $conninfo = $query->param('conninfo');
    my $conn = Pg::connectdb($conninfo);
    if ($conn->status == PGRES_CONNECTION_OK) {
        my $cmd = $query->param('cmd');
        my $result = $conn->exec($cmd);
        print "<P><CENTER><TABLE CELLPADDING=4 CELLSPACING=2 BORDER=1>\n";
        my @row;
        while (@row = $result->fetchrow) {
            print "<TR><TD>", join("</TD><TD>", @row), "</TD></TR>\n";
        }
        print "</TABLE></CENTER><P>\n";
    } else {
        print "<CENTER><H2>Connect to database failed</H2></CENTER>\n";
    }
}

print $query->end_html;

