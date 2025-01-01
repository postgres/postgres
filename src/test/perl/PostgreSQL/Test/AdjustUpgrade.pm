
# Copyright (c) 2023-2025, PostgreSQL Global Development Group

=pod

=head1 NAME

PostgreSQL::Test::AdjustUpgrade - helper module for cross-version upgrade tests

=head1 SYNOPSIS

  use PostgreSQL::Test::AdjustUpgrade;

  # Build commands to adjust contents of old-version database before dumping
  $statements = adjust_database_contents($old_version, %dbnames);

  # Adjust contents of old pg_dumpall output file to match newer version
  $dump = adjust_old_dumpfile($old_version, $dump);

  # Adjust contents of new pg_dumpall output file to match older version
  $dump = adjust_new_dumpfile($old_version, $dump);

=head1 DESCRIPTION

C<PostgreSQL::Test::AdjustUpgrade> encapsulates various hacks needed to
compare the results of cross-version upgrade tests.

=cut

package PostgreSQL::Test::AdjustUpgrade;

use strict;
use warnings FATAL => 'all';

use Exporter 'import';
use PostgreSQL::Version;

our @EXPORT = qw(
  adjust_database_contents
  adjust_old_dumpfile
  adjust_new_dumpfile
);

=pod

=head1 ROUTINES

=over

=item $statements = adjust_database_contents($old_version, %dbnames)

Generate SQL commands to perform any changes to an old-version installation
that are needed before we can pg_upgrade it into the current PostgreSQL
version.

Typically this involves dropping or adjusting no-longer-supported objects.

Arguments:

=over

=item C<old_version>: Branch we are upgrading from, represented as a
PostgreSQL::Version object.

=item C<dbnames>: Hash of database names present in the old installation.

=back

Returns a reference to a hash, wherein the keys are database names and the
values are arrayrefs to lists of statements to be run in those databases.

=cut

sub adjust_database_contents
{
	my ($old_version, %dbnames) = @_;
	my $result = {};

	die "wrong type for \$old_version\n"
	  unless $old_version->isa("PostgreSQL::Version");

	# The version tests can be sensitive if fixups have been applied in a
	# recent version and pg_upgrade is run with a beta version, or such.
	# Therefore, use a modified version object that only contains the major.
	$old_version = PostgreSQL::Version->new($old_version->major);

	# remove dbs of modules known to cause pg_upgrade to fail
	# anything not builtin and incompatible should clean up its own db
	foreach my $bad_module ('adminpack', 'test_ddl_deparse', 'tsearch2')
	{
		if ($dbnames{"contrib_regression_$bad_module"})
		{
			_add_st($result, 'postgres',
				"drop database contrib_regression_$bad_module");
			delete($dbnames{"contrib_regression_$bad_module"});
		}
		if ($dbnames{"regression_$bad_module"})
		{
			_add_st($result, 'postgres',
				"drop database regression_$bad_module");
			delete($dbnames{"regression_$bad_module"});
		}
	}

	# avoid no-path-to-downgrade-extension-version issues
	if ($dbnames{contrib_regression_test_extensions})
	{
		_add_st(
			$result,
			'contrib_regression_test_extensions',
			'drop extension if exists test_ext_cine',
			'drop extension if exists test_ext7');
	}

	# we removed this test-support function in v17
	if ($old_version >= 15 && $old_version < 17)
	{
		_add_st($result, 'regression',
			'drop function get_columns_length(oid[])');
	}

	# stuff not supported from release 16
	if ($old_version >= 12 && $old_version < 16)
	{
		# Can't upgrade aclitem in user tables from pre 16 to 16+.
		_add_st($result, 'regression',
			'alter table public.tab_core_types drop column aclitem');
		# Can't handle child tables with locally-generated columns.
		_add_st(
			$result, 'regression',
			'drop table public.gtest_normal_child',
			'drop table public.gtest_normal_child2');
	}

	# stuff not supported from release 14
	if ($old_version < 14)
	{
		# postfix operators (some don't exist in very old versions)
		_add_st(
			$result,
			'regression',
			'drop operator #@# (bigint,NONE)',
			'drop operator #%# (bigint,NONE)',
			'drop operator if exists !=- (bigint,NONE)',
			'drop operator if exists #@%# (bigint,NONE)');

		# get rid of dblink's dependencies on regress.so
		my $regrdb =
		  $old_version le '9.4'
		  ? 'contrib_regression'
		  : 'contrib_regression_dblink';

		if ($dbnames{$regrdb})
		{
			_add_st(
				$result, $regrdb,
				'drop function if exists public.putenv(text)',
				'drop function if exists public.wait_pid(integer)');
		}
	}

	# user table OIDs are gone from release 12 on
	if ($old_version < 12)
	{
		my $nooid_stmt = q{
           DO $stmt$
           DECLARE
              rec text;
           BEGIN
              FOR rec in
                 select oid::regclass::text
                 from pg_class
                 where relname !~ '^pg_'
                    and relhasoids
                    and relkind in ('r','m')
                 order by 1
              LOOP
                 execute 'ALTER TABLE ' || rec || ' SET WITHOUT OIDS';
                 RAISE NOTICE 'removing oids from table %', rec;
              END LOOP;
           END; $stmt$;
        };

		foreach my $oiddb ('regression', 'contrib_regression_btree_gist')
		{
			next unless $dbnames{$oiddb};
			_add_st($result, $oiddb, $nooid_stmt);
		}

		# this table had OIDs too, but we'll just drop it
		if ($old_version >= 10 && $dbnames{'contrib_regression_postgres_fdw'})
		{
			_add_st(
				$result,
				'contrib_regression_postgres_fdw',
				'drop foreign table ft_pg_type');
		}
	}

	# abstime+friends are gone from release 12 on; but these tables
	# might or might not be present depending on regression test vintage
	if ($old_version < 12)
	{
		_add_st($result, 'regression',
			'drop table if exists abstime_tbl, reltime_tbl, tinterval_tbl');
	}

	# some regression functions gone from release 11 on
	if ($old_version < 11)
	{
		_add_st(
			$result, 'regression',
			'drop function if exists public.boxarea(box)',
			'drop function if exists public.funny_dup17()');
	}

	# version-0 C functions are no longer supported
	if ($old_version < 10)
	{
		_add_st($result, 'regression',
			'drop function oldstyle_length(integer, text)');
	}

	if ($old_version lt '9.5')
	{
		# cope with changes of underlying functions
		_add_st(
			$result,
			'regression',
			'drop operator @#@ (NONE, bigint)',
			'CREATE OPERATOR @#@ ('
			  . 'PROCEDURE = factorial, RIGHTARG = bigint )',
			'drop aggregate public.array_cat_accum(anyarray)',
			'CREATE AGGREGATE array_larger_accum (anyarray) ' . ' ( '
			  . '   sfunc = array_larger, '
			  . '   stype = anyarray, '
			  . '   initcond = $${}$$ ' . '  ) ');

		# "=>" is no longer valid as an operator name
		_add_st($result, 'regression',
			'drop operator if exists public.=> (bigint, NONE)');
	}

	return $result;
}

# Internal subroutine to add statement(s) to the list for the given db.
sub _add_st
{
	my ($result, $db, @st) = @_;

	$result->{$db} ||= [];
	push(@{ $result->{$db} }, @st);
}

=pod

=item adjust_old_dumpfile($old_version, $dump)

Edit a dump output file, taken from the adjusted old-version installation
by current-version C<pg_dumpall -s>, so that it will match the results of
C<pg_dumpall -s> on the pg_upgrade'd installation.

Typically this involves coping with cosmetic differences in the output
of backend subroutines used by pg_dump.

Arguments:

=over

=item C<old_version>: Branch we are upgrading from, represented as a
PostgreSQL::Version object.

=item C<dump>: Contents of dump file

=back

Returns the modified dump text.

=cut

sub adjust_old_dumpfile
{
	my ($old_version, $dump) = @_;

	die "wrong type for \$old_version\n"
	  unless $old_version->isa("PostgreSQL::Version");
	# See adjust_database_contents about this
	$old_version = PostgreSQL::Version->new($old_version->major);

	# use Unix newlines
	$dump =~ s/\r\n/\n/g;

	# Version comments will certainly not match.
	$dump =~ s/^-- Dumped from database version.*\n//mg;

	if ($old_version < 16)
	{
		# Fix up some view queries that no longer require table-qualification.
		$dump = _mash_view_qualifiers($dump);
	}

	if ($old_version >= 14 && $old_version < 17)
	{
		# Fix up some privilege-set discrepancies.
		$dump =~
		  s {^REVOKE SELECT,INSERT,REFERENCES,DELETE,TRIGGER,TRUNCATE,UPDATE ON TABLE}
			{REVOKE ALL ON TABLE}mg;
		$dump =~
		  s {^(GRANT SELECT,INSERT,REFERENCES,TRIGGER,TRUNCATE),UPDATE ON TABLE}
			{$1,MAINTAIN,UPDATE ON TABLE}mg;
	}

	if ($old_version < 14)
	{
		# Remove mentions of extended hash functions.
		$dump =~ s {^(\s+OPERATOR\s1\s=\(integer,integer\))\s,\n
                    \s+FUNCTION\s2\s\(integer,\sinteger\)\spublic\.part_hashint4_noop\(integer,bigint\);}
				   {$1;}mxg;
		$dump =~ s {^(\s+OPERATOR\s1\s=\(text,text\))\s,\n
                    \s+FUNCTION\s2\s\(text,\stext\)\spublic\.part_hashtext_length\(text,bigint\);}
				   {$1;}mxg;
	}

	# Change trigger definitions to say ... EXECUTE FUNCTION ...
	if ($old_version < 12)
	{
		# would like to use lookbehind here but perl complains
		# so do it this way
		$dump =~ s/
			(^CREATE\sTRIGGER\s.*?)
			\sEXECUTE\sPROCEDURE
			/$1 EXECUTE FUNCTION/mgx;
	}

	if ($old_version lt '9.6')
	{
		# adjust some places where we don't print so many parens anymore

		my $prefix =
		  "'New York'\tnew & york | big & apple | nyc\t'new' & 'york'\t";
		my $orig = "( 'new' & 'york' | 'big' & 'appl' ) | 'nyc'";
		my $repl = "'new' & 'york' | 'big' & 'appl' | 'nyc'";
		$dump =~ s/(?<=^\Q$prefix\E)\Q$orig\E/$repl/mg;

		$prefix =
		  "'Sanct Peter'\tPeterburg | peter | 'Sanct Peterburg'\t'sanct' & 'peter'\t";
		$orig = "( 'peterburg' | 'peter' ) | 'sanct' & 'peterburg'";
		$repl = "'peterburg' | 'peter' | 'sanct' & 'peterburg'";
		$dump =~ s/(?<=^\Q$prefix\E)\Q$orig\E/$repl/mg;
	}

	if ($old_version lt '9.5')
	{
		# adjust some places where we don't print so many parens anymore

		my $prefix = "CONSTRAINT (?:sequence|copy)_con CHECK [(][(]";
		my $orig = "((x > 3) AND (y <> 'check failed'::text))";
		my $repl = "(x > 3) AND (y <> 'check failed'::text)";
		$dump =~ s/($prefix)\Q$orig\E/$1$repl/mg;

		$prefix = "CONSTRAINT insert_con CHECK [(][(]";
		$orig = "((x >= 3) AND (y <> 'check failed'::text))";
		$repl = "(x >= 3) AND (y <> 'check failed'::text)";
		$dump =~ s/($prefix)\Q$orig\E/$1$repl/mg;

		$orig = "DEFAULT ((-1) * currval('public.insert_seq'::regclass))";
		$repl =
		  "DEFAULT ('-1'::integer * currval('public.insert_seq'::regclass))";
		$dump =~ s/\Q$orig\E/$repl/mg;

		my $expr =
		  "(rsl.sl_color = rsh.slcolor) AND (rsl.sl_len_cm >= rsh.slminlen_cm)";
		$dump =~ s/WHERE \(\(\Q$expr\E\)/WHERE ($expr/g;

		$expr =
		  "(rule_and_refint_t3.id3a = new.id3a) AND (rule_and_refint_t3.id3b = new.id3b)";
		$dump =~ s/WHERE \(\(\Q$expr\E\)/WHERE ($expr/g;

		$expr =
		  "(rule_and_refint_t3_1.id3a = new.id3a) AND (rule_and_refint_t3_1.id3b = new.id3b)";
		$dump =~ s/WHERE \(\(\Q$expr\E\)/WHERE ($expr/g;
	}

	if ($old_version lt '9.3')
	{
		# CREATE VIEW/RULE statements were not pretty-printed before 9.3.
		# To cope, reduce all whitespace sequences within them to one space.
		# This must be done on both old and new dumps.
		$dump = _mash_view_whitespace($dump);

		# _mash_view_whitespace doesn't handle multi-command rules;
		# rather than trying to fix that, just hack the exceptions manually.

		my $prefix =
		  "CREATE RULE rtest_sys_del AS ON DELETE TO public.rtest_system DO (DELETE FROM public.rtest_interface WHERE (rtest_interface.sysname = old.sysname);";
		my $line2 = " DELETE FROM public.rtest_admin";
		my $line3 = " WHERE (rtest_admin.sysname = old.sysname);";
		$dump =~
		  s/(?<=\Q$prefix\E)\Q$line2$line3\E \);/\n$line2\n $line3\n);/mg;

		$prefix =
		  "CREATE RULE rtest_sys_upd AS ON UPDATE TO public.rtest_system DO (UPDATE public.rtest_interface SET sysname = new.sysname WHERE (rtest_interface.sysname = old.sysname);";
		$line2 = " UPDATE public.rtest_admin SET sysname = new.sysname";
		$line3 = " WHERE (rtest_admin.sysname = old.sysname);";
		$dump =~
		  s/(?<=\Q$prefix\E)\Q$line2$line3\E \);/\n$line2\n $line3\n);/mg;

		# and there's one place where pre-9.3 uses a different table alias
		$dump =~ s {^(CREATE\sRULE\srule_and_refint_t3_ins\sAS\s
			 ON\sINSERT\sTO\spublic\.rule_and_refint_t3\s
			 WHERE\s\(EXISTS\s\(SELECT\s1\sFROM\spublic\.rule_and_refint_t3)\s
			 (WHERE\s\(\(rule_and_refint_t3)
			 (\.id3a\s=\snew\.id3a\)\sAND\s\(rule_and_refint_t3)
			 (\.id3b\s=\snew\.id3b\)\sAND\s\(rule_and_refint_t3)}
		{$1 rule_and_refint_t3_1 $2_1$3_1$4_1}mx;

		# Also fix old use of NATURAL JOIN syntax
		$dump =~ s {NATURAL JOIN public\.credit_card r}
			{JOIN public.credit_card r USING (cid)}mg;
		$dump =~ s {NATURAL JOIN public\.credit_usage r}
			{JOIN public.credit_usage r USING (cid)}mg;
	}

	# Suppress blank lines, as some places in pg_dump emit more or fewer.
	$dump =~ s/\n\n+/\n/g;

	return $dump;
}


# Data for _mash_view_qualifiers
my @_unused_view_qualifiers = (
	# Present at least since 9.2
	{ obj => 'VIEW public.trigger_test_view', qual => 'trigger_test' },
	{ obj => 'VIEW public.domview', qual => 'domtab' },
	{ obj => 'VIEW public.my_property_normal', qual => 'customer' },
	{ obj => 'VIEW public.my_property_secure', qual => 'customer' },
	{ obj => 'VIEW public.pfield_v1', qual => 'pf' },
	{ obj => 'VIEW public.rtest_v1', qual => 'rtest_t1' },
	{ obj => 'VIEW public.rtest_vview1', qual => 'x' },
	{ obj => 'VIEW public.rtest_vview2', qual => 'rtest_view1' },
	{ obj => 'VIEW public.rtest_vview3', qual => 'x' },
	{ obj => 'VIEW public.rtest_vview5', qual => 'rtest_view1' },
	{ obj => 'VIEW public.shoelace_obsolete', qual => 'shoelace' },
	{ obj => 'VIEW public.shoelace_candelete', qual => 'shoelace_obsolete' },
	{ obj => 'VIEW public.toyemp', qual => 'emp' },
	{ obj => 'VIEW public.xmlview4', qual => 'emp' },
	# Since 9.3 (some of these were removed in 9.6)
	{ obj => 'VIEW public.tv', qual => 't' },
	{ obj => 'MATERIALIZED VIEW mvschema.tvm', qual => 'tv' },
	{ obj => 'VIEW public.tvv', qual => 'tv' },
	{ obj => 'MATERIALIZED VIEW public.tvvm', qual => 'tvv' },
	{ obj => 'VIEW public.tvvmv', qual => 'tvvm' },
	{ obj => 'MATERIALIZED VIEW public.bb', qual => 'tvvmv' },
	{ obj => 'VIEW public.nums', qual => 'nums' },
	{ obj => 'VIEW public.sums_1_100', qual => 't' },
	{ obj => 'MATERIALIZED VIEW public.tm', qual => 't' },
	{ obj => 'MATERIALIZED VIEW public.tmm', qual => 'tm' },
	{ obj => 'MATERIALIZED VIEW public.tvmm', qual => 'tvm' },
	# Since 9.4
	{
		obj => 'MATERIALIZED VIEW public.citext_matview',
		qual => 'citext_table'
	},
	{
		obj => 'OR REPLACE VIEW public.key_dependent_view',
		qual => 'view_base_table'
	},
	{
		obj => 'OR REPLACE VIEW public.key_dependent_view_no_cols',
		qual => 'view_base_table'
	},
	# Since 9.5
	{
		obj => 'VIEW public.dummy_seclabel_view1',
		qual => 'dummy_seclabel_tbl2'
	},
	{ obj => 'VIEW public.vv', qual => 'test_tablesample' },
	{ obj => 'VIEW public.test_tablesample_v1', qual => 'test_tablesample' },
	{ obj => 'VIEW public.test_tablesample_v2', qual => 'test_tablesample' },
	# Since 9.6
	{
		obj => 'MATERIALIZED VIEW public.test_pg_dump_mv1',
		qual => 'test_pg_dump_t1'
	},
	{ obj => 'VIEW public.test_pg_dump_v1', qual => 'test_pg_dump_t1' },
	{ obj => 'VIEW public.mvtest_tv', qual => 'mvtest_t' },
	{
		obj => 'MATERIALIZED VIEW mvtest_mvschema.mvtest_tvm',
		qual => 'mvtest_tv'
	},
	{ obj => 'VIEW public.mvtest_tvv', qual => 'mvtest_tv' },
	{ obj => 'MATERIALIZED VIEW public.mvtest_tvvm', qual => 'mvtest_tvv' },
	{ obj => 'VIEW public.mvtest_tvvmv', qual => 'mvtest_tvvm' },
	{ obj => 'MATERIALIZED VIEW public.mvtest_bb', qual => 'mvtest_tvvmv' },
	{ obj => 'MATERIALIZED VIEW public.mvtest_tm', qual => 'mvtest_t' },
	{ obj => 'MATERIALIZED VIEW public.mvtest_tmm', qual => 'mvtest_tm' },
	{ obj => 'MATERIALIZED VIEW public.mvtest_tvmm', qual => 'mvtest_tvm' },
	# Since 10 (some removed in 12)
	{ obj => 'VIEW public.itestv10', qual => 'itest10' },
	{ obj => 'VIEW public.itestv11', qual => 'itest11' },
	{ obj => 'VIEW public.xmltableview2', qual => '"xmltable"' },
	# Since 12
	{
		obj => 'MATERIALIZED VIEW public.tableam_tblmv_heap2',
		qual => 'tableam_tbl_heap2'
	},
	# Since 13
	{ obj => 'VIEW public.limit_thousand_v_1', qual => 'onek' },
	{ obj => 'VIEW public.limit_thousand_v_2', qual => 'onek' },
	{ obj => 'VIEW public.limit_thousand_v_3', qual => 'onek' },
	{ obj => 'VIEW public.limit_thousand_v_4', qual => 'onek' },
	# Since 14
	{ obj => 'MATERIALIZED VIEW public.compressmv', qual => 'cmdata1' });

# Internal subroutine to remove no-longer-used table qualifiers from
# CREATE [MATERIALIZED] VIEW commands.  See list of targeted views above.
sub _mash_view_qualifiers
{
	my ($dump) = @_;

	for my $uvq (@_unused_view_qualifiers)
	{
		my $leader = "CREATE $uvq->{obj} ";
		my $qualifier = $uvq->{qual};
		# Note: we loop because there are presently some cases where the same
		# view name appears in multiple databases.  Fortunately, the same
		# qualifier removal applies or is harmless for each instance ... but
		# we might want to rename some things to avoid assuming that.
		my @splitchunks = split $leader, $dump;
		$dump = shift(@splitchunks);
		foreach my $chunk (@splitchunks)
		{
			my @thischunks = split /;/, $chunk, 2;
			my $stmt = shift(@thischunks);

			# now $stmt is just the body of the CREATE [MATERIALIZED] VIEW
			$stmt =~ s/$qualifier\.//g;

			$dump .= $leader . $stmt . ';' . $thischunks[0];
		}
	}

	# Further hack a few cases where not all occurrences of the qualifier
	# should be removed.
	$dump =~ s {^(CREATE VIEW public\.rtest_vview1 .*?)(a\)\)\);)}
	{$1x.$2}ms;
	$dump =~ s {^(CREATE VIEW public\.rtest_vview3 .*?)(a\)\)\);)}
	{$1x.$2}ms;
	$dump =~
	  s {^(CREATE VIEW public\.shoelace_obsolete .*?)(sl_color\)\)\)\);)}
	{$1shoelace.$2}ms;

	return $dump;
}


# Internal subroutine to mangle whitespace within view/rule commands.
# Any consecutive sequence of whitespace is reduced to one space.
sub _mash_view_whitespace
{
	my ($dump) = @_;

	foreach my $leader ('CREATE VIEW', 'CREATE RULE')
	{
		my @splitchunks = split $leader, $dump;

		$dump = shift(@splitchunks);
		foreach my $chunk (@splitchunks)
		{
			my @thischunks = split /;/, $chunk, 2;
			my $stmt = shift(@thischunks);

			# now $stmt is just the body of the CREATE VIEW/RULE
			$stmt =~ s/\s+/ /sg;
			# we also need to smash these forms for sub-selects and rules
			$stmt =~ s/\( SELECT/(SELECT/g;
			$stmt =~ s/\( INSERT/(INSERT/g;
			$stmt =~ s/\( UPDATE/(UPDATE/g;
			$stmt =~ s/\( DELETE/(DELETE/g;

			$dump .= $leader . $stmt . ';' . $thischunks[0];
		}
	}
	return $dump;
}

=pod

=item adjust_new_dumpfile($old_version, $dump)

Edit a dump output file, taken from the pg_upgrade'd installation
by current-version C<pg_dumpall -s>, so that it will match the old
dump output file as adjusted by C<adjust_old_dumpfile>.

Typically this involves deleting data not present in the old installation.

Arguments:

=over

=item C<old_version>: Branch we are upgrading from, represented as a
PostgreSQL::Version object.

=item C<dump>: Contents of dump file

=back

Returns the modified dump text.

=cut

sub adjust_new_dumpfile
{
	my ($old_version, $dump) = @_;

	die "wrong type for \$old_version\n"
	  unless $old_version->isa("PostgreSQL::Version");
	# See adjust_database_contents about this
	$old_version = PostgreSQL::Version->new($old_version->major);

	# use Unix newlines
	$dump =~ s/\r\n/\n/g;

	# Version comments will certainly not match.
	$dump =~ s/^-- Dumped from database version.*\n//mg;

	if ($old_version < 14)
	{
		# Suppress noise-word uses of IN in CREATE/ALTER PROCEDURE.
		$dump =~ s/^(CREATE PROCEDURE .*?)\(IN /$1(/mg;
		$dump =~ s/^(ALTER PROCEDURE .*?)\(IN /$1(/mg;
		$dump =~ s/^(CREATE PROCEDURE .*?), IN /$1, /mg;
		$dump =~ s/^(ALTER PROCEDURE .*?), IN /$1, /mg;
		$dump =~ s/^(CREATE PROCEDURE .*?), IN /$1, /mg;
		$dump =~ s/^(ALTER PROCEDURE .*?), IN /$1, /mg;

		# Remove SUBSCRIPT clauses in CREATE TYPE.
		$dump =~ s/^\s+SUBSCRIPT = raw_array_subscript_handler,\n//mg;

		# Remove multirange_type_name clauses in CREATE TYPE AS RANGE.
		$dump =~ s {,\n\s+multirange_type_name = .*?(,?)$} {$1}mg;

		# Remove mentions of extended hash functions.
		$dump =~
		  s {^ALTER\sOPERATOR\sFAMILY\spublic\.part_test_int4_ops\sUSING\shash\sADD\n
						\s+FUNCTION\s2\s\(integer,\sinteger\)\spublic\.part_hashint4_noop\(integer,bigint\);} {}mxg;
		$dump =~
		  s {^ALTER\sOPERATOR\sFAMILY\spublic\.part_test_text_ops\sUSING\shash\sADD\n
						\s+FUNCTION\s2\s\(text,\stext\)\spublic\.part_hashtext_length\(text,bigint\);} {}mxg;
	}

	# pre-v12 dumps will not say anything about default_table_access_method.
	if ($old_version < 12)
	{
		$dump =~ s/^SET default_table_access_method = heap;\n//mg;
	}

	# dumps from pre-9.6 dblink may include redundant ACL settings
	if ($old_version lt '9.6')
	{
		my $comment =
		  "-- Name: FUNCTION dblink_connect_u\(.*?\); Type: ACL; Schema: public; Owner: .*";
		my $sql =
		  "REVOKE ALL ON FUNCTION public\.dblink_connect_u\(.*?\) FROM PUBLIC;";
		$dump =~ s/^--\n$comment\n--\n+$sql\n+//mg;
	}

	if ($old_version lt '9.3')
	{
		# CREATE VIEW/RULE statements were not pretty-printed before 9.3.
		# To cope, reduce all whitespace sequences within them to one space.
		# This must be done on both old and new dumps.
		$dump = _mash_view_whitespace($dump);
	}

	# Suppress blank lines, as some places in pg_dump emit more or fewer.
	$dump =~ s/\n\n+/\n/g;

	return $dump;
}

=pod

=back

=cut

1;
