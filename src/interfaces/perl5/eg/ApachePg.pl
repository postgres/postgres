#!/usr/local/bin/perl

# demo script, tested with:
#  - PostgreSQL-6.2
#  - apache_1.2.4
#  - mod_perl-1.00
#  - perl5.004_03

use CGI;
use Pg;

$query = new CGI;

print  $query->header,
       $query->start_html(-title=>'A Simple Example'),
       $query->startform,
       "<CENTER><H3>Testing Module Pg</H3></CENTER>",
       "Enter database name: ",
       $query->textfield(-name=>'dbname'),
       "<P>",
       "Enter select command: ",
       $query->textfield(-name=>'cmd', -size=>40),
       "<P>",
       $query->submit(-value=>'Submit'),
       $query->endform;

if ($query->param) {

    $dbname = $query->param('dbname');
    $conn = Pg::connectdb("dbname = $dbname");
    $cmd = $query->param('cmd');
    $result = $conn->exec($cmd);
    print "<TABLE>";
    for ($i = 0; $i < $result->ntuples; $i++) {
        print "<TR>";
        for ($j = 0; $j < $result->nfields; $j++) {
            print "<TD>", $result->getvalue($i, $j), "</TD>";
        }
        print "</TR>";
    }
    print "</TABLE>";
}

print $query->end_html;

