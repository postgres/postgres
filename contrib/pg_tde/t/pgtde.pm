package PGTDE;

use File::Basename;
use File::Compare;
use Test::More;

# Expected .out filename of TAP testcase being executed. These are already part of repo under t/expected/*.
our $expected_filename_with_path;

# Major version of PG Server that we are using.
our $PG_MAJOR_VERSION;

# Result .out filename of TAP testcase being executed. Where needed, a new *.out will be created for each TAP test.
our $out_filename_with_path;

# Runtime output file that is used only for debugging purposes for comparison to PGSS, blocks and timings.
our $debug_out_filename_with_path;

BEGIN {
    $PG_MAJOR_VERSION = `pg_config --version | awk {'print \$2'} | cut -f1 -d"." | sed -e 's/[^0-9].*\$//g'`;
    $PG_MAJOR_VERSION =~ s/^\s+|\s+$//g;

    if ($PG_MAJOR_VERSION >= 15) {
        eval {
            require PostgreSQL::Test::Cluster;
            import PostgreSQL::Test::Utils;
        }
    } else {
        eval {
            require PostgresNode;
            import TestLib;
        }
    }
}

sub pgtde_init_pg
{
    my $node;

    print "Postgres major version: $PG_MAJOR_VERSION\n";

    # For Server version 15 & above, spawn the server using PostgreSQL::Test::Cluster
    if ($PG_MAJOR_VERSION >= 15) {
        $node = PostgreSQL::Test::Cluster->new('pgtde_regression');
    } else {
        $node = PostgresNode->get_new_node('pgtde_regression');
    }

    $node->dump_info;
    $node->init;
    return $node;
}

sub psql
{
    my ($node, $dbname, $sql) = @_;

    my (undef, $stdout, $stderr) = $node->psql($dbname, $sql, extra_params => ['-a', '-Pformat=aligned', '-Ptuples_only=off']);

    if ($stdout ne '') {
        append_to_result_file($stdout);
    }

    if ($stderr ne '') {
        append_to_result_file($stderr);
    }
}

sub append_to_result_file
{
    my ($str) = @_;

    append_to_file($out_filename_with_path, $str . "\n");
}

sub append_to_debug_file
{
    my ($str) = @_;

    append_to_file($debug_out_filename_with_path, $str . "\n");
}

sub setup_files_dir
{
    my ($perlfilename) = @_;

    my $expected_folder = "t/expected";
    my $results_folder = "t/results";

    unless (-d $results_folder)
    {
        mkdir $results_folder or die "Can't create folder $results_folder: $!\n";
    }

    unless (-d $expected_folder)
    {
        BAIL_OUT "Expected files folder $expected_folder doesn't exist: \n";
    }

    my @split_arr = split /\./, $perlfilename;

    my $filename_without_extension = $split_arr[0];

    my $expected_filename = "${filename_without_extension}.out";
    $expected_filename_with_path = "${expected_folder}/${expected_filename}";

    my $out_filename = "${filename_without_extension}.out";
    $out_filename_with_path = "${results_folder}/${out_filename}";

    if (-f $out_filename_with_path)
    {
        unlink($out_filename_with_path) or die "Can't delete already existing $out_filename_with_path: $!\n";
    }

    $debug_out_filename_with_path = "${results_folder}/${out_filename}.debug";
}

sub compare_results
{
    return compare($expected_filename_with_path, $out_filename_with_path);
}

1;
