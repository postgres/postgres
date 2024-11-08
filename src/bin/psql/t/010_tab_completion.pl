
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Data::Dumper;

# Do nothing unless Makefile has told us that the build is --with-readline.
if (!defined($ENV{with_readline}) || $ENV{with_readline} ne 'yes')
{
	plan skip_all => 'readline is not supported by this build';
}

# Also, skip if user has set environment variable to command that.
# This is mainly intended to allow working around some of the more broken
# versions of libedit --- some users might find them acceptable even if
# they won't pass these tests.
if (defined($ENV{SKIP_READLINE_TESTS}))
{
	plan skip_all => 'SKIP_READLINE_TESTS is set';
}

# If we don't have IO::Pty, forget it, because IPC::Run depends on that
# to support pty connections
eval { require IO::Pty; };
if ($@)
{
	plan skip_all => 'IO::Pty is needed to run this test';
}

# start a new server
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# set up a few database objects
$node->safe_psql('postgres',
	"CREATE TABLE tab1 (c1 int primary key constraint foo not null, c2 text);\n"
	  . "CREATE TABLE mytab123 (f1 int, f2 text);\n"
	  . "CREATE TABLE mytab246 (f1 int, f2 text);\n"
	  . "CREATE TABLE \"mixedName\" (f1 int, f2 text);\n"
	  . "CREATE TYPE enum1 AS ENUM ('foo', 'bar', 'baz', 'BLACK');\n"
	  . "CREATE PUBLICATION some_publication;\n");

# In a VPATH build, we'll be started in the source directory, but we want
# to run in the build directory so that we can use relative paths to
# access the tab_comp_dir subdirectory; otherwise the output from filename
# completion tests is too variable.
if ($ENV{TESTDATADIR})
{
	chdir $ENV{TESTDATADIR}
	  or die "could not chdir to \"$ENV{TESTDATADIR}\": $!";
}

# Create some junk files for filename completion testing.
mkdir "tab_comp_dir";
my $FH;
open $FH, ">", "tab_comp_dir/somefile"
  or die("could not create file \"tab_comp_dir/somefile\": $!");
print $FH "some stuff\n";
close $FH;
open $FH, ">", "tab_comp_dir/afile123"
  or die("could not create file \"tab_comp_dir/afile123\": $!");
print $FH "more stuff\n";
close $FH;
open $FH, ">", "tab_comp_dir/afile456"
  or die("could not create file \"tab_comp_dir/afile456\": $!");
print $FH "other stuff\n";
close $FH;

# Arrange to capture, not discard, the interactive session's history output.
# Put it in the test log directory, so that buildfarm runs capture the result
# for possible debugging purposes.
my $historyfile = "${PostgreSQL::Test::Utils::log_path}/010_psql_history.txt";

# fire up an interactive psql session
my $h = $node->interactive_psql('postgres', history_file => $historyfile);

# Simple test case: type something and see if psql responds as expected
sub check_completion
{
	my ($send, $pattern, $annotation) = @_;

	# report test failures from caller location
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	# restart per-command timer
	$h->{timeout}->start($PostgreSQL::Test::Utils::timeout_default);

	# send the data to be sent and wait for its result
	my $out = $h->query_until($pattern, $send);
	my $okay = ($out =~ $pattern && !$h->{timeout}->is_expired);
	ok($okay, $annotation);
	# for debugging, log actual output if it didn't match
	local $Data::Dumper::Terse = 1;
	local $Data::Dumper::Useqq = 1;
	diag 'Actual output was ' . Dumper($out) . "Did not match \"$pattern\"\n"
	  if !$okay;
	return;
}

# Clear query buffer to start over
# (won't work if we are inside a string literal!)
sub clear_query
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	check_completion("\\r\n", qr/Query buffer reset.*postgres=# /s,
		"\\r works");
	return;
}

# Clear current line to start over
# (this will work in an incomplete string literal, but it's less desirable
# than clear_query because we lose evidence in the history file)
sub clear_line
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	check_completion("\025\n", qr/postgres=# /, "control-U works");
	return;
}

# check basic command completion: SEL<tab> produces SELECT<space>
check_completion("SEL\t", qr/SELECT /, "complete SEL<tab> to SELECT");

clear_query();

# check case variation is honored
check_completion("sel\t", qr/select /, "complete sel<tab> to select");

# check basic table name completion
check_completion("* from t\t", qr/\* from tab1 /, "complete t<tab> to tab1");

clear_query();

# check table name completion with multiple alternatives
# note: readline might print a bell before the completion
check_completion(
	"select * from my\t",
	qr/select \* from my\a?tab/,
	"complete my<tab> to mytab when there are multiple choices");

# some versions of readline/libedit require two tabs here, some only need one
check_completion(
	"\t\t",
	qr/mytab123 +mytab246/,
	"offer multiple table choices");

check_completion("2\t", qr/246 /,
	"finish completion of one of multiple table choices");

clear_query();

# check handling of quoted names
check_completion(
	"select * from \"my\t",
	qr/select \* from "my\a?tab/,
	"complete \"my<tab> to \"mytab when there are multiple choices");

check_completion(
	"\t\t",
	qr/"mytab123" +"mytab246"/,
	"offer multiple quoted table choices");

check_completion("2\t", qr/246" /,
	"finish completion of one of multiple quoted table choices");

clear_query();

# check handling of mixed-case names
check_completion(
	"select * from \"mi\t",
	qr/"mixedName" /,
	"complete a mixed-case name");

clear_query();

# check case folding
check_completion("select * from TAB\t", qr/tab1 /, "automatically fold case");

clear_query();

# check case-sensitive keyword replacement
# note: various versions of readline/libedit handle backspacing
# differently, so just check that the replacement comes out correctly
check_completion("\\DRD\t", qr/drds /, "complete \\DRD<tab> to \\drds");

clear_query();

# check completion of a schema-qualified name
check_completion("select * from pub\t",
	qr/public\./, "complete schema when relevant");

check_completion("tab\t", qr/tab1 /, "complete schema-qualified name");

clear_query();

check_completion(
	"select * from PUBLIC.t\t",
	qr/public\.tab1 /,
	"automatically fold case in schema-qualified name");

clear_query();

# check interpretation of referenced names
check_completion(
	"alter table tab1 drop constraint t\t",
	qr/tab1_pkey /,
	"complete index name for referenced table");

clear_query();

check_completion(
	"alter table TAB1 drop constraint t\t",
	qr/tab1_pkey /,
	"complete index name for referenced table, with downcasing");

clear_query();

check_completion(
	"alter table public.\"tab1\" drop constraint t\t",
	qr/tab1_pkey /,
	"complete index name for referenced table, with schema and quoting");

clear_query();

# check variant where we're completing a qualified name from a refname
# (this one also checks successful completion in a multiline command)
check_completion(
	"comment on constraint tab1_pkey \n on public.\t",
	qr/public\.tab1/,
	"complete qualified name from object reference");

clear_query();

# check filename completion
check_completion(
	"\\lo_import tab_comp_dir/some\t",
	qr|tab_comp_dir/somefile |,
	"filename completion with one possibility");

clear_query();

# note: readline might print a bell before the completion
check_completion(
	"\\lo_import tab_comp_dir/af\t",
	qr|tab_comp_dir/af\a?ile|,
	"filename completion with multiple possibilities");

# here we are inside a string literal 'afile*', so must use clear_line().
clear_line();

# COPY requires quoting
check_completion(
	"COPY foo FROM tab_comp_dir/some\t",
	qr|'tab_comp_dir/somefile' |,
	"quoted filename completion with one possibility");

clear_query();

check_completion(
	"COPY foo FROM tab_comp_dir/af\t",
	qr|'tab_comp_dir/afile|,
	"quoted filename completion with multiple possibilities");

# some versions of readline/libedit require two tabs here, some only need one
# also, some will offer the whole path name and some just the file name
# the quotes might appear, too
check_completion(
	"\t\t",
	qr|afile123'? +'?(tab_comp_dir/)?afile456|,
	"offer multiple file choices");

clear_line();

# check enum label completion
# some versions of readline/libedit require two tabs here, some only need one
# also, some versions will offer quotes, some will not
check_completion(
	"ALTER TYPE enum1 RENAME VALUE 'ba\t\t",
	qr|'?bar'? +'?baz'?|,
	"offer multiple enum choices");

clear_line();

# enum labels are case sensitive, so this should complete BLACK immediately
check_completion(
	"ALTER TYPE enum1 RENAME VALUE 'B\t",
	qr|BLACK|,
	"enum labels are case sensitive");

clear_line();

# check timezone name completion
check_completion("SET timezone TO am\t",
	qr|'America/|, "offer partial timezone name");

check_completion("new_\t", qr|New_York|, "complete partial timezone name");

clear_line();

# check completion of a keyword offered in addition to object names;
# such a keyword should obey COMP_KEYWORD_CASE
foreach (
	[ 'lower', 'CO', 'column' ],
	[ 'upper', 'co', 'COLUMN' ],
	[ 'preserve-lower', 'co', 'column' ],
	[ 'preserve-upper', 'CO', 'COLUMN' ],)
{
	my ($case, $in, $out) = @$_;

	check_completion(
		"\\set COMP_KEYWORD_CASE $case\n",
		qr/postgres=#/,
		"set completion case to '$case'");
	check_completion("alter table tab1 rename $in\t\t\t",
		qr|$out|,
		"offer keyword $out for input $in<TAB>, COMP_KEYWORD_CASE = $case");
	clear_query();
}

# alternate path where keyword comes from SchemaQuery
check_completion(
	"DROP TYPE big\t",
	qr/DROP TYPE bigint /,
	"offer keyword from SchemaQuery");

clear_query();

# check create_command_generator
check_completion(
	"CREATE TY\t",
	qr/CREATE TYPE /,
	"check create_command_generator");

clear_query();

# check words_after_create infrastructure
check_completion(
	"CREATE TABLE mytab\t\t",
	qr/mytab123 +mytab246/,
	"check words_after_create");

clear_query();

# check VersionedQuery infrastructure
check_completion(
	"DROP PUBLIC\t \t\t",
	qr/DROP PUBLICATION\s+some_publication /,
	"check VersionedQuery");

clear_query();

# hits ends_with() and logic for completing in multi-line queries
check_completion("analyze (\n\t\t", qr/VERBOSE/,
	"check ANALYZE (VERBOSE ...");

clear_query();

# check completions for GUCs
check_completion(
	"set interval\t\t",
	qr/intervalstyle TO/,
	"complete a GUC name");
check_completion(" iso\t", qr/iso_8601 /, "complete a GUC enum value");

clear_query();

# same, for qualified GUC names
check_completion(
	"DO \$\$begin end\$\$ LANGUAGE plpgsql;\n",
	qr/postgres=# /,
	"load plpgsql extension");

check_completion("set plpg\t", qr/plpg\a?sql\./,
	"complete prefix of a GUC name");
check_completion(
	"var\t\t",
	qr/variable_conflict TO/,
	"complete a qualified GUC name");
check_completion(" USE_C\t",
	qr/use_column/, "complete a qualified GUC enum value");

clear_query();

# check completions for psql variables
check_completion("\\set VERB\t", qr/VERBOSITY /,
	"complete a psql variable name");
check_completion("def\t", qr/default /, "complete a psql variable value");

clear_query();

check_completion(
	"\\echo :VERB\t",
	qr/:VERBOSITY /,
	"complete an interpolated psql variable name");

clear_query();

# check completion for psql variable test
check_completion(
	"\\echo :{?VERB\t",
	qr/:\{\?VERBOSITY} /,
	"complete a psql variable test");

clear_query();

# check no-completions code path
check_completion("blarg \t\t", qr//, "check completion failure path");

clear_query();

# check COPY FROM with DEFAULT option
check_completion(
	"COPY foo FROM stdin WITH ( DEF\t)",
	qr/DEFAULT /,
	"COPY FROM with DEFAULT completion");

clear_line();

# send psql an explicit \q to shut it down, else pty won't close properly
$h->quit or die "psql returned $?";

# done
$node->stop;
done_testing();
