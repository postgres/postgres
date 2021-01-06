test_regex is a module for testing the regular expression package.
It is mostly meant to allow us to absorb Tcl's regex test suite.
Therefore, there are provisions to exercise regex features that
aren't currently exposed at the SQL level by PostgreSQL.

Currently, one function is provided:

test_regex(pattern text, string text, flags text) returns setof text[]

Reports an error if the pattern is an invalid regex.  Otherwise,
the first row of output contains the number of subexpressions,
followed by words reporting set bit(s) in the regex's re_info field.
If the pattern doesn't match the string, that's all.
If the pattern does match, the next row contains the whole match
as the first array element.  If there are parenthesized subexpression(s),
following array elements contain the matches to those subexpressions.
If the "g" (glob) flag is set, then additional row(s) of output similarly
report any additional matches.

The "flags" argument is a string of zero or more single-character
flags that modify the behavior of the regex package or the test
function.  As described in Tcl's reg.test file:

The flag characters are complex and a bit eclectic.  Generally speaking,
lowercase letters are compile options, uppercase are expected re_info
bits, and nonalphabetics are match options, controls for how the test is
run, or testing options.  The one small surprise is that AREs are the
default, and you must explicitly request lesser flavors of RE.  The flags
are as follows.  It is admitted that some are not very mnemonic.

	-	no-op (placeholder)
	0	report indices not actual strings
		(This substitutes for Tcl's -indices switch)
	!	expect partial match, report start position anyway
	%	force small state-set cache in matcher (to test cache replace)
	^	beginning of string is not beginning of line
	$	end of string is not end of line
	*	test is Unicode-specific, needs big character set
	+	provide fake xy equivalence class and ch collating element
		(Note: the equivalence class is implemented, the
		collating element is not; so references to [.ch.] fail)
	,	set REG_PROGRESS (only useful in REG_DEBUG builds)
	.	set REG_DUMP (only useful in REG_DEBUG builds)
	:	set REG_MTRACE (only useful in REG_DEBUG builds)
	;	set REG_FTRACE (only useful in REG_DEBUG builds)

	&	test as both ARE and BRE
		(Not implemented in Postgres, we use separate tests)
	b	BRE
	e	ERE
	a	turn advanced-features bit on (error unless ERE already)
	q	literal string, no metacharacters at all

	g	global match (find all matches)
	i	case-independent matching
	o	("opaque") do not return match locations
	p	newlines are half-magic, excluded from . and [^ only
	w	newlines are half-magic, significant to ^ and $ only
	n	newlines are fully magic, both effects
	x	expanded RE syntax
	t	incomplete-match reporting
	c	canmatch (equivalent to "t0!", in Postgres implementation)
	s	match only at start (REG_BOSONLY)

	A	backslash-_a_lphanumeric seen
	B	ERE/ARE literal-_b_race heuristic used
	E	backslash (_e_scape) seen within []
	H	looka_h_ead constraint seen
	I	_i_mpossible to match
	L	_l_ocale-specific construct seen
	M	unportable (_m_achine-specific) construct seen
	N	RE can match empty (_n_ull) string
	P	non-_P_OSIX construct seen
	Q	{} _q_uantifier seen
	R	back _r_eference seen
	S	POSIX-un_s_pecified syntax seen
	T	prefers shortest (_t_iny)
	U	saw original-POSIX botch: unmatched right paren in ERE (_u_gh)
