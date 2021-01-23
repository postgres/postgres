#!/usr/bin/perl
# src/interfaces/ecpg/preproc/parse.pl
# parser generator for ecpg version 2
# call with backend parser as stdin
#
# Copyright (c) 2007-2021, PostgreSQL Global Development Group
#
# Written by Mike Aubury <mike.aubury@aubit.com>
#            Michael Meskes <meskes@postgresql.org>
#            Andy Colson <andy@squeakycode.net>
#
# Placed under the same license as PostgreSQL.
#

use strict;
use warnings;
no warnings 'uninitialized';

my $path = shift @ARGV;
$path = "." unless $path;

my $copymode              = 0;
my $brace_indent          = 0;
my $yaccmode              = 0;
my $in_rule               = 0;
my $header_included       = 0;
my $feature_not_supported = 0;
my $tokenmode             = 0;

my (%buff, $infield, $comment, %tokens, %addons);
my ($stmt_mode, @fields);
my ($line,      $non_term_id);


# some token have to be replaced by other symbols
# either in the rule
my %replace_token = (
	'BCONST' => 'ecpg_bconst',
	'FCONST' => 'ecpg_fconst',
	'Sconst' => 'ecpg_sconst',
	'XCONST' => 'ecpg_xconst',
	'IDENT'  => 'ecpg_ident',
	'PARAM'  => 'ecpg_param',);

# or in the block
my %replace_string = (
	'NOT_LA'         => 'not',
	'NULLS_LA'       => 'nulls',
	'WITH_LA'        => 'with',
	'TYPECAST'       => '::',
	'DOT_DOT'        => '..',
	'COLON_EQUALS'   => ':=',
	'EQUALS_GREATER' => '=>',
	'LESS_EQUALS'    => '<=',
	'GREATER_EQUALS' => '>=',
	'NOT_EQUALS'     => '<>',);

# specific replace_types for specific non-terminals - never include the ':'
# ECPG-only replace_types are defined in ecpg-replace_types
my %replace_types = (
	'PrepareStmt'      => '<prep>',
	'ExecuteStmt'      => '<exec>',
	'opt_array_bounds' => '<index>',

	# "ignore" means: do not create type and rules for this non-term-id
	'parse_toplevel'     => 'ignore',
	'stmtmulti'          => 'ignore',
	'CreateAsStmt'       => 'ignore',
	'DeallocateStmt'     => 'ignore',
	'ColId'              => 'ignore',
	'type_function_name' => 'ignore',
	'ColLabel'           => 'ignore',
	'Sconst'             => 'ignore',
	'opt_distinct_clause' => 'ignore',
	'PLpgSQL_Expr'       => 'ignore',
	'PLAssignStmt'       => 'ignore',
	'plassign_target'    => 'ignore',
	'plassign_equals'    => 'ignore',);

# these replace_line commands excise certain keywords from the core keyword
# lists.  Be sure to account for these in ColLabel and related productions.
my %replace_line = (
	'unreserved_keywordCONNECTION' => 'ignore',
	'unreserved_keywordCURRENT_P'  => 'ignore',
	'unreserved_keywordDAY_P'      => 'ignore',
	'unreserved_keywordHOUR_P'     => 'ignore',
	'unreserved_keywordINPUT_P'    => 'ignore',
	'unreserved_keywordMINUTE_P'   => 'ignore',
	'unreserved_keywordMONTH_P'    => 'ignore',
	'unreserved_keywordSECOND_P'   => 'ignore',
	'unreserved_keywordYEAR_P'     => 'ignore',
	'col_name_keywordCHAR_P'       => 'ignore',
	'col_name_keywordINT_P'        => 'ignore',
	'col_name_keywordVALUES'       => 'ignore',
	'reserved_keywordTO'           => 'ignore',
	'reserved_keywordUNION'        => 'ignore',

	# some other production rules have to be ignored or replaced
	'fetch_argsFORWARDopt_from_incursor_name'      => 'ignore',
	'fetch_argsBACKWARDopt_from_incursor_name'     => 'ignore',
	"opt_array_boundsopt_array_bounds'['Iconst']'" => 'ignore',
	'VariableShowStmtSHOWvar_name' => 'SHOW var_name ecpg_into',
	'VariableShowStmtSHOWTIMEZONE' => 'SHOW TIME ZONE ecpg_into',
	'VariableShowStmtSHOWTRANSACTIONISOLATIONLEVEL' =>
	  'SHOW TRANSACTION ISOLATION LEVEL ecpg_into',
	'VariableShowStmtSHOWSESSIONAUTHORIZATION' =>
	  'SHOW SESSION AUTHORIZATION ecpg_into',
	'returning_clauseRETURNINGtarget_list' =>
	  'RETURNING target_list opt_ecpg_into',
	'ExecuteStmtEXECUTEnameexecute_param_clause' =>
	  'EXECUTE prepared_name execute_param_clause execute_rest',
	'ExecuteStmtCREATEOptTempTABLEcreate_as_targetASEXECUTEnameexecute_param_clauseopt_with_data'
	  => 'CREATE OptTemp TABLE create_as_target AS EXECUTE prepared_name execute_param_clause opt_with_data execute_rest',
	'ExecuteStmtCREATEOptTempTABLEIF_PNOTEXISTScreate_as_targetASEXECUTEnameexecute_param_clauseopt_with_data'
	  => 'CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS EXECUTE prepared_name execute_param_clause opt_with_data execute_rest',
	'PrepareStmtPREPAREnameprep_type_clauseASPreparableStmt' =>
	  'PREPARE prepared_name prep_type_clause AS PreparableStmt',
	'var_nameColId' => 'ECPGColId');

preload_addons();

main();

dump_buffer('header');
dump_buffer('tokens');
dump_buffer('types');
dump_buffer('ecpgtype');
dump_buffer('orig_tokens');
print '%%',                "\n";
print 'prog: statements;', "\n";
dump_buffer('rules');
include_file('trailer', 'ecpg.trailer');
dump_buffer('trailer');

sub main
{
  line: while (<>)
	{
		if (/ERRCODE_FEATURE_NOT_SUPPORTED/)
		{
			$feature_not_supported = 1;
			next line;
		}

		chomp;

		# comment out the line below to make the result file match (blank line wise)
		# the prior version.
		#next if ($_ eq '');

		# Dump the action for a rule -
		# stmt_mode indicates if we are processing the 'stmt:'
		# rule (mode==0 means normal,  mode==1 means stmt:)
		# flds are the fields to use. These may start with a '$' - in
		# which case they are the result of a previous non-terminal
		#
		# if they don't start with a '$' then they are token name
		#
		# len is the number of fields in flds...
		# leadin is the padding to apply at the beginning (just use for formatting)

		if (/^%%/)
		{
			$tokenmode = 2;
			$copymode  = 1;
			$yaccmode++;
			$infield = 0;
		}

		my $prec = 0;

		# Make sure any braces are split
		s/{/ { /g;
		s/}/ } /g;

		# Any comments are split
		s|\/\*| /* |g;
		s|\*\/| */ |g;

		# Now split the line into individual fields
		my @arr = split(' ');

		if ($arr[0] eq '%token' && $tokenmode == 0)
		{
			$tokenmode = 1;
			include_file('tokens', 'ecpg.tokens');
		}
		elsif ($arr[0] eq '%type' && $header_included == 0)
		{
			include_file('header',   'ecpg.header');
			include_file('ecpgtype', 'ecpg.type');
			$header_included = 1;
		}

		if ($tokenmode == 1)
		{
			my $str   = '';
			my $prior = '';
			for my $a (@arr)
			{
				if ($a eq '/*')
				{
					$comment++;
					next;
				}
				if ($a eq '*/')
				{
					$comment--;
					next;
				}
				if ($comment)
				{
					next;
				}
				if (substr($a, 0, 1) eq '<')
				{
					next;

					# its a type
				}
				$tokens{$a} = 1;

				$str = $str . ' ' . $a;
				if ($a eq 'IDENT' && $prior eq '%nonassoc')
				{

					# add more tokens to the list
					$str = $str . "\n%nonassoc CSTRING";
				}
				$prior = $a;
			}
			add_to_buffer('orig_tokens', $str);
			next line;
		}

		# Don't worry about anything if we're not in the right section of gram.y
		if ($yaccmode != 1)
		{
			next line;
		}


		# Go through each field in turn
		for (
			my $fieldIndexer = 0;
			$fieldIndexer < scalar(@arr);
			$fieldIndexer++)
		{
			if ($arr[$fieldIndexer] eq '*/' && $comment)
			{
				$comment = 0;
				next;
			}
			elsif ($comment)
			{
				next;
			}
			elsif ($arr[$fieldIndexer] eq '/*')
			{

				# start of a multiline comment
				$comment = 1;
				next;
			}
			elsif ($arr[$fieldIndexer] eq '//')
			{
				next line;
			}
			elsif ($arr[$fieldIndexer] eq '}')
			{
				$brace_indent--;
				next;
			}
			elsif ($arr[$fieldIndexer] eq '{')
			{
				$brace_indent++;
				next;
			}

			if ($brace_indent > 0)
			{
				next;
			}
			if ($arr[$fieldIndexer] eq ';')
			{
				if ($copymode)
				{
					if ($infield)
					{
						dump_line($stmt_mode, \@fields);
					}
					add_to_buffer('rules', ";\n\n");
				}
				else
				{
					$copymode = 1;
				}
				@fields  = ();
				$infield = 0;
				$line    = '';
				$in_rule = 0;
				next;
			}

			if ($arr[$fieldIndexer] eq '|')
			{
				if ($copymode)
				{
					if ($infield)
					{
						$infield = $infield + dump_line($stmt_mode, \@fields);
					}
					if ($infield > 1)
					{
						$line = '| ';
					}
				}
				@fields = ();
				next;
			}

			if (exists $replace_token{ $arr[$fieldIndexer] })
			{
				$arr[$fieldIndexer] = $replace_token{ $arr[$fieldIndexer] };
			}

			# Are we looking at a declaration of a non-terminal ?
			if (($arr[$fieldIndexer] =~ /[A-Za-z0-9]+:/)
				|| $arr[ $fieldIndexer + 1 ] eq ':')
			{
				$non_term_id = $arr[$fieldIndexer];
				$non_term_id =~ tr/://d;

				if (not defined $replace_types{$non_term_id})
				{
					$replace_types{$non_term_id} = '<str>';
					$copymode = 1;
				}
				elsif ($replace_types{$non_term_id} eq 'ignore')
				{
					$copymode = 0;
					$line     = '';
					next line;
				}
				$line = $line . ' ' . $arr[$fieldIndexer];

				# Do we have the : attached already ?
				# If yes, we'll have already printed the ':'
				if (!($arr[$fieldIndexer] =~ '[A-Za-z0-9]+:'))
				{

					# Consume the ':' which is next...
					$line = $line . ':';
					$fieldIndexer++;
				}

				# Special mode?
				if ($non_term_id eq 'stmt')
				{
					$stmt_mode = 1;
				}
				else
				{
					$stmt_mode = 0;
				}
				my $tstr =
				    '%type '
				  . $replace_types{$non_term_id} . ' '
				  . $non_term_id;
				add_to_buffer('types', $tstr);

				if ($copymode)
				{
					add_to_buffer('rules', $line);
				}
				$line    = '';
				@fields  = ();
				$infield = 1;
				die "unterminated rule at grammar line $.\n"
				  if $in_rule;
				$in_rule = 1;
				next;
			}
			elsif ($copymode)
			{
				$line = $line . ' ' . $arr[$fieldIndexer];
			}
			if ($arr[$fieldIndexer] eq '%prec')
			{
				$prec = 1;
				next;
			}

			if (   $copymode
				&& !$prec
				&& !$comment
				&& length($arr[$fieldIndexer])
				&& $infield)
			{
				if ($arr[$fieldIndexer] ne 'Op'
					&& (   $tokens{ $arr[$fieldIndexer] } > 0
						|| $arr[$fieldIndexer] =~ /'.+'/)
					|| $stmt_mode == 1)
				{
					my $S;
					if (exists $replace_string{ $arr[$fieldIndexer] })
					{
						$S = $replace_string{ $arr[$fieldIndexer] };
					}
					else
					{
						$S = $arr[$fieldIndexer];
					}
					$S =~ s/_P//g;
					$S =~ tr/'//d;
					if ($stmt_mode == 1)
					{
						push(@fields, $S);
					}
					else
					{
						push(@fields, lc($S));
					}
				}
				else
				{
					push(@fields, '$' . (scalar(@fields) + 1));
				}
			}
		}
	}
	die "unterminated rule at end of grammar\n"
	  if $in_rule;
	return;
}


# append a file onto a buffer.
# Arguments:  buffer_name, filename (without path)
sub include_file
{
	my ($buffer, $filename) = @_;
	my $full = "$path/$filename";
	open(my $fh, '<', $full) or die;
	while (<$fh>)
	{
		chomp;
		add_to_buffer($buffer, $_);
	}
	close($fh);
	return;
}

sub include_addon
{
	my ($buffer, $block, $fields, $stmt_mode) = @_;
	my $rec = $addons{$block};
	return 0 unless $rec;

	if ($rec->{type} eq 'rule')
	{
		dump_fields($stmt_mode, $fields, ' { ');
	}
	elsif ($rec->{type} eq 'addon')
	{
		add_to_buffer('rules', ' { ');
	}

	#add_to_buffer( $stream, $_ );
	#We have an array to add to the buffer, we'll add it ourself instead of
	#calling add_to_buffer, which does not know about arrays

	push(@{ $buff{$buffer} }, @{ $rec->{lines} });

	if ($rec->{type} eq 'addon')
	{
		dump_fields($stmt_mode, $fields, '');
	}


	# if we added something (ie there are lines in our array), return 1
	return 1 if (scalar(@{ $rec->{lines} }) > 0);
	return 0;
}


# include_addon does this same thing, but does not call this
# sub... so if you change this, you need to fix include_addon too
#   Pass:  buffer_name, string_to_append
sub add_to_buffer
{
	push(@{ $buff{ $_[0] } }, "$_[1]\n");
	return;
}

sub dump_buffer
{
	my ($buffer) = @_;
	print '/* ', $buffer, ' */', "\n";
	my $ref = $buff{$buffer};
	print @$ref;
	return;
}

sub dump_fields
{
	my ($mode, $flds, $ln) = @_;
	my $len = scalar(@$flds);

	if ($mode == 0)
	{

		#Normal
		add_to_buffer('rules', $ln);
		if ($feature_not_supported == 1)
		{

			# we found an unsupported feature, but we have to
			# filter out ExecuteStmt: CREATE OptTemp TABLE ...
			# because the warning there is only valid in some situations
			if ($flds->[0] ne 'create' || $flds->[2] ne 'table')
			{
				add_to_buffer('rules',
					'mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to server");'
				);
			}
			$feature_not_supported = 0;
		}

		if ($len == 0)
		{

			# We have no fields ?
			add_to_buffer('rules', ' $$=EMPTY; }');
		}
		else
		{

			# Go through each field and try to 'aggregate' the tokens
			# into a single 'mm_strdup' where possible
			my @flds_new;
			my $str;
			for (my $z = 0; $z < $len; $z++)
			{
				if (substr($flds->[$z], 0, 1) eq '$')
				{
					push(@flds_new, $flds->[$z]);
					next;
				}

				$str = $flds->[$z];

				while (1)
				{
					if ($z >= $len - 1
						|| substr($flds->[ $z + 1 ], 0, 1) eq '$')
					{

						# We're at the end...
						push(@flds_new, "mm_strdup(\"$str\")");
						last;
					}
					$z++;
					$str = $str . ' ' . $flds->[$z];
				}
			}

			# So - how many fields did we end up with ?
			$len = scalar(@flds_new);
			if ($len == 1)
			{

				# Straight assignment
				$str = ' $$ = ' . $flds_new[0] . ';';
				add_to_buffer('rules', $str);
			}
			else
			{

				# Need to concatenate the results to form
				# our final string
				$str =
				  ' $$ = cat_str(' . $len . ',' . join(',', @flds_new) . ');';
				add_to_buffer('rules', $str);
			}
			add_to_buffer('rules', '}');
		}
	}
	else
	{

		# we're in the stmt: rule
		if ($len)
		{

			# or just the statement ...
			add_to_buffer('rules',
				' { output_statement($1, 0, ECPGst_normal); }');
		}
		else
		{
			add_to_buffer('rules', ' { $$ = NULL; }');
		}
	}
	return;
}


sub dump_line
{
	my ($stmt_mode, $fields) = @_;
	my $block = $non_term_id . $line;
	$block =~ tr/ |//d;
	my $rep = $replace_line{$block};
	if ($rep)
	{
		if ($rep eq 'ignore')
		{
			return 0;
		}

		if (index($line, '|') != -1)
		{
			$line = '| ' . $rep;
		}
		else
		{
			$line = $rep;
		}
		$block = $non_term_id . $line;
		$block =~ tr/ |//d;
	}
	add_to_buffer('rules', $line);
	my $i = include_addon('rules', $block, $fields, $stmt_mode);
	if ($i == 0)
	{
		dump_fields($stmt_mode, $fields, ' { ');
	}
	return 1;
}

=top
	load addons into cache
	%addons = {
		stmtClosePortalStmt => { 'type' => 'block', 'lines' => [ "{", "if (INFORMIX_MODE)" ..., "}" ] },
		stmtViewStmt => { 'type' => 'rule', 'lines' => [ "| ECPGAllocateDescr", ... ] }
	}

=cut

sub preload_addons
{
	my $filename = $path . "/ecpg.addons";
	open(my $fh, '<', $filename) or die;

	# there may be multiple lines starting ECPG: and then multiple lines of code.
	# the code need to be add to all prior ECPG records.
	my (@needsRules, @code, $record);

	# there may be comments before the first ECPG line, skip them
	my $skip = 1;
	while (<$fh>)
	{
		if (/^ECPG:\s(\S+)\s?(\w+)?/)
		{
			$skip = 0;
			if (@code)
			{
				for my $x (@needsRules)
				{
					push(@{ $x->{lines} }, @code);
				}
				@code       = ();
				@needsRules = ();
			}
			$record          = {};
			$record->{type}  = $2;
			$record->{lines} = [];
			if (exists $addons{$1}) { die "Ga! there are dups!\n"; }
			$addons{$1} = $record;
			push(@needsRules, $record);
		}
		else
		{
			next if $skip;
			push(@code, $_);
		}
	}
	close($fh);
	if (@code)
	{
		for my $x (@needsRules)
		{
			push(@{ $x->{lines} }, @code);
		}
	}
	return;
}
