#!/usr/bin/perl

use strict;
use warnings;
use Env;
use File::Basename;
use Test::More;
use lib 't';
use pgtde;

{
package MyWebServer;
 
use HTTP::Server::Simple::CGI;
use base qw(HTTP::Server::Simple::CGI);
 
my %dispatch = (
    '/token' => \&resp_token,
    '/url' => \&resp_url,
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

sub resp_token {
    my $cgi  = shift;
    print $cgi->header,
		  "$ENV{'ROOT_TOKEN'}\r\n";
}

sub resp_url {
    my $cgi  = shift;
    print $cgi->header,
		  "http://127.0.0.1:8200\r\n";
}
 
}

my $pid = MyWebServer->new(8889)->background();

PGTDE::setup_files_dir(basename($0));

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");

my $rt_value = $node->start();
ok($rt_value == 1, "Start Server");

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION IF NOT EXISTS pg_tde;');

PGTDE::psql($node, 'postgres', "SELECT pg_tde_add_database_key_provider_vault_v2('vault-provider', json_object( 'type' VALUE 'remote', 'url' VALUE 'http://localhost:8889/token' ), json_object( 'type' VALUE 'remote', 'url' VALUE 'http://localhost:8889/url' ), to_json('secret'::text), NULL);");
PGTDE::psql($node, 'postgres', "SELECT pg_tde_set_key_using_database_key_provider('test-db-key','vault-provider');");

PGTDE::psql($node, 'postgres', 'CREATE TABLE test_enc2(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING tde_heap;');

PGTDE::psql($node, 'postgres', 'INSERT INTO test_enc2 (k) VALUES (5),(6);');

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;');

PGTDE::append_to_result_file("-- server restart");
$node->stop();
$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

PGTDE::psql($node, 'postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;');

PGTDE::psql($node, 'postgres', 'DROP TABLE test_enc2;');

PGTDE::psql($node, 'postgres', 'DROP EXTENSION pg_tde;');

$node->stop();

system("kill -9 $pid");

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

done_testing();
