package PGTDE;

use File::Basename;
use File::Compare;
use Test::More;

our @ISA= qw( Exporter );

# These CAN be exported.
our @EXPORT = qw( pgtde_init_pg pgtde_start_pg pgtde_stop_pg pgtde_psql_cmd pgtde_setup_pg_tde pgtde_create_extension pgtde_drop_extension );

# Instance of pg server that would be spanwed by TAP testing. A new server will be created for each TAP test.
our $pg_node;

# Expected .out filename of TAP testcase being executed. These are already part of repo under t/expected/*.
our $expected_filename_with_path;

# Major version of PG Server that we are using.
our $PG_MAJOR_VERSION;

# Result .out filename of TAP testcase being executed. Where needed, a new *.out will be created for each TAP test.
our $out_filename_with_path;

# Runtime output file that is used only for debugging purposes for comparison to PGSS, blocks and timings.
our $debug_out_filename_with_path;

BEGIN {
    # Get PG Server Major version from pg_config 
    $PG_MAJOR_VERSION = `pg_config --version | awk {'print \$2'} | cut -f1 -d"." | sed -e 's/[^0-9].*\$//g'`;
    $PG_MAJOR_VERSION =~ s/^\s+|\s+$//g;

    # Depending upon PG server version load the required module at runtime when pgtde.pm is loaded.
    my $node_module = $PG_MAJOR_VERSION > 14 ? "PostgreSQL::Test::Cluster" : "PostgresNode";
    my $node_module_file = $node_module;
    $node_module_file =~ s[::][/]g;
    $node_module_file .= '.pm';
    require $node_module_file;
    $node_module->import;
}

sub pgtde_init_pg
{
    print "Postgres major version: $PG_MAJOR_VERSION \n";

    # For Server version 15 & above, spawn the server using PostgreSQL::Test::Cluster
    if ($PG_MAJOR_VERSION > 14) {
        $pg_node = PostgreSQL::Test::Cluster->new('pgtde_regression');
    }
    # For Server version 14 & below, spawn the server using PostgresNode
    elsif ($PG_MAJOR_VERSION < 15) {
        $pg_node = PostgresNode->get_new_node('pgtde_regression');
    }

    $pg_node->dump_info;
    $pg_node->init;
    return $pg_node;
}

sub append_to_file
{
    my ($str) = @_;

    # For Server version 15 & above, use PostgreSQL::Test::Utils to write to files
    if ($PG_MAJOR_VERSION > 14) {
        PostgreSQL::Test::Utils::append_to_file($out_filename_with_path, $str . "\n");
    }
    # For Server version 14 & below, use PostgresNode to write to files
    elsif ($PG_MAJOR_VERSION < 15) {
        TestLib::append_to_file($out_filename_with_path, $str . "\n");
    }
    chmod(0640 , $out_filename_with_path)
    or die("unable to set permissions for $out_filename_with_path");

    return;
}

sub append_to_debug_file
{
    my ($str) = @_;

    # For Server version 15 & above, use PostgreSQL::Test::Utils to write to files
    if ($PG_MAJOR_VERSION > 14) {
        PostgreSQL::Test::Utils::append_to_file($debug_out_filename_with_path, $str . "\n");
    }
    # For Server version 14 & below, use PostgresNode to write to files
    elsif ($PG_MAJOR_VERSION < 15) {
        TestLib::append_to_file($debug_out_filename_with_path, $str . "\n");
    }
    chmod(0640 , $debug_out_filename_with_path)
    or die("unable to set permissions for $debug_out_filename_with_path");

    return;
}

sub setup_files_dir
{
    my ($perlfilename) = @_;

    # Expected folder where expected output will be present
    my $expected_folder = "t/expected";

    # Results/out folder where generated results files will be placed
    my $results_folder = "t/results";

    # Check if results folder exists or not, create if it doesn't
    unless (-d $results_folder)
    {
        mkdir $results_folder or die "Can't create folder $results_folder: $!\n";
    }

    # Check if expected folder exists or not, bail out if it doesn't
    unless (-d $expected_folder)
    {
        BAIL_OUT "Expected files folder $expected_folder doesn't exist: \n";
    }

    #Remove .pl from filename and store in a variable
    my @split_arr = split /\./, $perlfilename;

    my $filename_without_extension = $split_arr[0];

    # Create expected filename with path
    my $expected_filename = "${filename_without_extension}.out";

    $expected_filename_with_path = "${expected_folder}/${expected_filename}";

    # Create results filename with path
    my $out_filename = "${filename_without_extension}.out";
    $out_filename_with_path = "${results_folder}/${out_filename}";

    # Delete already existing result out file, if it exists.
    if ( -f $out_filename_with_path)
    {
        unlink($out_filename_with_path) or die "Can't delete already existing $out_filename_with_path: $!\n";
    }

    $debug_out_filename_with_path = "${results_folder}/${out_filename}.debug";
}

sub compare_results
{
    # Compare expected and results files and return the result
    return compare($expected_filename_with_path, $out_filename_with_path);
}

1;
