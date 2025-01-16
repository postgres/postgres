#!/usr/bin/perl
# src/interfaces/ecpg/preproc/parse.pl
# parser generator for ecpg
#
# See README.parser for some explanation of what this does.
#
# Command-line options:
#   --srcdir: where to find ecpg-provided input files (default ".")
#   --parser: the backend gram.y file to read (required, no default)
#   --output: where to write preproc.y (required, no default)
#
# Copyright (c) 2007-2025, PostgreSQL Global Development Group
#
# Written by Mike Aubury <mike.aubury@aubit.com>
#            Michael Meskes <meskes@postgresql.org>
#            Andy Colson <andy@squeakycode.net>
#
# Placed under the same license as PostgreSQL.
#

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

my $srcdir = '.';
my $outfile = '';
my $parser = '';

GetOptions(
	'srcdir=s' => \$srcdir,
	'output=s' => \$outfile,
	'parser=s' => \$parser,) or die "wrong arguments";


# These hash tables define additional transformations to apply to
# grammar rules.  For bug-detection purposes, we count usages of
# each hash table entry in a second hash table, and verify that
# all the entries get used.

# Substitutions to apply to tokens whenever they are seen in a rule.
my %replace_token = (
	'BCONST' => 'ecpg_bconst',
	'FCONST' => 'ecpg_fconst',
	'Sconst' => 'ecpg_sconst',
	'XCONST' => 'ecpg_xconst',
	'IDENT' => 'ecpg_ident',
	'PARAM' => 'ecpg_param',);

my %replace_token_used;

# This hash can provide a result type to override "void" for nonterminals
# that need that, or it can specify 'ignore' to cause us to skip the rule
# for that nonterminal.  (In either case, ecpg.trailer had better provide
# a substitute rule, since the default won't do.)
my %replace_types = (
	'PrepareStmt' => '<prep>',
	'ExecuteStmt' => '<exec>',
	'opt_array_bounds' => '<index>',

	# "ignore" means: do not create type and rules for this nonterminal
	'parse_toplevel' => 'ignore',
	'stmtmulti' => 'ignore',
	'CreateAsStmt' => 'ignore',
	'DeallocateStmt' => 'ignore',
	'ColId' => 'ignore',
	'type_function_name' => 'ignore',
	'ColLabel' => 'ignore',
	'Sconst' => 'ignore',
	'opt_distinct_clause' => 'ignore',
	'PLpgSQL_Expr' => 'ignore',
	'PLAssignStmt' => 'ignore',
	'plassign_target' => 'ignore',
	'plassign_equals' => 'ignore',);

my %replace_types_used;

# This hash provides an "ignore" option or substitute expansion for any
# rule or rule alternative.  The hash key is the same "tokenlist" tag
# used for lookup in ecpg.addons.
my %replace_line = (
	# These entries excise certain keywords from the core keyword lists.
	# Be sure to account for these in ColLabel and related productions.
	'unreserved_keyword CONNECTION' => 'ignore',
	'unreserved_keyword CURRENT_P' => 'ignore',
	'unreserved_keyword DAY_P' => 'ignore',
	'unreserved_keyword HOUR_P' => 'ignore',
	'unreserved_keyword INPUT_P' => 'ignore',
	'unreserved_keyword MINUTE_P' => 'ignore',
	'unreserved_keyword MONTH_P' => 'ignore',
	'unreserved_keyword SECOND_P' => 'ignore',
	'unreserved_keyword YEAR_P' => 'ignore',
	'col_name_keyword CHAR_P' => 'ignore',
	'col_name_keyword INT_P' => 'ignore',
	'col_name_keyword VALUES' => 'ignore',
	'reserved_keyword TO' => 'ignore',
	'reserved_keyword UNION' => 'ignore',

	# some other production rules have to be ignored or replaced
	'fetch_args FORWARD opt_from_in cursor_name' => 'ignore',
	'fetch_args BACKWARD opt_from_in cursor_name' => 'ignore',
	"opt_array_bounds opt_array_bounds '[' Iconst ']'" => 'ignore',
	'VariableShowStmt SHOW var_name' => 'SHOW var_name ecpg_into',
	'VariableShowStmt SHOW TIME ZONE' => 'SHOW TIME ZONE ecpg_into',
	'VariableShowStmt SHOW TRANSACTION ISOLATION LEVEL' =>
	  'SHOW TRANSACTION ISOLATION LEVEL ecpg_into',
	'VariableShowStmt SHOW SESSION AUTHORIZATION' =>
	  'SHOW SESSION AUTHORIZATION ecpg_into',
	'returning_clause RETURNING returning_with_clause target_list' =>
	  'RETURNING returning_with_clause target_list opt_ecpg_into',
	'ExecuteStmt EXECUTE name execute_param_clause' =>
	  'EXECUTE prepared_name execute_param_clause execute_rest',
	'ExecuteStmt CREATE OptTemp TABLE create_as_target AS EXECUTE name execute_param_clause opt_with_data'
	  => 'CREATE OptTemp TABLE create_as_target AS EXECUTE prepared_name execute_param_clause opt_with_data execute_rest',
	'ExecuteStmt CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS EXECUTE name execute_param_clause opt_with_data'
	  => 'CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS EXECUTE prepared_name execute_param_clause opt_with_data execute_rest',
	'PrepareStmt PREPARE name prep_type_clause AS PreparableStmt' =>
	  'PREPARE prepared_name prep_type_clause AS PreparableStmt',
	'var_name ColId' => 'ECPGColId');

my %replace_line_used;


# Declare assorted state variables.

# yaccmode counts the '%%' separator lines we have seen, so that we can
# distinguish prologue, rules, and epilogue sections of gram.y.
my $yaccmode = 0;
# in /* ... */ comment?
my $comment = 0;
# in { ... } braced text?
my $brace_indent = 0;
# within a rule (production)?
my $in_rule = 0;
# count of alternatives processed within the current rule.
my $alt_count = 0;
# copymode = 1 when we want to emit the current rule to preproc.y.
# If it's 0, we have decided to ignore the current rule, and should
# skip all output until we get to the ending semicolon.
my $copymode = 0;
# tokenmode = 1 indicates we are processing %token and following declarations.
my $tokenmode = 0;
# stmt_mode = 1 indicates that we are processing the 'stmt:' rule.
my $stmt_mode = 0;
# Hacky state for emitting feature-not-supported warnings.
my $has_feature_not_supported = 0;
my $has_if_command = 0;

# %addons holds the rules loaded from ecpg.addons.
my %addons;

# %buff holds various named "buffers", which are just strings that accumulate
# the output destined for different sections of the preproc.y file.  This
# allows us to process the input in one pass even though the resulting output
# needs to appear in various places.  See dump_buffer calls below for the
# set of buffer names and the order in which they'll be dumped.
my %buff;

# %tokens contains an entry for every name we have discovered to be a token.
my %tokens;

# $non_term_id is the name of the nonterminal that is the target of the
# current rule.
my $non_term_id;

# $line holds the reconstructed rule text (that is, RHS token list) that
# we plan to emit for the current rule.
my $line = '';

# count of tokens included in $line.
my $line_count = 0;


# Open parser / output file early, to raise errors early.
open(my $parserfh, '<', $parser) or die "could not open parser file $parser";
open(my $outfh, '>', $outfile) or die "could not open output file $outfile";

# Read the various ecpg-supplied input files.
# ecpg.addons is loaded into the %addons hash, while the other files
# are just copied into buffers for verbatim output later.
preload_addons();
include_file('header', 'ecpg.header');
include_file('tokens', 'ecpg.tokens');
include_file('ecpgtype', 'ecpg.type');
include_file('trailer', 'ecpg.trailer');

# Read gram.y, and do the bulk of the processing.
main();

# Emit data from the various buffers we filled.
dump_buffer('header');
dump_buffer('tokens');
dump_buffer('types');
dump_buffer('ecpgtype');
dump_buffer('orig_tokens');
print $outfh '%%', "\n";
print $outfh 'prog: statements;', "\n";
dump_buffer('rules');
dump_buffer('trailer');

close($parserfh);

# Cross-check that we don't have dead or ambiguous addon rules.
foreach (keys %addons)
{
	die "addon rule $_ was never used\n" if $addons{$_}{used} == 0;
	die "addon rule $_ was matched multiple times\n" if $addons{$_}{used} > 1;
}

# Likewise cross-check that entries in our internal hash tables match something.
foreach (keys %replace_token)
{
	die "replace_token entry $_ was never used\n"
	  if !defined($replace_token_used{$_});
	# multiple use of a replace_token entry is fine
}

foreach (keys %replace_types)
{
	die "replace_types entry $_ was never used\n"
	  if !defined($replace_types_used{$_});
	die "replace_types entry $_ was matched multiple times\n"
	  if $replace_types_used{$_} > 1;
}

foreach (keys %replace_line)
{
	die "replace_line entry $_ was never used\n"
	  if !defined($replace_line_used{$_});
	die "replace_line entry $_ was matched multiple times\n"
	  if $replace_line_used{$_} > 1;
}


# Read the backend grammar.
sub main
{
  line: while (<$parserfh>)
	{
		chomp;

		if (/^%%/)
		{
			# New file section, so advance yaccmode.
			$yaccmode++;
			# We are no longer examining %token and related commands.
			$tokenmode = 0;
			# Shouldn't be anything else on the line.
			next line;
		}

		# Hacky check for rules that throw FEATURE_NOT_SUPPORTED
		# (do this before $_ has a chance to get clobbered)
		if ($yaccmode == 1)
		{
			$has_feature_not_supported = 1 if /ERRCODE_FEATURE_NOT_SUPPORTED/;
			$has_if_command = 1 if /^\s*if/;
		}

		# Make sure any braces are split into separate fields
		s/{/ { /g;
		s/}/ } /g;

		# Likewise for comment start/end markers
		s|\/\*| /* |g;
		s|\*\/| */ |g;

		# Now split the line into individual fields
		my @arr = split(' ');

		# Ignore empty lines
		if (!@arr)
		{
			next line;
		}

		# Once we have seen %token in the prologue, we assume all that follows
		# up to the '%%' separator is %token and associativity declarations.
		# Collect and process that as necessary.
		if ($arr[0] eq '%token' && $yaccmode == 0)
		{
			$tokenmode = 1;
		}

		if ($tokenmode == 1)
		{
			# Collect everything of interest on this line into $str.
			my $str = '';
			for my $a (@arr)
			{
				# Skip comments.
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

				# If it's "<something>", it's a type in a %token declaration,
				# which we should just drop so that the tokens have void type.
				if (substr($a, 0, 1) eq '<')
				{
					next;
				}

				# Remember that this is a token.  This will also make entries
				# for "%token" and the associativity keywords such as "%left",
				# which should be harmless so it's not worth the trouble to
				# avoid it.  If a token appears both in %token and in an
				# associativity declaration, we'll redundantly re-set its
				# entry, which is also OK.
				$tokens{$a} = 1;

				# Accumulate the line in $str.
				$str = $str . ' ' . $a;

				# Give our token CSTRING the same precedence as IDENT.
				if ($a eq 'IDENT' && $arr[0] eq '%nonassoc')
				{
					$str = $str . " CSTRING";
				}
			}
			# Save the lightly-processed line in orig_tokens.
			add_to_buffer('orig_tokens', $str);
			next line;
		}

		# The rest is only appropriate if we're in the rules section of gram.y
		if ($yaccmode != 1)
		{
			next line;
		}

		# Go through each word of the rule in turn
		for (
			my $fieldIndexer = 0;
			$fieldIndexer < scalar(@arr);
			$fieldIndexer++)
		{
			# Detect and ignore comments and braced action text
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
				# start of a possibly-multiline comment
				$comment = 1;
				next;
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

			# OK, it's not a comment or part of an action.
			# Check for ';' ending the current rule, or '|' ending the
			# current alternative.
			if ($arr[$fieldIndexer] eq ';')
			{
				if ($copymode)
				{
					# Print the accumulated rule.
					emit_rule();
					add_to_buffer('rules', ";\n\n");
				}
				else
				{
					# End of an ignored rule; revert to copymode = 1.
					$copymode = 1;
				}

				# Reset for the next rule.
				$line = '';
				$line_count = 0;
				$in_rule = 0;
				$alt_count = 0;
				$has_feature_not_supported = 0;
				$has_if_command = 0;
				next;
			}

			if ($arr[$fieldIndexer] eq '|')
			{
				if ($copymode)
				{
					# Print the accumulated alternative.
					# Increment $alt_count for each non-ignored alternative.
					$alt_count += emit_rule();
				}

				# Reset for the next alternative.
				# Start the next line with '|' if we've printed at least one
				# alternative.
				if ($alt_count > 1)
				{
					$line = '| ';
				}
				else
				{
					$line = '';
				}
				$line_count = 0;
				$has_feature_not_supported = 0;
				$has_if_command = 0;
				next;
			}

			# Apply replace_token substitution if we have one.
			if (exists $replace_token{ $arr[$fieldIndexer] })
			{
				$replace_token_used{ $arr[$fieldIndexer] }++;
				$arr[$fieldIndexer] = $replace_token{ $arr[$fieldIndexer] };
			}

			# Are we looking at a declaration of a non-terminal?
			# We detect that by seeing ':' on the end of the token or
			# as the next token.
			if (($arr[$fieldIndexer] =~ /[A-Za-z0-9]+:$/)
				|| (   $fieldIndexer + 1 < scalar(@arr)
					&& $arr[ $fieldIndexer + 1 ] eq ':'))
			{
				# Extract the non-terminal, sans : if any
				$non_term_id = $arr[$fieldIndexer];
				$non_term_id =~ tr/://d;

				# Consume the ':' if it's separate
				if (!($arr[$fieldIndexer] =~ /[A-Za-z0-9]+:$/))
				{
					$fieldIndexer++;
				}

				# Check for %replace_types entry indicating to ignore it.
				if (defined $replace_types{$non_term_id}
					&& $replace_types{$non_term_id} eq 'ignore')
				{
					# We'll ignore this nonterminal and rule altogether.
					$replace_types_used{$non_term_id}++;
					$copymode = 0;
					next line;
				}

				# OK, we want this rule.
				$copymode = 1;

				# Set special mode for the "stmt:" rule.
				if ($non_term_id eq 'stmt')
				{
					$stmt_mode = 1;
				}
				else
				{
					$stmt_mode = 0;
				}

				# Emit appropriate %type declaration for this nonterminal,
				# if it has a type; otherwise omit that.
				if (defined $replace_types{$non_term_id})
				{
					my $tstr =
						'%type '
					  . $replace_types{$non_term_id} . ' '
					  . $non_term_id;
					add_to_buffer('types', $tstr);
					$replace_types_used{$non_term_id}++;
				}

				# Emit the target part of the rule.
				# Note: the leading space is just to match
				# the rather weird pre-v18 output logic.
				my $tstr = ' ' . $non_term_id . ':';
				add_to_buffer('rules', $tstr);

				# Prepare for reading the tokens of the rule.
				$line = '';
				$line_count = 0;
				die "unterminated rule at grammar line $.\n"
				  if $in_rule;
				$in_rule = 1;
				$alt_count = 1;
				next;
			}
			elsif ($copymode)
			{
				# Not a nonterminal declaration, so just add it to $line.
				$line = $line . ' ' . $arr[$fieldIndexer];
				$line_count++;
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
	my $full = "$srcdir/$filename";
	open(my $fh, '<', $full) or die;
	while (<$fh>)
	{
		chomp;
		add_to_buffer($buffer, $_);
	}
	close($fh);
	return;
}

# Emit the semantic action for the current rule.
# This function mainly accounts for any modifications specified
# by an ecpg.addons entry.
sub emit_rule_action
{
	my ($tag) = @_;

	# See if we have an addons entry; if not, just emit default action
	my $rec = $addons{$tag};
	if (!$rec)
	{
		emit_default_action(0);
		return;
	}

	# Track addons entry usage for later cross-check
	$rec->{used}++;

	my $rectype = $rec->{type};
	if ($rectype eq 'rule')
	{
		# Emit default action and then the code block.
		emit_default_action(0);
	}
	elsif ($rectype eq 'addon')
	{
		# Emit the code block wrapped in the same braces as the default action.
		add_to_buffer('rules', ' { ');
	}

	# Emit the addons entry's code block.
	# We have an array to add to the buffer, we'll add it directly instead of
	# calling add_to_buffer, which does not know about arrays.
	push(@{ $buff{'rules'} }, @{ $rec->{lines} });

	if ($rectype eq 'addon')
	{
		emit_default_action(1);
	}
	return;
}

# Add the given line to the specified buffer.
#   Pass:  buffer_name, string_to_append
# Note we add a newline automatically.
sub add_to_buffer
{
	push(@{ $buff{ $_[0] } }, "$_[1]\n");
	return;
}

# Dump the specified buffer to the output file.
sub dump_buffer
{
	my ($buffer) = @_;
	# Label the output for debugging purposes.
	print $outfh '/* ', $buffer, ' */', "\n";
	my $ref = $buff{$buffer};
	print $outfh @$ref;
	return;
}

# Emit the default action (usually token concatenation) for the current rule.
#   Pass: brace_printed boolean
# brace_printed should be true if caller already printed action's open brace.
sub emit_default_action
{
	my ($brace_printed) = @_;

	if ($stmt_mode == 0)
	{
		# Normal rule
		if ($has_feature_not_supported and not $has_if_command)
		{
			# The backend unconditionally reports
			# FEATURE_NOT_SUPPORTED in this rule, so let's emit
			# a warning on the ecpg side.
			if (!$brace_printed)
			{
				add_to_buffer('rules', ' { ');
				$brace_printed = 1;
			}
			add_to_buffer('rules',
				'mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to server");'
			);
		}

		add_to_buffer('rules', '}') if ($brace_printed);
	}
	else
	{
		# We're in the "stmt:" rule, where we need to output special actions.
		# This code assumes that no ecpg.addons entry applies.
		if ($line_count)
		{
			# Any regular kind of statement calls output_statement
			add_to_buffer('rules',
				' { output_statement(@1, 0, ECPGst_normal); }');
		}
		else
		{
			# The empty production for stmt: do nothing
		}
	}
	return;
}

# Print the accumulated rule text (in $line) and the appropriate action.
# Ordinarily return 1.  However, if the rule matches an "ignore"
# entry in %replace_line, then do nothing and return 0.
sub emit_rule
{
	# compute tag to be used as lookup key in %replace_line and %addons
	my $tag = $non_term_id . ' ' . $line;
	$tag =~ tr/|//d;
	$tag = join(' ', split(/\s+/, $tag));

	# apply replace_line substitution if any
	my $rep = $replace_line{$tag};
	if (defined $rep)
	{
		$replace_line_used{$tag}++;

		if ($rep eq 'ignore')
		{
			return 0;
		}

		# non-ignore entries replace the line, but we'd better keep any '|';
		# we don't bother to update $line_count here.
		if (index($line, '|') != -1)
		{
			$line = '| ' . $rep;
		}
		else
		{
			$line = $rep;
		}

		# recompute tag for use in emit_rule_action
		$tag = $non_term_id . ' ' . $line;
		$tag =~ tr/|//d;
		$tag = join(' ', split(/\s+/, $tag));
	}

	# Emit $line, then print the appropriate action.
	add_to_buffer('rules', $line);
	emit_rule_action($tag);
	return 1;
}

=top
	load ecpg.addons into %addons hash.  The result is something like
	%addons = {
		'stmt ClosePortalStmt' => { 'type' => 'block', 'lines' => [ "{", "if (INFORMIX_MODE)" ..., "}" ], 'used' => 0 },
		'stmt ViewStmt' => { 'type' => 'rule', 'lines' => [ "| ECPGAllocateDescr", ... ], 'used' => 0 }
	}

=cut

sub preload_addons
{
	my $filename = $srcdir . "/ecpg.addons";
	open(my $fh, '<', $filename) or die;

	# There may be multiple "ECPG:" lines and then multiple lines of code.
	# The block of code needs to be added to each of the consecutively-
	# preceding "ECPG:" records.
	my (@needsRules, @code);

	# there may be comments before the first "ECPG:" line, skip them
	my $skip = 1;
	while (<$fh>)
	{
		if (/^ECPG:\s+(\w+)\s+(.*)$/)
		{
			# Found an "ECPG:" line, so we're done skipping the header
			$skip = 0;
			my $type = $1;
			my $target = $2;
			# Normalize target so there's exactly one space between tokens
			$target = join(' ', split(/\s+/, $target));
			# Validate record type and target
			die "invalid record type $type in addon rule for $target\n"
			  unless ($type eq 'block'
				or $type eq 'addon'
				or $type eq 'rule');
			die "duplicate addon rule for $target\n"
			  if (exists $addons{$target});
			# If we had some preceding code lines, attach them to all
			# as-yet-unfinished records.
			if (@code)
			{
				for my $x (@needsRules)
				{
					push(@{ $x->{lines} }, @code);
				}
				@code = ();
				@needsRules = ();
			}
			my $record = {};
			$record->{type} = $type;
			$record->{lines} = [];
			$record->{used} = 0;
			$addons{$target} = $record;
			push(@needsRules, $record);
		}
		elsif (/^ECPG:/)
		{
			# Complain if preceding regex failed to match
			die "incorrect syntax in ECPG line: $_\n";
		}
		else
		{
			# Non-ECPG line: add to @code unless we're still skipping
			next if $skip;
			push(@code, $_);
		}
	}
	close($fh);
	# Deal with final code block
	if (@code)
	{
		for my $x (@needsRules)
		{
			push(@{ $x->{lines} }, @code);
		}
	}
	return;
}
