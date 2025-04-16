#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

my $node = PGTDE->pgtde_init_pg();
my $pgdata = $node->data_dir;

{
package MyWebServer;
 
use HTTP::Server::Simple::CGI;
use base qw(HTTP::Server::Simple::CGI);
 
my %dispatch = (
    '/hello' => \&resp_hello,
);
 
sub handle_request {
    my $self = shift;
    my $cgi  = shift;
   
    my $path = $cgi->path_info();
    my $handler = $dispatch{$path};
 
    if (ref($handler) eq "CODE") {
        print "HTTP/1.0 200 OK\r\n";
        $handler->($cgi);
         
    } else {
        print "HTTP/1.0 404 Not found\r\n";
        print $cgi->header,
              $cgi->start_html('Not found'),
              $cgi->h1('Not found'),
              $cgi->end_html;
    }
}

sub resp_hello {
    my $cgi  = shift;
    print $cgi->header,
		  "/tmp/http_datafile\r\n";
}
 
}

my $pid = MyWebServer->new(8888)->background();

open my $conf, '>>', "$pgdata/postgresql.conf";
print $conf "shared_preload_libraries = 'pg_tde'\n";
close $conf;

my $rt_value = $node->start();
ok($rt_value == 1, "Start Server");

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');

PGTDE::psql($node, 'postgres', "SELECT pg_tde_add_database_key_provider_file('file-provider', json_object( 'type' VALUE 'remote', 'url' VALUE 'http://localhost:8888/hello' ));");
PGTDE::psql($node, 'postgres', "SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-provider');");

PGTDE::psql($node, 'postgres', 'CREATE TABLE test_enc2(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc2 (k) VALUES (5),(6);');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;');

PGTDE::append_to_file("-- server restart");
$node->stop();
$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;');

PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc2;');

PGTDE::psql($node, 'postgres', 'DROP EXTENSION pg_tde;');

$node->stop();

system("kill $pid");

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

done_testing();
