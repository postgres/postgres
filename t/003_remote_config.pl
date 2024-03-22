#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use File::Compare;
use File::Copy;
use Test::More;
use lib 't';
use pgtde;

# Get file name and CREATE out file name and dirs WHERE requried
PGTDE::setup_files_dir(basename($0));

# CREATE new PostgreSQL node and do initdb
my $node = PGTDE->pgtde_init_pg();
my $pgdata = $node->data_dir;

{
package MyWebServer;
 
use HTTP::Server::Simple::CGI;
use base qw(HTTP::Server::Simple::CGI);
 
my %dispatch = (
    '/hello' => \&resp_hello,
    # ...
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


# UPDATE postgresql.conf to include/load pg_tde library
open my $conf, '>>', "$pgdata/postgresql.conf";
print $conf "shared_preload_libraries = 'pg_tde'\n";
close $conf;

my $rt_value = $node->stop();
$rt_value = $node->start();
ok($rt_value == 1, "Restart Server");

my ($cmdret, $stdout, $stderr) = $node->psql('postgres', 'CREATE EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "CREATE PGTDE EXTENSION");
PGTDE::append_to_file($stdout);

$rt_value = $node->psql('postgres', "SELECT pg_tde_add_key_provider_file('file-provider', json_object( 'type' VALUE 'remote', 'url' VALUE 'http://localhost:8888/hello' ));", extra_params => ['-a']);
$rt_value = $node->psql('postgres', "SELECT pg_tde_set_master_key('test-db-master-key','file-provider');", extra_params => ['-a']);

$stdout = $node->safe_psql('postgres', 'CREATE TABLE test_enc2(id SERIAL,k INTEGER,PRIMARY KEY (id)) USING pg_tde;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'INSERT INTO test_enc2 (k) VALUES (5),(6);', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# Restart the server
PGTDE::append_to_file("-- server restart");
$rt_value = $node->stop();
$rt_value = $node->start();

$stdout = $node->safe_psql('postgres', 'SELECT * FROM test_enc2 ORDER BY id ASC;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

$stdout = $node->safe_psql('postgres', 'DROP TABLE test_enc2;', extra_params => ['-a']);
PGTDE::append_to_file($stdout);

# DROP EXTENSION
$stdout = $node->safe_psql('postgres', 'DROP EXTENSION pg_tde;', extra_params => ['-a']);
ok($cmdret == 0, "DROP PGTDE EXTENSION");
PGTDE::append_to_file($stdout);
# Stop the server
$node->stop();

system("kill $pid");

# compare the expected and out file
my $compare = PGTDE->compare_results();

# Test/check if expected and result/out file match. If Yes, test passes.
is($compare,0,"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files.");

# Done testing for this testcase file.
done_testing();