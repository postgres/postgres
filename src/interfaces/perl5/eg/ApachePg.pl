#!/usr/local/bin/perl

# demo script, tested with:
#  - PostgreSQL-6.2
#  - apache_1.2.4
#  - mod_perl-1.00
#  - perl5.004_01

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
    $result->print(STDOUT, 0, 0, 0, 1, 0, 0, '', '', '');
}

print $query->end_html;

