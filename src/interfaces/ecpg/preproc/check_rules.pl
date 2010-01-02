#!/usr/bin/perl
# $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/check_rules.pl,v 1.2 2010/01/02 16:58:11 momjian Exp $
# test parser generater for ecpg
# call with backend parser as stdin
#
# Copyright (c) 2009-2010, PostgreSQL Global Development Group
#
# Written by Michael Meskes <meskes@postgresql.org>
#
# Placed under the same license as PostgreSQL.
#

if (@ARGV) {
        $path = $ARGV[0];
        $parser = $ARGV[1];
}

$[ = 1;                 # set array base to 1

if ($path eq '') { $path = "."; }
$filename = $path . "/ecpg.addons";

if ($parser eq '') { $parser = "../../../backend/parser/gram.y"; }

$replace_line{'ExecuteStmtEXECUTEnameexecute_param_clause'} = 'EXECUTE prepared_name execute_param_clause execute_rest';
$replace_line{'ExecuteStmtCREATEOptTempTABLEcreate_as_targetASEXECUTEnameexecute_param_clause'} = 'CREATE OptTemp TABLE create_as_target AS EXECUTE prepared_name execute_param_clause';
$replace_line{'PrepareStmtPREPAREnameprep_type_clauseASPreparableStmt'} = 'PREPARE prepared_name prep_type_clause AS PreparableStmt';

$block = '';
$ret = 0;
$yaccmod = 0;
$brace_indent = 0;

open GRAM, $parser or die $!;
while (<GRAM>) {
	chomp;      # strip record separator

	if (/^%%/) {
        	$yaccmode++;
	}

	if ($yaccmode != 1) {
	        next;
    	}

	$S = $_;
	$prec = 0;

	# Make sure any braces are split
	$S =~ s/{/ { /g;
	$S =~ s/}/ } /g;
	# Any comments are split
	$S =~ s#[/][*]# /* #g;
	$S =~ s#[*][/]# */ #g;

	# Now split the line into individual fields
	$n = (@arr = split(' ', $S));

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
		    next;
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

		if ($arr[$fieldIndexer] eq ';' || $arr[$fieldIndexer] eq '|') {
			$block = $non_term_id . $block;
			if ($replace_line{$block}) {
				$block = &generate_block($replace_line{$block});
			}
			$found{$block} = 'found';
			$block = '';
		}
		elsif (($arr[$fieldIndexer] =~ '[A-Za-z0-9]+:') || $arr[$fieldIndexer + 1] eq ':') {
			$non_term_id = $arr[$fieldIndexer];
			$non_term_id =~ s/://g;
		}
		else  {
			$block = $block . $arr[$fieldIndexer];
		}
	}
} 

close GRAM;

open ECPG, $filename or die $!;

line: while (<ECPG>) {
    chomp;	# strip record separator
    @Fld = split(' ', $_, -1);

    if (!/^ECPG:/) {
	next line; 
    }

    if ($found{$Fld[2]} ne 'found') {
	printf $Fld[2] . " is not used for building parser!\n";
	$ret = 1;
    }
}

close ECPG;

exit $ret;

sub generate_block {
    local($line) = @_;
    $block = $non_term_id . $line;
    $block =~ s/ //g;
    $s = "\\|", $block =~ s/$s//g;
    return $block;
}

