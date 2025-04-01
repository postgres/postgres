# Copyright (c) 2024-2025, PostgreSQL Global Development Group

=pod

=head1 NAME

PostgreSQL::Test::AdjustDump - helper module for dump/restore tests

=head1 SYNOPSIS

  use PostgreSQL::Test::AdjustDump;

  # Adjust contents of dump output file so that dump output from original
  # regression database and that from the restored regression database match
  $dump = adjust_regress_dumpfile($dump, $adjust_child_columns);

=head1 DESCRIPTION

C<PostgreSQL::Test::AdjustDump> encapsulates various hacks needed to
compare the results of dump/restore tests.

=cut

package PostgreSQL::Test::AdjustDump;

use strict;
use warnings FATAL => 'all';

use Exporter 'import';
use Test::More;

our @EXPORT = qw(
  adjust_regress_dumpfile
);

=pod

=head1 ROUTINES

=over

=item $dump = adjust_regress_dumpfile($dump, $adjust_child_columns)

Edit a dump output file, taken from the source regression database,
to remove the known differences to a dump taken after restoring the
same database.

Arguments:

=over

=item C<dump>: Contents of dump file

=item C<adjust_child_columns>: 1 indicates that the given dump file requires
adjusting columns in the child tables; usually when the dump is from original
database. 0 indicates no such adjustment is needed; usually when the dump is
from restored database.

=back

Returns the adjusted dump text.

Adjustments Applied:

=over

=item Column reordering on child table creation

This rearranges the column declarations in the C<CREATE TABLE... INHERITS>
statements in the dump file from original database so that they match those
from the restored database.

Only executed if C<adjust_child_columns> is true.

Context: some regression tests purposefully create child tables in such a way
that the order of their inherited columns differ from column orders of their
respective parents.  In the restored database, however, the order of their
inherited columns are same as that of their respective parents. Thus the column
orders of these child tables in the original database and those in the restored
database differ, causing difference in the dump outputs. See
C<MergeAttributes()> and C<dumpTableSchema()> for details.

=item Removal of problematic C<COPY> statements

Remove COPY statements to abnormal children tables.

Context: This difference is caused because of columns that are added to parent
tables that already have children; because recreating the children tables puts
the columns from the parent ahead of columns declared locally in children,
these extra columns are in earlier position compared to the original database.
Reordering columns on the entire C<COPY> data is impractical, so we just remove
them.

=item Newline adjustment

Windows-style newlines are changed to Unix-style.  Empty lines are trimmed.

=back

=cut

sub adjust_regress_dumpfile
{
	my ($dump, $adjust_child_columns) = @_;

	# use Unix newlines
	$dump =~ s/\r\n/\n/g;

	# Adjust the CREATE TABLE ... INHERITS statements.
	if ($adjust_child_columns)
	{
		$dump =~ s/(^CREATE\sTABLE\sgenerated_stored_tests\.gtestxx_4\s\()
		(\n\s+b\sinteger),
		(\n\s+a\sinteger\sNOT\sNULL)/$1$3,$2/mgx;

		$dump =~ s/(^CREATE\sTABLE\sgenerated_virtual_tests\.gtestxx_4\s\()
		(\n\s+b\sinteger),
		(\n\s+a\sinteger\sNOT\sNULL)/$1$3,$2/mgx;

		$dump =~ s/(^CREATE\sTABLE\spublic\.test_type_diff2_c1\s\()
		(\n\s+int_four\sbigint),
		(\n\s+int_eight\sbigint),
		(\n\s+int_two\ssmallint)/$1$4,$2,$3/mgx;

		$dump =~ s/(^CREATE\sTABLE\spublic\.test_type_diff2_c2\s\()
		(\n\s+int_eight\sbigint),
		(\n\s+int_two\ssmallint),
		(\n\s+int_four\sbigint)/$1$3,$4,$2/mgx;
	}

	# Remove COPY statements with differing column order
	for my $table (
		'public\.b_star', 'public\.c_star',
		'public\.cc2', 'public\.d_star',
		'public\.e_star', 'public\.f_star',
		'public\.renamecolumnanother', 'public\.renamecolumnchild',
		'public\.test_type_diff2_c1', 'public\.test_type_diff2_c2',
		'public\.test_type_diff_c')
	{
		# This multiline pattern matches the whole COPY, up to the
		# terminating "\."
		$dump =~ s/^COPY $table \(.+?^\\\.$//sm;
	}

	# Suppress blank lines, as some places in pg_dump emit more or fewer.
	$dump =~ s/\n\n+/\n/g;

	return $dump;
}

=pod

=back

=cut

1;
