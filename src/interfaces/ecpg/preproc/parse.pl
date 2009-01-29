#!/usr/bin/perl
# $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/parse.pl,v 1.3 2009/01/29 09:38:38 petere Exp $
# parser generater for ecpg
# call with backend parser as stdin
#
# Copyright (c) 2007-2009, PostgreSQL Global Development Group
#
# Written by Mike Aubury <mike.aubury@aubit.com>
#	     Michael Meskes <meskes@postgresql.org>
#
# Placed under the same license as PostgreSQL.
#

if (@ARGV) {
	$path = $ARGV[0];
	shift @ARGV;
}

if ($path eq '') { $path = "."; }

$[ = 1;			# set array base to 1
$, = ' ';		# set output field separator
$\ = "\n";		# set output record separator

$copymode = 'off';
$brace_indent = 0;
$yaccmode = 0;
$header_included = 0;
$feature_not_supported = 0;
$tokenmode = 0;

# some token have to be replaced by other symbols
# either in the rule
$replace_token{'BCONST'} = 'ecpg_bconst';
$replace_token{'FCONST'} = 'ecpg_fconst';
$replace_token{'Sconst'} = 'ecpg_sconst';
$replace_token{'IDENT'} = 'ecpg_ident';
$replace_token{'PARAM'} = 'ecpg_param';
# or in the block
$replace_string{'WITH_TIME'} = 'with time';
$replace_string{'NULLS_FIRST'} = 'nulls first';
$replace_string{'NULLS_LAST'} = 'nulls last';
$replace_string{'TYPECAST'} = '::';

# specific replace_types for specific non-terminals - never include the ':'
# ECPG-only replace_types are defined in ecpg-replace_types
$replace_types{'PrepareStmt'} = '<prep>';
$replace_types{'opt_array_bounds'} = '<index>';
# "ignore" means: do not create type and rules for this non-term-id
$replace_types{'stmtblock'} = 'ignore';
$replace_types{'stmtmulti'} = 'ignore';
$replace_types{'CreateAsStmt'} = 'ignore';
$replace_types{'DeallocateStmt'} = 'ignore';
$replace_types{'RuleStmt'} = 'ignore';
$replace_types{'ColLabel'} = 'ignore';
$replace_types{'unreserved_keyword'} = 'ignore';
$replace_types{'Sconst'} = 'ignore';

# some production rules have to be ignored or replaced
$replace_line{'fetch_direction'} = 'ignore';
$replace_line{"opt_array_boundsopt_array_bounds'['Iconst']'"} = 'ignore';
$replace_line{'col_name_keywordCHAR_P'} = 'ignore';
$replace_line{'col_name_keywordINT_P'} = 'ignore';
$replace_line{'col_name_keywordVALUES'} = 'ignore';
$replace_line{'reserved_keywordTO'} = 'ignore';
$replace_line{'reserved_keywordUNION'} = 'ignore';
$replace_line{'VariableShowStmtSHOWvar_name'} = 'SHOW var_name ecpg_into';
$replace_line{'VariableShowStmtSHOWTIMEZONE'} = 'SHOW TIME ZONE ecpg_into';
$replace_line{'VariableShowStmtSHOWTRANSACTIONISOLATIONLEVEL'} = 'SHOW TRANSACTION ISOLATION LEVEL ecpg_into';
$replace_line{'VariableShowStmtSHOWSESSIONAUTHORIZATION'} = 'SHOW SESSION AUTHORIZATION ecpg_into';
$replace_line{'ExecuteStmtEXECUTEnameexecute_param_clause'} = 'EXECUTE prepared_name execute_param_clause execute_rest';
$replace_line{'ExecuteStmtCREATEOptTempTABLEcreate_as_targetASEXECUTEnameexecute_param_clause'} = 'CREATE OptTemp TABLE create_as_target AS EXECUTE prepared_name execute_param_clause';
$replace_line{'PrepareStmtPREPAREnameprep_type_clauseASPreparableStmt'} = 'PREPARE prepared_name prep_type_clause AS PreparableStmt';
$replace_line{'var_nameColId'} = 'ECPGColId';

line: while (<>) {
    chomp;	# strip record separator
    @Fld = split(' ', $_, -1);

    # Dump the action for a rule - 
    # mode indicates if we are processing the 'stmt:' rule (mode==0 means normal,  mode==1 means stmt:)
    # flds are the fields to use. These may start with a '$' - in which case they are the result of a previous non-terminal
    #                             if they dont start with a '$' then they are token name
    #
    # len is the number of fields in flds...
    # leadin is the padding to apply at the beginning (just use for formatting)

    if (/ERRCODE_FEATURE_NOT_SUPPORTED/) {
	$feature_not_supported = 1;
	next line;
    }

    if (/^%%/) {
	$tokenmode = 2;
	$copymode = 'on';
	$yaccmode++;
	$infield = 0;
	$fieldcount = 0;
    }

    $S = $_;
    $prec = 0;
    # Make sure any braces are split
    $s = '{', $S =~ s/$s/ { /g;
    $s = '}', $S =~ s/$s/ } /g;
    # Any comments are split
    $s = '[/][*]', $S =~ s#$s# /* #g;
    $s = '[*][/]', $S =~ s#$s# */ #g;

    # Now split the line into individual fields
    $n = (@arr = split(' ', $S));

    if ($arr[1] eq '%token' && $tokenmode == 0) {
	$tokenmode = 1;
	&include_stuff('tokens', 'ecpg.tokens', '', 1, 0);
	$type = 1;
    }
    elsif ($arr[1] eq '%type' && $header_included == 0) {
	&include_stuff('header', 'ecpg.header', '', 1, 0);
	&include_stuff('ecpgtype', 'ecpg.type', '', 1, 0);
	$header_included = 1;
    }

    if ($tokenmode == 1) {
	$str = '';
	for ($a = 1; $a <= $n; $a++) {
	    if ($arr[$a] eq '/*') {
		$comment++;
		next;
	    }
	    if ($arr[$a] eq '*/') {
		$comment--;
		next;
	    }
	    if ($comment) {
		next;
	    }
	    if (substr($arr[$a], 1, 1) eq '<') {
		next;
		# its a type
	    }
	    $tokens{$arr[$a]} = 1;

	    $str = $str . ' ' . $arr[$a];
	    if ($arr[$a] eq 'IDENT' && $arr[$a - 1] eq '%nonassoc') {
	    # add two more tokens to the list
		$str = $str . "\n%nonassoc CSTRING\n%nonassoc UIDENT";
	    }
	}
	&add_to_buffer('orig_tokens', $str);
	next line;
    }

    # Dont worry about anything if we're not in the right section of gram.y
    if ($yaccmode != 1) {
	next line;
    }

    # Go through each field in turn
    for ($fieldIndexer = 1; $fieldIndexer <= $n; $fieldIndexer++) {
	if ($arr[$fieldIndexer] eq '*/' && $comment) {
	    $comment = 0;
	    next;
	}
	elsif ($comment) {
	    next;
	}
	elsif ($arr[$fieldIndexer] eq '/*') {
	    # start of a multiline comment
	    $comment = 1;
	    next;
	}
	elsif ($arr[$fieldIndexer] eq '//') {
	    next line;
	}
	elsif ($arr[$fieldIndexer] eq '}') {
	    $brace_indent--;
	    next;
	}
	elsif ($arr[$fieldIndexer] eq '{') {
	    $brace_indent++;
	    next;
	}

	if ($brace_indent > 0) {
	    next;
	}
	if ($arr[$fieldIndexer] eq ';') {
	    if ($copymode eq 'on') {
		if ($infield && $includetype eq '') {
		    &dump_line($stmt_mode, $fields, $field_count);
		}
		&add_to_buffer('rules', ";\n\n");
	    }
	    else {
		$copymode = 'on';
	    }
	    $field_count = 0;
	    $infield = 0;
	    $line = '';
	    $includetype = '';
	    next;
	}

	if ($arr[$fieldIndexer] eq '|') {
	    if ($copymode eq 'on') {
		if ($infield && $includetype eq '') {
		    $infield = $infield + &dump_line($stmt_mode, $fields, $field_count);
		}
		if ($infield > 1) {
		    $line = '| ';
		}
	    }
	    $field_count = 0;
	    $includetype = '';
	    next;
	}

	if ($replace_token{$arr[$fieldIndexer]}) {
	    $arr[$fieldIndexer] = $replace_token{$arr[$fieldIndexer]};
	}
	
	# Are we looking at a declaration of a non-terminal ? 
	if (($arr[$fieldIndexer] =~ '[A-Za-z0-9]+:') || $arr[$fieldIndexer + 1] eq ':') {
	    $non_term_id = $arr[$fieldIndexer];
	    $s = ':', $non_term_id =~ s/$s//g;

	    if ($replace_types{$non_term_id} eq '') {
		$replace_types{$non_term_id} = '<str>';
	    }
	    if ($replace_types{$non_term_id} eq 'ignore') {
		$copymode = ';';
		$line = '';
		next line;
	    }
	    else {
		$copymode = 'on';
	    }
	    $line = $line . ' ' . $arr[$fieldIndexer];
	    # Do we have the : attached already ? 
	    # If yes, we'll have already printed the ':'
	    if (!($arr[$fieldIndexer] =~ '[A-Za-z0-9]+:')) {
		# Consume the ':' which is next...
		$line = $line . ':';
		$fieldIndexer++;
	    }

	    # Special mode? 
	    if ($non_term_id eq 'stmt') {
		$stmt_mode = 1;
	    }
	    else {
		$stmt_mode = 0;
	    }
	    $tstr = '%type ' . $replace_types{$non_term_id} . ' ' .  $non_term_id;
	    &add_to_buffer('types', $tstr);

	    if ($copymode eq 'on') {
		&add_to_buffer('rules', $line);
	    }
	    $line = '';
	    $field_count = 0;
	    $infield = 1;
	    next;
	}
	elsif ($copymode eq 'on') {
	    $line = $line . ' ' . $arr[$fieldIndexer];
	}
	if ($arr[$fieldIndexer] eq '%prec') {
	    $prec = 1;
	    next;
	}

	if ($copymode eq 'on' && !$prec && !$comment && $arr[$fieldIndexer] ne '/*EMPTY*/' && length($arr[$fieldIndexer]) && $infield) {
	    $nfield = $field_count + 1;
	    if ($arr[$fieldIndexer] ne 'Op' && ($tokens{$arr[$fieldIndexer]} > 0 || $arr[$fieldIndexer] =~ "'.+'") || $stmt_mode == 1) {
		if ($replace_string{$arr[$fieldIndexer]}) {
		    $S = $replace_string{$arr[$fieldIndexer]};
		}
		else {
		    $S = $arr[$fieldIndexer];
		}
		$s = '_P', $S =~ s/$s//g;
		$s = "'", $S =~ s/$s//g;
		if ($stmt_mode == 1) {
		    $fields{$field_count++} = $S;
		}
		else {
		    $fields{$field_count++} = lc($S);
		}
	    }
	    else {
		$fields{$field_count++} = "\$" . $nfield;
	    }
	}
    }
}

&dump('header');
&dump('tokens');
&dump('types');
&dump('ecpgtype');
&dump('orig_tokens');
print '%%';
print 'prog: statements;';
&dump('rules');
&include_stuff('trailer', 'ecpg.trailer', '', 1, 0);
&dump('trailer');

sub include_stuff {
    local($includestream, $includefilename, $includeblock, $copy, $field_count) = @_;
    $copied = 0;
    $inblock = 0;
    $filename = $path . "/" . $includefilename;
    while (($_ = &Getline2($filename),$getline_ok)) {
	if ($includeblock ne '' && $Fld[1] eq 'ECPG:' && $inblock == 0) {
	    if ($Fld[2] eq $includeblock) {
		$copy = 1;
		$inblock = 1;
		$includetype = $Fld[3];
		if ($includetype eq 'rule') {
		    &dump_fields($stmt_mode, *fields, $field_count, ' { ');
		}
		elsif ($includetype eq 'addon') {
		    &add_to_buffer('rules', ' { ');
		}
	    }
	    else {
		$copy = 0;
	    }
	}
	else {
	    if ($copy == 1 && $Fld[1] ne 'ECPG:') {
		&add_to_buffer($includestream, $_);
		$copied = 1;
		$inblock = 0;
	    }
	}
    }
    delete $opened{$filename} && close($filename);
    if ($includetype eq 'addon') {
	&dump_fields($stmt_mode, *fields, $field_count, '');
    }
    if ($copied == 1) {
	$field_count = 0;
	$line = '';
    }
    $copied;
}

sub add_to_buffer {
    local($buffer, $str) = @_;
    $buff{$buffer, $buffcnt{$buffer}++} = $str;
}

sub dump {
    local($buffer) = @_;
    print '/* ' . $buffer . ' */';
    for ($a = 0; $a < $buffcnt{$buffer}; $a++) {
	print $buff{$buffer, $a};
    }
}

sub dump_fields {
    local($mode, *flds, $len, $ln) = @_;
    if ($mode == 0) {
	#Normal 
	&add_to_buffer('rules', $ln);
	if ($feature_not_supported == 1) {
	    # we found an unsupported feature, but we have to
	    # filter out ExecuteStmt: CREATE OptTemp TABLE ...
	    # because the warning there is only valid in some situations
	    if ($flds{0} ne 'create' || $flds{2} ne 'table') {
		&add_to_buffer('rules', "mmerror(PARSE_ERROR, ET_WARNING, \"unsupported feature will be passed to server\");");
	    }
	    $feature_not_supported = 0;
	}

	if ($len == 0) {
	    # We have no fields ? 
	    &add_to_buffer('rules', " \$\$=EMPTY; }");
	}
	else {
	    # Go through each field and try to 'aggregate' the tokens into a single 'make_str' where possible
	    $cnt = 0;
	    for ($z = 0; $z < $len; $z++) {
		if (substr($flds{$z}, 1, 1) eq "\$") {
		    $flds_new{$cnt++} = $flds{$z};
		    next;
		}

		$str = $flds{$z};

		while (1) {
		    if ($z >= $len - 1 || substr($flds{$z + 1}, 1, 1) eq "\$") {
			# We're at the end...
			$flds_new{$cnt++} = "make_str(\"" . $str . "\")";
			last;
		    }
		    $z++;
		    $str = $str . ' ' . $flds{$z};
		}
	    }

	    # So - how many fields did we end up with ? 
	    if ($cnt == 1) {
		# Straight assignement
		$str = " \$\$ = " . $flds_new{0} . ';';
		&add_to_buffer('rules', $str);
	    }
	    else {
		# Need to concatenate the results to form
		# our final string
		$str = " \$\$ = cat_str(" . $cnt;

		for ($z = 0; $z < $cnt; $z++) {
		    $str = $str . ',' . $flds_new{$z};
		}
		$str = $str . ');';
		&add_to_buffer('rules', $str);
	    }
	    if ($literal_mode == 0) {
		&add_to_buffer('rules', '}');
	    }
	}
    }
    else {
	# we're in the stmt: rule
	if ($len) {
	    # or just the statement ...
	    &add_to_buffer('rules', " { output_statement(\$1, 0, ECPGst_normal); }");
	}
	else {
	    &add_to_buffer('rules', " { \$\$ = NULL; }");
	}
    }
}

sub generate_block {
    local($line) = @_;
    $block = $non_term_id . $line;
    $s = ' ', $block =~ s/$s//g;
    $s = "\\|", $block =~ s/$s//g;
    return $block;
}

sub dump_line {
    local($stmt_mode, $fields, $field_count) = @_;
    $block = &generate_block($line);
    if ($replace_line{$block} eq 'ignore') {
	return 0;
    }
    elsif ($replace_line{$block}) {
	if (index($line, '|') != 0) {
	    $line = '| ' . $replace_line{$block};
	}
	else {
	    $line = $replace_line{$block};
	}
	$block = &generate_block($line);
    }
    &add_to_buffer('rules', $line);
    $i = &include_stuff('rules', 'ecpg.addons', $block, 0, $field_count);
    if ($i == 0) {
	&dump_fields($stmt_mode, *fields, $field_count, ' { ');
    }
    return 1;
}

sub Getline2 {
    &Pick('',@_);
    if ($getline_ok = (($_ = <$fh>) ne '')) {
	chomp;	# strip record separator
	@Fld = split(' ', $_, -1);
    }
    $_;
}

sub Pick {
    local($mode,$name,$pipe) = @_;
    $fh = $name;
    open($name,$mode.$name.$pipe) unless $opened{$name}++;
}
