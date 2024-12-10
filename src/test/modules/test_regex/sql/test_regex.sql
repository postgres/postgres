-- This file is based on tests/reg.test from the Tcl distribution,
-- which is marked
-- # Copyright (c) 1998, 1999 Henry Spencer.  All rights reserved.
-- The full copyright notice can be found in src/backend/regex/COPYRIGHT.
-- Most commented lines below are copied from reg.test.  Each
-- test case is followed by an equivalent test using test_regex().

create extension test_regex;

set standard_conforming_strings = on;

-- # support functions and preliminary misc.
-- # This is sensitive to changes in message wording, but we really have to
-- # test the code->message expansion at least once.
-- ::tcltest::test reg-0.1 "regexp error reporting" {
--     list [catch {regexp (*) ign} msg] $msg
-- } {1 {couldn't compile regular expression pattern: quantifier operand invalid}}
select * from test_regex('(*)', '', '');

-- doing 1 "basic sanity checks"

-- expectMatch	1.1 &		abc	abc		abc
select * from test_regex('abc', 'abc', '');
select * from test_regex('abc', 'abc', 'b');
-- expectNomatch	1.2 &		abc	def
select * from test_regex('abc', 'def', '');
select * from test_regex('abc', 'def', 'b');
-- expectMatch	1.3 &		abc	xyabxabce	abc
select * from test_regex('abc', 'xyabxabce', '');
select * from test_regex('abc', 'xyabxabce', 'b');

-- doing 2 "invalid option combinations"

-- expectError	2.1 qe		a	INVARG
select * from test_regex('a', '', 'qe');
-- expectError	2.2 qa		a	INVARG
select * from test_regex('a', '', 'qa');
-- expectError	2.3 qx		a	INVARG
select * from test_regex('a', '', 'qx');
-- expectError	2.4 qn		a	INVARG
select * from test_regex('a', '', 'qn');
-- expectError	2.5 ba		a	INVARG
select * from test_regex('a', '', 'ba');

-- doing 3 "basic syntax"

-- expectIndices	3.1 &NS		""	a	{0 -1}
select * from test_regex('', 'a', '0NS');
select * from test_regex('', 'a', '0NSb');
-- expectMatch	3.2 NS		a|	a	a
select * from test_regex('a|', 'a', 'NS');
-- expectMatch	3.3 -		a|b	a	a
select * from test_regex('a|b', 'a', '-');
-- expectMatch	3.4 -		a|b	b	b
select * from test_regex('a|b', 'b', '-');
-- expectMatch	3.5 NS		a||b	b	b
select * from test_regex('a||b', 'b', 'NS');
-- expectMatch	3.6 &		ab	ab	ab
select * from test_regex('ab', 'ab', '');
select * from test_regex('ab', 'ab', 'b');

-- doing 4 "parentheses"

-- expectMatch	4.1  -		(a)e		ae	ae	a
select * from test_regex('(a)e', 'ae', '-');
-- expectMatch	4.2  oPR	(.)\1e		abeaae	aae	{}
select * from test_regex('(.)\1e', 'abeaae', 'oPR');
-- expectMatch	4.3  b		{\(a\)b}	ab	ab	a
select * from test_regex('\(a\)b', 'ab', 'b');
-- expectMatch	4.4  -		a((b)c)		abc	abc	bc	b
select * from test_regex('a((b)c)', 'abc', '-');
-- expectMatch	4.5  -		a(b)(c)		abc	abc	b	c
select * from test_regex('a(b)(c)', 'abc', '-');
-- expectError	4.6  -		a(b		EPAREN
select * from test_regex('a(b', '', '-');
-- expectError	4.7  b		{a\(b}		EPAREN
select * from test_regex('a\(b', '', 'b');
-- # sigh, we blew it on the specs here... someday this will be fixed in POSIX,
-- #  but meanwhile, it's fixed in AREs
-- expectMatch	4.8  eU		a)b		a)b	a)b
select * from test_regex('a)b', 'a)b', 'eU');
-- expectError	4.9  -		a)b		EPAREN
select * from test_regex('a)b', '', '-');
-- expectError	4.10 b		{a\)b}		EPAREN
select * from test_regex('a\)b', '', 'b');
-- expectMatch	4.11 P		a(?:b)c		abc	abc
select * from test_regex('a(?:b)c', 'abc', 'P');
-- expectError	4.12 e		a(?:b)c		BADRPT
select * from test_regex('a(?:b)c', '', 'e');
-- expectIndices	4.13 S		a()b		ab	{0 1}	{1 0}
select * from test_regex('a()b', 'ab', '0S');
-- expectMatch	4.14 SP		a(?:)b		ab	ab
select * from test_regex('a(?:)b', 'ab', 'SP');
-- expectIndices	4.15 S		a(|b)c		ac	{0 1}	{1 0}
select * from test_regex('a(|b)c', 'ac', '0S');
-- expectMatch	4.16 S		a(b|)c		abc	abc	b
select * from test_regex('a(b|)c', 'abc', 'S');

-- doing 5 "simple one-char matching"
-- # general case of brackets done later

-- expectMatch	5.1 &		a.b		axb	axb
select * from test_regex('a.b', 'axb', '');
select * from test_regex('a.b', 'axb', 'b');
-- expectNomatch	5.2 &n		"a.b"		"a\nb"
select * from test_regex('a.b', E'a\nb', 'n');
select * from test_regex('a.b', E'a\nb', 'nb');
-- expectMatch	5.3 &		{a[bc]d}	abd	abd
select * from test_regex('a[bc]d', 'abd', '');
select * from test_regex('a[bc]d', 'abd', 'b');
-- expectMatch	5.4 &		{a[bc]d}	acd	acd
select * from test_regex('a[bc]d', 'acd', '');
select * from test_regex('a[bc]d', 'acd', 'b');
-- expectNomatch	5.5 &		{a[bc]d}	aed
select * from test_regex('a[bc]d', 'aed', '');
select * from test_regex('a[bc]d', 'aed', 'b');
-- expectNomatch	5.6 &		{a[^bc]d}	abd
select * from test_regex('a[^bc]d', 'abd', '');
select * from test_regex('a[^bc]d', 'abd', 'b');
-- expectMatch	5.7 &		{a[^bc]d}	aed	aed
select * from test_regex('a[^bc]d', 'aed', '');
select * from test_regex('a[^bc]d', 'aed', 'b');
-- expectNomatch	5.8 &p		"a\[^bc]d"	"a\nd"
select * from test_regex('a[^bc]d', E'a\nd', 'p');
select * from test_regex('a[^bc]d', E'a\nd', 'pb');

-- doing 6 "context-dependent syntax"
-- # plus odds and ends

-- expectError	6.1  -		*	BADRPT
select * from test_regex('*', '', '-');
-- expectMatch	6.2  b		*	*	*
select * from test_regex('*', '*', 'b');
-- expectMatch	6.3  b		{\(*\)}	*	*	*
select * from test_regex('\(*\)', '*', 'b');
-- expectError	6.4  -		(*)	BADRPT
select * from test_regex('(*)', '', '-');
-- expectMatch	6.5  b		^*	*	*
select * from test_regex('^*', '*', 'b');
-- expectError	6.6  -		^*	BADRPT
select * from test_regex('^*', '', '-');
-- expectNomatch	6.7  &		^b	^b
select * from test_regex('^b', '^b', '');
select * from test_regex('^b', '^b', 'b');
-- expectMatch	6.8  b		x^	x^	x^
select * from test_regex('x^', 'x^', 'b');
-- expectNomatch	6.9  I		x^	x
select * from test_regex('x^', 'x', 'I');
-- expectMatch	6.10 n		"\n^"	"x\nb"	"\n"
select * from test_regex(E'\n^', E'x\nb', 'n');
-- expectNomatch	6.11 bS		{\(^b\)} ^b
select * from test_regex('\(^b\)', '^b', 'bS');
-- expectMatch	6.12 -		(^b)	b	b	b
select * from test_regex('(^b)', 'b', '-');
-- expectMatch	6.13 &		{x$}	x	x
select * from test_regex('x$', 'x', '');
select * from test_regex('x$', 'x', 'b');
-- expectMatch	6.14 bS		{\(x$\)} x	x	x
select * from test_regex('\(x$\)', 'x', 'bS');
-- expectMatch	6.15 -		{(x$)}	x	x	x
select * from test_regex('(x$)', 'x', '-');
-- expectMatch	6.16 b		{x$y}	"x\$y"	"x\$y"
select * from test_regex('x$y', 'x$y', 'b');
-- expectNomatch	6.17 I		{x$y}	xy
select * from test_regex('x$y', 'xy', 'I');
-- expectMatch	6.18 n		"x\$\n"	"x\n"	"x\n"
select * from test_regex(E'x$\n', E'x\n', 'n');
-- expectError	6.19 -		+	BADRPT
select * from test_regex('+', '', '-');
-- expectError	6.20 -		?	BADRPT
select * from test_regex('?', '', '-');

-- These two are not yet incorporated in Tcl, cf
-- https://core.tcl-lang.org/tcl/tktview?name=5ea71fdcd3291c38
-- expectError	6.21 -		{x(\w)(?=(\1))}	ESUBREG
select * from test_regex('x(\w)(?=(\1))', '', '-');
-- expectMatch	6.22 HP		{x(?=((foo)))}	xfoo	x
select * from test_regex('x(?=((foo)))', 'xfoo', 'HP');

-- doing 7 "simple quantifiers"

-- expectMatch	7.1  &N		a*	aa	aa
select * from test_regex('a*', 'aa', 'N');
select * from test_regex('a*', 'aa', 'Nb');
-- expectIndices	7.2  &N		a*	b	{0 -1}
select * from test_regex('a*', 'b', '0N');
select * from test_regex('a*', 'b', '0Nb');
-- expectMatch	7.3  -		a+	aa	aa
select * from test_regex('a+', 'aa', '-');
-- expectMatch	7.4  -		a?b	ab	ab
select * from test_regex('a?b', 'ab', '-');
-- expectMatch	7.5  -		a?b	b	b
select * from test_regex('a?b', 'b', '-');
-- expectError	7.6  -		**	BADRPT
select * from test_regex('**', '', '-');
-- expectMatch	7.7  bN		**	***	***
select * from test_regex('**', '***', 'bN');
-- expectError	7.8  &		a**	BADRPT
select * from test_regex('a**', '', '');
select * from test_regex('a**', '', 'b');
-- expectError	7.9  &		a**b	BADRPT
select * from test_regex('a**b', '', '');
select * from test_regex('a**b', '', 'b');
-- expectError	7.10 &		***	BADRPT
select * from test_regex('***', '', '');
select * from test_regex('***', '', 'b');
-- expectError	7.11 -		a++	BADRPT
select * from test_regex('a++', '', '-');
-- expectError	7.12 -		a?+	BADRPT
select * from test_regex('a?+', '', '-');
-- expectError	7.13 -		a?*	BADRPT
select * from test_regex('a?*', '', '-');
-- expectError	7.14 -		a+*	BADRPT
select * from test_regex('a+*', '', '-');
-- expectError	7.15 -		a*+	BADRPT
select * from test_regex('a*+', '', '-');
-- tests for ancient brenext() bugs; not currently in Tcl
select * from test_regex('.*b', 'aaabbb', 'b');
select * from test_regex('.\{1,10\}', 'abcdef', 'bQ');

-- doing 8 "braces"

-- expectMatch	8.1  NQ		"a{0,1}"	""	""
select * from test_regex('a{0,1}', '', 'NQ');
-- expectMatch	8.2  NQ		"a{0,1}"	ac	a
select * from test_regex('a{0,1}', 'ac', 'NQ');
-- expectError	8.3  -		"a{1,0}"	BADBR
select * from test_regex('a{1,0}', '', '-');
-- expectError	8.4  -		"a{1,2,3}"	BADBR
select * from test_regex('a{1,2,3}', '', '-');
-- expectError	8.5  -		"a{257}"	BADBR
select * from test_regex('a{257}', '', '-');
-- expectError	8.6  -		"a{1000}"	BADBR
select * from test_regex('a{1000}', '', '-');
-- expectError	8.7  -		"a{1"		EBRACE
select * from test_regex('a{1', '', '-');
-- expectError	8.8  -		"a{1n}"		BADBR
select * from test_regex('a{1n}', '', '-');
-- expectMatch	8.9  BS		"a{b"		"a\{b"	"a\{b"
select * from test_regex('a{b', 'a{b', 'BS');
-- expectMatch	8.10 BS		"a{"		"a\{"	"a\{"
select * from test_regex('a{', 'a{', 'BS');
-- expectMatch	8.11 bQ		"a\\{0,1\\}b"	cb	b
select * from test_regex('a\{0,1\}b', 'cb', 'bQ');
-- expectError	8.12 b		"a\\{0,1"	EBRACE
select * from test_regex('a\{0,1', '', 'b');
-- expectError	8.13 -		"a{0,1\\"	BADBR
select * from test_regex('a{0,1\', '', '-');
-- expectMatch	8.14 Q		"a{0}b"		ab	b
select * from test_regex('a{0}b', 'ab', 'Q');
-- expectMatch	8.15 Q		"a{0,0}b"	ab	b
select * from test_regex('a{0,0}b', 'ab', 'Q');
-- expectMatch	8.16 Q		"a{0,1}b"	ab	ab
select * from test_regex('a{0,1}b', 'ab', 'Q');
-- expectMatch	8.17 Q		"a{0,2}b"	b	b
select * from test_regex('a{0,2}b', 'b', 'Q');
-- expectMatch	8.18 Q		"a{0,2}b"	aab	aab
select * from test_regex('a{0,2}b', 'aab', 'Q');
-- expectMatch	8.19 Q		"a{0,}b"	aab	aab
select * from test_regex('a{0,}b', 'aab', 'Q');
-- expectMatch	8.20 Q		"a{1,1}b"	aab	ab
select * from test_regex('a{1,1}b', 'aab', 'Q');
-- expectMatch	8.21 Q		"a{1,3}b"	aaaab	aaab
select * from test_regex('a{1,3}b', 'aaaab', 'Q');
-- expectNomatch	8.22 Q		"a{1,3}b"	b
select * from test_regex('a{1,3}b', 'b', 'Q');
-- expectMatch	8.23 Q		"a{1,}b"	aab	aab
select * from test_regex('a{1,}b', 'aab', 'Q');
-- expectNomatch	8.24 Q		"a{2,3}b"	ab
select * from test_regex('a{2,3}b', 'ab', 'Q');
-- expectMatch	8.25 Q		"a{2,3}b"	aaaab	aaab
select * from test_regex('a{2,3}b', 'aaaab', 'Q');
-- expectNomatch	8.26 Q		"a{2,}b"	ab
select * from test_regex('a{2,}b', 'ab', 'Q');
-- expectMatch	8.27 Q		"a{2,}b"	aaaab	aaaab
select * from test_regex('a{2,}b', 'aaaab', 'Q');

-- doing 9 "brackets"

-- expectMatch	9.1  &		{a[bc]}		ac	ac
select * from test_regex('a[bc]', 'ac', '');
select * from test_regex('a[bc]', 'ac', 'b');
-- expectMatch	9.2  &		{a[-]}		a-	a-
select * from test_regex('a[-]', 'a-', '');
select * from test_regex('a[-]', 'a-', 'b');
-- expectMatch	9.3  &		{a[[.-.]]}	a-	a-
select * from test_regex('a[[.-.]]', 'a-', '');
select * from test_regex('a[[.-.]]', 'a-', 'b');
-- expectMatch	9.4  &L		{a[[.zero.]]}	a0	a0
select * from test_regex('a[[.zero.]]', 'a0', 'L');
select * from test_regex('a[[.zero.]]', 'a0', 'Lb');
-- expectMatch	9.5  &LM	{a[[.zero.]-9]}	a2	a2
select * from test_regex('a[[.zero.]-9]', 'a2', 'LM');
select * from test_regex('a[[.zero.]-9]', 'a2', 'LMb');
-- expectMatch	9.6  &M		{a[0-[.9.]]}	a2	a2
select * from test_regex('a[0-[.9.]]', 'a2', 'M');
select * from test_regex('a[0-[.9.]]', 'a2', 'Mb');
-- expectMatch	9.7  &+L	{a[[=x=]]}	ax	ax
select * from test_regex('a[[=x=]]', 'ax', '+L');
select * from test_regex('a[[=x=]]', 'ax', '+Lb');
-- expectMatch	9.8  &+L	{a[[=x=]]}	ay	ay
select * from test_regex('a[[=x=]]', 'ay', '+L');
select * from test_regex('a[[=x=]]', 'ay', '+Lb');
-- expectNomatch	9.9  &+L	{a[[=x=]]}	az
select * from test_regex('a[[=x=]]', 'az', '+L');
select * from test_regex('a[[=x=]]', 'az', '+Lb');
-- expectMatch	9.9b  &iL	{a[[=Y=]]}	ay	ay
select * from test_regex('a[[=Y=]]', 'ay', 'iL');
select * from test_regex('a[[=Y=]]', 'ay', 'iLb');
-- expectNomatch	9.9c  &L	{a[[=Y=]]}	ay
select * from test_regex('a[[=Y=]]', 'ay', 'L');
select * from test_regex('a[[=Y=]]', 'ay', 'Lb');
-- expectError	9.10 &		{a[0-[=x=]]}	ERANGE
select * from test_regex('a[0-[=x=]]', '', '');
select * from test_regex('a[0-[=x=]]', '', 'b');
-- expectMatch	9.11 &L		{a[[:digit:]]}	a0	a0
select * from test_regex('a[[:digit:]]', 'a0', 'L');
select * from test_regex('a[[:digit:]]', 'a0', 'Lb');
-- expectError	9.12 &		{a[[:woopsie:]]}	ECTYPE
select * from test_regex('a[[:woopsie:]]', '', '');
select * from test_regex('a[[:woopsie:]]', '', 'b');
-- expectNomatch	9.13 &L		{a[[:digit:]]}	ab
select * from test_regex('a[[:digit:]]', 'ab', 'L');
select * from test_regex('a[[:digit:]]', 'ab', 'Lb');
-- expectError	9.14 &		{a[0-[:digit:]]}	ERANGE
select * from test_regex('a[0-[:digit:]]', '', '');
select * from test_regex('a[0-[:digit:]]', '', 'b');
-- expectMatch	9.15 &LP	{[[:<:]]a}	a	a
select * from test_regex('[[:<:]]a', 'a', 'LP');
select * from test_regex('[[:<:]]a', 'a', 'LPb');
-- expectMatch	9.16 &LP	{a[[:>:]]}	a	a
select * from test_regex('a[[:>:]]', 'a', 'LP');
select * from test_regex('a[[:>:]]', 'a', 'LPb');
-- expectError	9.17 &		{a[[..]]b}	ECOLLATE
select * from test_regex('a[[..]]b', '', '');
select * from test_regex('a[[..]]b', '', 'b');
-- expectError	9.18 &		{a[[==]]b}	ECOLLATE
select * from test_regex('a[[==]]b', '', '');
select * from test_regex('a[[==]]b', '', 'b');
-- expectError	9.19 &		{a[[::]]b}	ECTYPE
select * from test_regex('a[[::]]b', '', '');
select * from test_regex('a[[::]]b', '', 'b');
-- expectError	9.20 &		{a[[.a}		EBRACK
select * from test_regex('a[[.a', '', '');
select * from test_regex('a[[.a', '', 'b');
-- expectError	9.21 &		{a[[=a}		EBRACK
select * from test_regex('a[[=a', '', '');
select * from test_regex('a[[=a', '', 'b');
-- expectError	9.22 &		{a[[:a}		EBRACK
select * from test_regex('a[[:a', '', '');
select * from test_regex('a[[:a', '', 'b');
-- expectError	9.23 &		{a[}		EBRACK
select * from test_regex('a[', '', '');
select * from test_regex('a[', '', 'b');
-- expectError	9.24 &		{a[b}		EBRACK
select * from test_regex('a[b', '', '');
select * from test_regex('a[b', '', 'b');
-- expectError	9.25 &		{a[b-}		EBRACK
select * from test_regex('a[b-', '', '');
select * from test_regex('a[b-', '', 'b');
-- expectError	9.26 &		{a[b-c}		EBRACK
select * from test_regex('a[b-c', '', '');
select * from test_regex('a[b-c', '', 'b');
-- expectMatch	9.27 &M		{a[b-c]}	ab	ab
select * from test_regex('a[b-c]', 'ab', 'M');
select * from test_regex('a[b-c]', 'ab', 'Mb');
-- expectMatch	9.28 &		{a[b-b]}	ab	ab
select * from test_regex('a[b-b]', 'ab', '');
select * from test_regex('a[b-b]', 'ab', 'b');
-- expectMatch	9.29 &M		{a[1-2]}	a2	a2
select * from test_regex('a[1-2]', 'a2', 'M');
select * from test_regex('a[1-2]', 'a2', 'Mb');
-- expectError	9.30 &		{a[c-b]}	ERANGE
select * from test_regex('a[c-b]', '', '');
select * from test_regex('a[c-b]', '', 'b');
-- expectError	9.31 &		{a[a-b-c]}	ERANGE
select * from test_regex('a[a-b-c]', '', '');
select * from test_regex('a[a-b-c]', '', 'b');
-- expectMatch	9.32 &M		{a[--?]b}	a?b	a?b
select * from test_regex('a[--?]b', 'a?b', 'M');
select * from test_regex('a[--?]b', 'a?b', 'Mb');
-- expectMatch	9.33 &		{a[---]b}	a-b	a-b
select * from test_regex('a[---]b', 'a-b', '');
select * from test_regex('a[---]b', 'a-b', 'b');
-- expectMatch	9.34 &		{a[]b]c}	a]c	a]c
select * from test_regex('a[]b]c', 'a]c', '');
select * from test_regex('a[]b]c', 'a]c', 'b');
-- expectMatch	9.35 EP		{a[\]]b}	a]b	a]b
select * from test_regex('a[\]]b', 'a]b', 'EP');
-- expectNomatch	9.36 bE		{a[\]]b}	a]b
select * from test_regex('a[\]]b', 'a]b', 'bE');
-- expectMatch	9.37 bE		{a[\]]b}	"a\\]b"	"a\\]b"
select * from test_regex('a[\]]b', 'a\]b', 'bE');
-- expectMatch	9.38 eE		{a[\]]b}	"a\\]b"	"a\\]b"
select * from test_regex('a[\]]b', 'a\]b', 'eE');
-- expectMatch	9.39 EP		{a[\\]b}	"a\\b"	"a\\b"
select * from test_regex('a[\\]b', 'a\b', 'EP');
-- expectMatch	9.40 eE		{a[\\]b}	"a\\b"	"a\\b"
select * from test_regex('a[\\]b', 'a\b', 'eE');
-- expectMatch	9.41 bE		{a[\\]b}	"a\\b"	"a\\b"
select * from test_regex('a[\\]b', 'a\b', 'bE');
-- expectError	9.42 -		{a[\Z]b}	EESCAPE
select * from test_regex('a[\Z]b', '', '-');
-- expectMatch	9.43 &		{a[[b]c}	"a\[c"	"a\[c"
select * from test_regex('a[[b]c', 'a[c', '');
select * from test_regex('a[[b]c', 'a[c', 'b');
-- This only works in UTF8 encoding, so it's moved to test_regex_utf8.sql:
-- expectMatch	9.44 EMP*	{a[\u00fe-\u0507][\u00ff-\u0300]b} \
-- 	"a\u0102\u02ffb"	"a\u0102\u02ffb"

-- doing 10 "anchors and newlines"

-- expectMatch	10.1  &		^a	a	a
select * from test_regex('^a', 'a', '');
select * from test_regex('^a', 'a', 'b');
-- expectNomatch	10.2  &^	^a	a
select * from test_regex('^a', 'a', '^');
select * from test_regex('^a', 'a', '^b');
-- expectIndices	10.3  &N	^	a	{0 -1}
select * from test_regex('^', 'a', '0N');
select * from test_regex('^', 'a', '0Nb');
-- expectIndices	10.4  &		{a$}	aba	{2 2}
select * from test_regex('a$', 'aba', '0');
select * from test_regex('a$', 'aba', '0b');
-- expectNomatch	10.5  {&$}	{a$}	a
select * from test_regex('a$', 'a', '$');
select * from test_regex('a$', 'a', '$b');
-- expectIndices	10.6  &N	{$}	ab	{2 1}
select * from test_regex('$', 'ab', '0N');
select * from test_regex('$', 'ab', '0Nb');
-- expectMatch	10.7  &n	^a	a	a
select * from test_regex('^a', 'a', 'n');
select * from test_regex('^a', 'a', 'nb');
-- expectMatch	10.8  &n	"^a"	"b\na"	"a"
select * from test_regex('^a', E'b\na', 'n');
select * from test_regex('^a', E'b\na', 'nb');
-- expectIndices	10.9  &w	"^a"	"a\na"	{0 0}
select * from test_regex('^a', E'a\na', '0w');
select * from test_regex('^a', E'a\na', '0wb');
-- expectIndices	10.10 &n^	"^a"	"a\na"	{2 2}
select * from test_regex('^a', E'a\na', '0n^');
select * from test_regex('^a', E'a\na', '0n^b');
-- expectMatch	10.11 &n	{a$}	a	a
select * from test_regex('a$', 'a', 'n');
select * from test_regex('a$', 'a', 'nb');
-- expectMatch	10.12 &n	"a\$"	"a\nb"	"a"
select * from test_regex('a$', E'a\nb', 'n');
select * from test_regex('a$', E'a\nb', 'nb');
-- expectIndices	10.13 &n	"a\$"	"a\na"	{0 0}
select * from test_regex('a$', E'a\na', '0n');
select * from test_regex('a$', E'a\na', '0nb');
-- expectIndices	10.14 N		^^	a	{0 -1}
select * from test_regex('^^', 'a', '0N');
-- expectMatch	10.15 b		^^	^	^
select * from test_regex('^^', '^', 'b');
-- expectIndices	10.16 N		{$$}	a	{1 0}
select * from test_regex('$$', 'a', '0N');
-- expectMatch	10.17 b		{$$}	"\$"	"\$"
select * from test_regex('$$', '$', 'b');
-- expectMatch	10.18 &N	{^$}	""	""
select * from test_regex('^$', '', 'N');
select * from test_regex('^$', '', 'Nb');
-- expectNomatch	10.19 &N	{^$}	a
select * from test_regex('^$', 'a', 'N');
select * from test_regex('^$', 'a', 'Nb');
-- expectIndices	10.20 &nN	"^\$"	a\n\nb	{2 1}
select * from test_regex('^$', E'a\n\nb', '0nN');
select * from test_regex('^$', E'a\n\nb', '0nNb');
-- expectMatch	10.21 N		{$^}	""	""
select * from test_regex('$^', '', 'N');
-- expectMatch	10.22 b		{$^}	"\$^"	"\$^"
select * from test_regex('$^', '$^', 'b');
-- expectMatch	10.23 P		{\Aa}	a	a
select * from test_regex('\Aa', 'a', 'P');
-- expectMatch	10.24 ^P	{\Aa}	a	a
select * from test_regex('\Aa', 'a', '^P');
-- expectNomatch	10.25 ^nP	{\Aa}	"b\na"
select * from test_regex('\Aa', E'b\na', '^nP');
-- expectMatch	10.26 P		{a\Z}	a	a
select * from test_regex('a\Z', 'a', 'P');
-- expectMatch	10.27 \$P	{a\Z}	a	a
select * from test_regex('a\Z', 'a', '$P');
-- expectNomatch	10.28 \$nP	{a\Z}	"a\nb"
select * from test_regex('a\Z', E'a\nb', '$nP');
-- expectError	10.29 -		^*	BADRPT
select * from test_regex('^*', '', '-');
-- expectError	10.30 -		{$*}	BADRPT
select * from test_regex('$*', '', '-');
-- expectError	10.31 -		{\A*}	BADRPT
select * from test_regex('\A*', '', '-');
-- expectError	10.32 -		{\Z*}	BADRPT
select * from test_regex('\Z*', '', '-');

-- doing 11 "boundary constraints"

-- expectMatch	11.1  &LP	{[[:<:]]a}	a	a
select * from test_regex('[[:<:]]a', 'a', 'LP');
select * from test_regex('[[:<:]]a', 'a', 'LPb');
-- expectMatch	11.2  &LP	{[[:<:]]a}	-a	a
select * from test_regex('[[:<:]]a', '-a', 'LP');
select * from test_regex('[[:<:]]a', '-a', 'LPb');
-- expectNomatch	11.3  &LP	{[[:<:]]a}	ba
select * from test_regex('[[:<:]]a', 'ba', 'LP');
select * from test_regex('[[:<:]]a', 'ba', 'LPb');
-- expectMatch	11.4  &LP	{a[[:>:]]}	a	a
select * from test_regex('a[[:>:]]', 'a', 'LP');
select * from test_regex('a[[:>:]]', 'a', 'LPb');
-- expectMatch	11.5  &LP	{a[[:>:]]}	a-	a
select * from test_regex('a[[:>:]]', 'a-', 'LP');
select * from test_regex('a[[:>:]]', 'a-', 'LPb');
-- expectNomatch	11.6  &LP	{a[[:>:]]}	ab
select * from test_regex('a[[:>:]]', 'ab', 'LP');
select * from test_regex('a[[:>:]]', 'ab', 'LPb');
-- expectMatch	11.7  bLP	{\<a}		a	a
select * from test_regex('\<a', 'a', 'bLP');
-- expectNomatch	11.8  bLP	{\<a}		ba
select * from test_regex('\<a', 'ba', 'bLP');
-- expectMatch	11.9  bLP	{a\>}		a	a
select * from test_regex('a\>', 'a', 'bLP');
-- expectNomatch	11.10 bLP	{a\>}		ab
select * from test_regex('a\>', 'ab', 'bLP');
-- expectMatch	11.11 LP	{\ya}		a	a
select * from test_regex('\ya', 'a', 'LP');
-- expectNomatch	11.12 LP	{\ya}		ba
select * from test_regex('\ya', 'ba', 'LP');
-- expectMatch	11.13 LP	{a\y}		a	a
select * from test_regex('a\y', 'a', 'LP');
-- expectNomatch	11.14 LP	{a\y}		ab
select * from test_regex('a\y', 'ab', 'LP');
-- expectMatch	11.15 LP	{a\Y}		ab	a
select * from test_regex('a\Y', 'ab', 'LP');
-- expectNomatch	11.16 LP	{a\Y}		a-
select * from test_regex('a\Y', 'a-', 'LP');
-- expectNomatch	11.17 LP	{a\Y}		a
select * from test_regex('a\Y', 'a', 'LP');
-- expectNomatch	11.18 LP	{-\Y}		-a
select * from test_regex('-\Y', '-a', 'LP');
-- expectMatch	11.19 LP	{-\Y}		-%	-
select * from test_regex('-\Y', '-%', 'LP');
-- expectNomatch	11.20 LP	{\Y-}		a-
select * from test_regex('\Y-', 'a-', 'LP');
-- expectError	11.21 -		{[[:<:]]*}	BADRPT
select * from test_regex('[[:<:]]*', '', '-');
-- expectError	11.22 -		{[[:>:]]*}	BADRPT
select * from test_regex('[[:>:]]*', '', '-');
-- expectError	11.23 b		{\<*}		BADRPT
select * from test_regex('\<*', '', 'b');
-- expectError	11.24 b		{\>*}		BADRPT
select * from test_regex('\>*', '', 'b');
-- expectError	11.25 -		{\y*}		BADRPT
select * from test_regex('\y*', '', '-');
-- expectError	11.26 -		{\Y*}		BADRPT
select * from test_regex('\Y*', '', '-');
-- expectMatch	11.27 LP	{\ma}		a	a
select * from test_regex('\ma', 'a', 'LP');
-- expectNomatch	11.28 LP	{\ma}		ba
select * from test_regex('\ma', 'ba', 'LP');
-- expectMatch	11.29 LP	{a\M}		a	a
select * from test_regex('a\M', 'a', 'LP');
-- expectNomatch	11.30 LP	{a\M}		ab
select * from test_regex('a\M', 'ab', 'LP');
-- expectNomatch	11.31 ILP	{\Ma}		a
select * from test_regex('\Ma', 'a', 'ILP');
-- expectNomatch	11.32 ILP	{a\m}		a
select * from test_regex('a\m', 'a', 'ILP');

-- doing 12 "character classes"

-- expectMatch	12.1  LP	{a\db}		a0b	a0b
select * from test_regex('a\db', 'a0b', 'LP');
-- expectNomatch	12.2  LP	{a\db}		axb
select * from test_regex('a\db', 'axb', 'LP');
-- expectNomatch	12.3  LP	{a\Db}		a0b
select * from test_regex('a\Db', 'a0b', 'LP');
-- expectMatch	12.4  LP	{a\Db}		axb	axb
select * from test_regex('a\Db', 'axb', 'LP');
-- expectMatch	12.5  LP	"a\\sb"		"a b"	"a b"
select * from test_regex('a\sb', 'a b', 'LP');
-- expectMatch	12.6  LP	"a\\sb"		"a\tb"	"a\tb"
select * from test_regex('a\sb', E'a\tb', 'LP');
-- expectMatch	12.7  LP	"a\\sb"		"a\nb"	"a\nb"
select * from test_regex('a\sb', E'a\nb', 'LP');
-- expectNomatch	12.8  LP	{a\sb}		axb
select * from test_regex('a\sb', 'axb', 'LP');
-- expectMatch	12.9  LP	{a\Sb}		axb	axb
select * from test_regex('a\Sb', 'axb', 'LP');
-- expectNomatch	12.10 LP	"a\\Sb"		"a b"
select * from test_regex('a\Sb', 'a b', 'LP');
-- expectMatch	12.11 LP	{a\wb}		axb	axb
select * from test_regex('a\wb', 'axb', 'LP');
-- expectNomatch	12.12 LP	{a\wb}		a-b
select * from test_regex('a\wb', 'a-b', 'LP');
-- expectNomatch	12.13 LP	{a\Wb}		axb
select * from test_regex('a\Wb', 'axb', 'LP');
-- expectMatch	12.14 LP	{a\Wb}		a-b	a-b
select * from test_regex('a\Wb', 'a-b', 'LP');
-- expectMatch	12.15 LP	{\y\w+z\y}	adze-guz	guz
select * from test_regex('\y\w+z\y', 'adze-guz', 'LP');
-- expectMatch	12.16 LPE	{a[\d]b}	a1b	a1b
select * from test_regex('a[\d]b', 'a1b', 'LPE');
-- expectMatch	12.17 LPE	"a\[\\s]b"	"a b"	"a b"
select * from test_regex('a[\s]b', 'a b', 'LPE');
-- expectMatch	12.18 LPE	{a[\w]b}	axb	axb
select * from test_regex('a[\w]b', 'axb', 'LPE');

-- these should be invalid
select * from test_regex('[\w-~]*', 'ab01_~-`**', 'LNPSE');
select * from test_regex('[~-\w]*', 'ab01_~-`**', 'LNPSE');
select * from test_regex('[[:alnum:]-~]*', 'ab01~-`**', 'LNS');
select * from test_regex('[~-[:alnum:]]*', 'ab01~-`**', 'LNS');

-- test complemented char classes within brackets
select * from test_regex('[\D]', '0123456789abc*', 'LPE');
select * from test_regex('[^\D]', 'abc0123456789*', 'LPE');
select * from test_regex('[1\D7]', '0123456789abc*', 'LPE');
select * from test_regex('[7\D1]', '0123456789abc*', 'LPE');
select * from test_regex('[^0\D1]', 'abc0123456789*', 'LPE');
select * from test_regex('[^1\D0]', 'abc0123456789*', 'LPE');
select * from test_regex('\W', '0123456789abc_*', 'LP');
select * from test_regex('[\W]', '0123456789abc_*', 'LPE');
select * from test_regex('[\s\S]*', '012  3456789abc_*', 'LNPE');
-- bug #18708:
select * from test_regex('(?:[^\d\D]){0}', '0123456789abc*', 'LNPQE');
select * from test_regex('[^\d\D]', '0123456789abc*', 'ILPE');

-- check char classes' handling of newlines
select * from test_regex('\s+', E'abc  \n  def', 'LP');
select * from test_regex('\s+', E'abc  \n  def', 'nLP');
select * from test_regex('[\s]+', E'abc  \n  def', 'LPE');
select * from test_regex('[\s]+', E'abc  \n  def', 'nLPE');
select * from test_regex('\S+', E'abc\ndef', 'LP');
select * from test_regex('\S+', E'abc\ndef', 'nLP');
select * from test_regex('[\S]+', E'abc\ndef', 'LPE');
select * from test_regex('[\S]+', E'abc\ndef', 'nLPE');
select * from test_regex('\d+', E'012\n345', 'LP');
select * from test_regex('\d+', E'012\n345', 'nLP');
select * from test_regex('[\d]+', E'012\n345', 'LPE');
select * from test_regex('[\d]+', E'012\n345', 'nLPE');
select * from test_regex('\D+', E'abc\ndef345', 'LP');
select * from test_regex('\D+', E'abc\ndef345', 'nLP');
select * from test_regex('[\D]+', E'abc\ndef345', 'LPE');
select * from test_regex('[\D]+', E'abc\ndef345', 'nLPE');
select * from test_regex('\w+', E'abc_012\ndef', 'LP');
select * from test_regex('\w+', E'abc_012\ndef', 'nLP');
select * from test_regex('[\w]+', E'abc_012\ndef', 'LPE');
select * from test_regex('[\w]+', E'abc_012\ndef', 'nLPE');
select * from test_regex('\W+', E'***\n@@@___', 'LP');
select * from test_regex('\W+', E'***\n@@@___', 'nLP');
select * from test_regex('[\W]+', E'***\n@@@___', 'LPE');
select * from test_regex('[\W]+', E'***\n@@@___', 'nLPE');


-- doing 13 "escapes"

-- expectError	13.1  &		"a\\"		EESCAPE
select * from test_regex('a\', '', '');
select * from test_regex('a\', '', 'b');
-- expectMatch	13.2  -		{a\<b}		a<b	a<b
select * from test_regex('a\<b', 'a<b', '-');
-- expectMatch	13.3  e		{a\<b}		a<b	a<b
select * from test_regex('a\<b', 'a<b', 'e');
-- expectMatch	13.4  bAS	{a\wb}		awb	awb
select * from test_regex('a\wb', 'awb', 'bAS');
-- expectMatch	13.5  eAS	{a\wb}		awb	awb
select * from test_regex('a\wb', 'awb', 'eAS');
-- expectMatch	13.6  PL	"a\\ab"		"a\007b"	"a\007b"
select * from test_regex('a\ab', E'a\007b', 'PL');
-- expectMatch	13.7  P		"a\\bb"		"a\bb"	"a\bb"
select * from test_regex('a\bb', E'a\bb', 'P');
-- expectMatch	13.8  P		{a\Bb}		"a\\b"	"a\\b"
select * from test_regex('a\Bb', 'a\b', 'P');
-- expectMatch	13.9  MP	"a\\chb"	"a\bb"	"a\bb"
select * from test_regex('a\chb', E'a\bb', 'MP');
-- expectMatch	13.10 MP	"a\\cHb"	"a\bb"	"a\bb"
select * from test_regex('a\cHb', E'a\bb', 'MP');
-- expectMatch	13.11 LMP	"a\\e"		"a\033"	"a\033"
select * from test_regex('a\e', E'a\033', 'LMP');
-- expectMatch	13.12 P		"a\\fb"		"a\fb"	"a\fb"
select * from test_regex('a\fb', E'a\fb', 'P');
-- expectMatch	13.13 P		"a\\nb"		"a\nb"	"a\nb"
select * from test_regex('a\nb', E'a\nb', 'P');
-- expectMatch	13.14 P		"a\\rb"		"a\rb"	"a\rb"
select * from test_regex('a\rb', E'a\rb', 'P');
-- expectMatch	13.15 P		"a\\tb"		"a\tb"	"a\tb"
select * from test_regex('a\tb', E'a\tb', 'P');
-- expectMatch	13.16 P		"a\\u0008x"	"a\bx"	"a\bx"
select * from test_regex('a\u0008x', E'a\bx', 'P');
-- expectMatch	13.17 P		{a\u008x}	"a\bx"	"a\bx"
-- Tcl has relaxed their code to allow 1-4 hex digits, but Postgres hasn't
select * from test_regex('a\u008x', E'a\bx', 'P');
-- expectMatch	13.18 P		"a\\u00088x"	"a\b8x"	"a\b8x"
select * from test_regex('a\u00088x', E'a\b8x', 'P');
-- expectMatch	13.19 P		"a\\U00000008x"	"a\bx"	"a\bx"
select * from test_regex('a\U00000008x', E'a\bx', 'P');
-- expectMatch	13.20 P		{a\U0000008x}	"a\bx"	"a\bx"
-- Tcl has relaxed their code to allow 1-8 hex digits, but Postgres hasn't
select * from test_regex('a\U0000008x', E'a\bx', 'P');
-- expectMatch	13.21 P		"a\\vb"		"a\vb"	"a\vb"
select * from test_regex('a\vb', E'a\013b', 'P');
-- expectMatch	13.22 MP	"a\\x08x"	"a\bx"	"a\bx"
select * from test_regex('a\x08x', E'a\bx', 'MP');
-- expectError	13.23 -		{a\xq}		EESCAPE
select * from test_regex('a\xq', '', '-');
-- expectMatch	13.24 MP	"a\\x08x"	"a\bx"	"a\bx"
select * from test_regex('a\x08x', E'a\bx', 'MP');
-- expectError	13.25 -		{a\z}		EESCAPE
select * from test_regex('a\z', '', '-');
-- expectMatch	13.26 MP	"a\\010b"	"a\bb"	"a\bb"
select * from test_regex('a\010b', E'a\bb', 'MP');
-- These only work in UTF8 encoding, so they're moved to test_regex_utf8.sql:
-- expectMatch	13.27 P		"a\\U00001234x"	"a\u1234x"	"a\u1234x"
-- expectMatch	13.28 P		{a\U00001234x}	"a\u1234x"	"a\u1234x"
-- expectMatch	13.29 P		"a\\U0001234x"	"a\u1234x"	"a\u1234x"
-- expectMatch	13.30 P		{a\U0001234x}	"a\u1234x"	"a\u1234x"
-- expectMatch	13.31 P		"a\\U000012345x"	"a\u12345x"	"a\u12345x"
-- expectMatch	13.32 P		{a\U000012345x}	"a\u12345x"	"a\u12345x"
-- expectMatch	13.33 P		"a\\U1000000x"	"a\ufffd0x"	"a\ufffd0x"
-- expectMatch	13.34 P		{a\U1000000x}	"a\ufffd0x"	"a\ufffd0x"

-- doing 14 "back references"
-- # ugh

-- expectMatch	14.1  RP	{a(b*)c\1}	abbcbb	abbcbb	bb
select * from test_regex('a(b*)c\1', 'abbcbb', 'RP');
-- expectMatch	14.2  RP	{a(b*)c\1}	ac	ac	""
select * from test_regex('a(b*)c\1', 'ac', 'RP');
-- expectNomatch	14.3  RP	{a(b*)c\1}	abbcb
select * from test_regex('a(b*)c\1', 'abbcb', 'RP');
-- expectMatch	14.4  RP	{a(b*)\1}	abbcbb	abb	b
select * from test_regex('a(b*)\1', 'abbcbb', 'RP');
-- expectMatch	14.5  RP	{a(b|bb)\1}	abbcbb	abb	b
select * from test_regex('a(b|bb)\1', 'abbcbb', 'RP');
-- expectMatch	14.6  RP	{a([bc])\1}	abb	abb	b
select * from test_regex('a([bc])\1', 'abb', 'RP');
-- expectNomatch	14.7  RP	{a([bc])\1}	abc
select * from test_regex('a([bc])\1', 'abc', 'RP');
-- expectMatch	14.8  RP	{a([bc])\1}	abcabb	abb	b
select * from test_regex('a([bc])\1', 'abcabb', 'RP');
-- expectNomatch	14.9  RP	{a([bc])*\1}	abc
select * from test_regex('a([bc])*\1', 'abc', 'RP');
-- expectNomatch	14.10 RP	{a([bc])\1}	abB
select * from test_regex('a([bc])\1', 'abB', 'RP');
-- expectMatch	14.11 iRP	{a([bc])\1}	abB	abB	b
select * from test_regex('a([bc])\1', 'abB', 'iRP');
-- expectMatch	14.12 RP	{a([bc])\1+}	abbb	abbb	b
select * from test_regex('a([bc])\1+', 'abbb', 'RP');
-- expectMatch	14.13 QRP	"a(\[bc])\\1{3,4}"	abbbb	abbbb	b
select * from test_regex('a([bc])\1{3,4}', 'abbbb', 'QRP');
-- expectNomatch	14.14 QRP	"a(\[bc])\\1{3,4}"	abbb
select * from test_regex('a([bc])\1{3,4}', 'abbb', 'QRP');
-- expectMatch	14.15 RP	{a([bc])\1*}	abbb	abbb	b
select * from test_regex('a([bc])\1*', 'abbb', 'RP');
-- expectMatch	14.16 RP	{a([bc])\1*}	ab	ab	b
select * from test_regex('a([bc])\1*', 'ab', 'RP');
-- expectMatch	14.17 RP	{a([bc])(\1*)}	ab	ab	b	""
select * from test_regex('a([bc])(\1*)', 'ab', 'RP');
-- expectError	14.18 -		{a((b)\1)}	ESUBREG
select * from test_regex('a((b)\1)', '', '-');
-- expectError	14.19 -		{a(b)c\2}	ESUBREG
select * from test_regex('a(b)c\2', '', '-');
-- expectMatch	14.20 bR	{a\(b*\)c\1}	abbcbb	abbcbb	bb
select * from test_regex('a\(b*\)c\1', 'abbcbb', 'bR');
-- expectMatch	14.21 RP	{^([bc])\1*$}	bbb	bbb	b
select * from test_regex('^([bc])\1*$', 'bbb', 'RP');
-- expectMatch	14.22 RP	{^([bc])\1*$}	ccc	ccc	c
select * from test_regex('^([bc])\1*$', 'ccc', 'RP');
-- expectNomatch	14.23 RP	{^([bc])\1*$}	bcb
select * from test_regex('^([bc])\1*$', 'bcb', 'RP');
-- expectMatch	14.24 LRP	{^(\w+)( \1)+$}	{abc abc abc} {abc abc abc} abc { abc}
select * from test_regex('^(\w+)( \1)+$', 'abc abc abc', 'LRP');
-- expectNomatch	14.25 LRP	{^(\w+)( \1)+$}	{abc abd abc}
select * from test_regex('^(\w+)( \1)+$', 'abc abd abc', 'LRP');
-- expectNomatch	14.26 LRP	{^(\w+)( \1)+$}	{abc abc abd}
select * from test_regex('^(\w+)( \1)+$', 'abc abc abd', 'LRP');
-- expectMatch	14.27 RP	{^(.+)( \1)+$}	{abc abc abc} {abc abc abc} abc { abc}
select * from test_regex('^(.+)( \1)+$', 'abc abc abc', 'RP');
-- expectNomatch	14.28 RP	{^(.+)( \1)+$}	{abc abd abc}
select * from test_regex('^(.+)( \1)+$', 'abc abd abc', 'RP');
-- expectNomatch	14.29 RP	{^(.+)( \1)+$}	{abc abc abd}
select * from test_regex('^(.+)( \1)+$', 'abc abc abd', 'RP');
-- expectNomatch	14.30 RP	{^(.)\1|\1.}	{abcdef}
select * from test_regex('^(.)\1|\1.', 'abcdef', 'RP');
-- expectNomatch	14.31 RP	{^((.)\2|..)\2}	{abadef}
select * from test_regex('^((.)\2|..)\2', 'abadef', 'RP');

-- back reference only matches the string, not any constraints
select * from test_regex('(^\w+).*\1', 'abc abc abc', 'LRP');
select * from test_regex('(^\w+\M).*\1', 'abc abcd abd', 'LRP');
select * from test_regex('(\w+(?= )).*\1', 'abc abcd abd', 'HLRP');

-- exercise oversize-regmatch_t-array paths in regexec()
-- (that case is not reachable via test_regex, sadly)
select substring('fffoooooooooooooooooooooooooooooooo', '^(.)\1(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)(.)');
select regexp_split_to_array('abcxxxdefyyyghi', '((.))(\1\2)');

-- doing 15 "octal escapes vs back references"

-- # initial zero is always octal
-- expectMatch	15.1  MP	"a\\010b"	"a\bb"	"a\bb"
select * from test_regex('a\010b', E'a\bb', 'MP');
-- expectMatch	15.2  MP	"a\\0070b"	"a\0070b"	"a\0070b"
select * from test_regex('a\0070b', E'a\0070b', 'MP');
-- expectMatch	15.3  MP	"a\\07b"	"a\007b"	"a\007b"
select * from test_regex('a\07b', E'a\007b', 'MP');
-- expectMatch	15.4  MP	"a(b)(b)(b)(b)(b)(b)(b)(b)(b)(b)\\07c" \
-- 	"abbbbbbbbbb\007c" abbbbbbbbbb\007c b b b b b b b b b b
select * from test_regex('a(b)(b)(b)(b)(b)(b)(b)(b)(b)(b)\07c', E'abbbbbbbbbb\007c', 'MP');
-- # a single digit is always a backref
-- expectError	15.5  -		{a\7b}	ESUBREG
select * from test_regex('a\7b', '', '-');
-- # otherwise it's a backref only if within range (barf!)
-- expectMatch	15.6  MP	"a\\10b"	"a\bb"	"a\bb"
select * from test_regex('a\10b', E'a\bb', 'MP');
-- expectMatch	15.7  MP	{a\101b}	aAb	aAb
select * from test_regex('a\101b', 'aAb', 'MP');
-- expectMatch	15.8  RP	{a(b)(b)(b)(b)(b)(b)(b)(b)(b)(b)\10c} \
-- 	"abbbbbbbbbbbc" abbbbbbbbbbbc b b b b b b b b b b
select * from test_regex('a(b)(b)(b)(b)(b)(b)(b)(b)(b)(b)\10c', 'abbbbbbbbbbbc', 'RP');
-- # but we're fussy about border cases -- guys who want octal should use the zero
-- expectError	15.9  -	{a((((((((((b\10))))))))))c}	ESUBREG
select * from test_regex('a((((((((((b\10))))))))))c', '', '-');
-- # BREs don't have octal, EREs don't have backrefs
-- expectMatch	15.10 MP	"a\\12b"	"a\nb"	"a\nb"
select * from test_regex('a\12b', E'a\nb', 'MP');
-- expectError	15.11 b		{a\12b}		ESUBREG
select * from test_regex('a\12b', '', 'b');
-- expectMatch	15.12 eAS	{a\12b}		a12b	a12b
select * from test_regex('a\12b', 'a12b', 'eAS');
-- expectMatch	15.13 MP	{a\701b}	a\u00381b	a\u00381b
select * from test_regex('a\701b', 'a81b', 'MP');

-- doing 16 "expanded syntax"

-- expectMatch	16.1 xP		"a b c"		"abc"	"abc"
select * from test_regex('a b c', 'abc', 'xP');
-- expectMatch	16.2 xP		"a b #oops\nc\td"	"abcd"	"abcd"
select * from test_regex(E'a b #oops\nc\td', 'abcd', 'xP');
-- expectMatch	16.3 x		"a\\ b\\\tc"	"a b\tc"	"a b\tc"
select * from test_regex(E'a\\ b\\\tc', E'a b\tc', 'x');
-- expectMatch	16.4 xP		"a b\\#c"	"ab#c"	"ab#c"
select * from test_regex('a b\#c', 'ab#c', 'xP');
-- expectMatch	16.5 xP		"a b\[c d]e"	"ab e"	"ab e"
select * from test_regex('a b[c d]e', 'ab e', 'xP');
-- expectMatch	16.6 xP		"a b\[c#d]e"	"ab#e"	"ab#e"
select * from test_regex('a b[c#d]e', 'ab#e', 'xP');
-- expectMatch	16.7 xP		"a b\[c#d]e"	"abde"	"abde"
select * from test_regex('a b[c#d]e', 'abde', 'xP');
-- expectMatch	16.8 xSPB	"ab{ d"		"ab\{d"	"ab\{d"
select * from test_regex('ab{ d', 'ab{d', 'xSPB');
-- expectMatch	16.9 xPQ	"ab{ 1 , 2 }c"	"abc"	"abc"
select * from test_regex('ab{ 1 , 2 }c', 'abc', 'xPQ');

-- doing 17 "misc syntax"

-- expectMatch	17.1 P	a(?#comment)b	ab	ab
select * from test_regex('a(?#comment)b', 'ab', 'P');

-- doing 18 "unmatchable REs"

-- expectNomatch	18.1 I	a^b		ab
select * from test_regex('a^b', 'ab', 'I');

-- doing 19 "case independence"

-- expectMatch	19.1 &i		ab		Ab	Ab
select * from test_regex('ab', 'Ab', 'i');
select * from test_regex('ab', 'Ab', 'ib');
-- expectMatch	19.2 &i		{a[bc]}		aC	aC
select * from test_regex('a[bc]', 'aC', 'i');
select * from test_regex('a[bc]', 'aC', 'ib');
-- expectNomatch	19.3 &i		{a[^bc]}	aB
select * from test_regex('a[^bc]', 'aB', 'i');
select * from test_regex('a[^bc]', 'aB', 'ib');
-- expectMatch	19.4 &iM	{a[b-d]}	aC	aC
select * from test_regex('a[b-d]', 'aC', 'iM');
select * from test_regex('a[b-d]', 'aC', 'iMb');
-- expectNomatch	19.5 &iM	{a[^b-d]}	aC
select * from test_regex('a[^b-d]', 'aC', 'iM');
select * from test_regex('a[^b-d]', 'aC', 'iMb');
-- expectMatch	19.6 &iM	{a[B-Z]}	aC	aC
select * from test_regex('a[B-Z]', 'aC', 'iM');
select * from test_regex('a[B-Z]', 'aC', 'iMb');
-- expectNomatch	19.7 &iM	{a[^B-Z]}	aC
select * from test_regex('a[^B-Z]', 'aC', 'iM');
select * from test_regex('a[^B-Z]', 'aC', 'iMb');

-- doing 20 "directors and embedded options"

-- expectError	20.1  &		***?		BADPAT
select * from test_regex('***?', '', '');
select * from test_regex('***?', '', 'b');
-- expectMatch	20.2  q		***?		***?	***?
select * from test_regex('***?', '***?', 'q');
-- expectMatch	20.3  &P	***=a*b		a*b	a*b
select * from test_regex('***=a*b', 'a*b', 'P');
select * from test_regex('***=a*b', 'a*b', 'Pb');
-- expectMatch	20.4  q		***=a*b		***=a*b	***=a*b
select * from test_regex('***=a*b', '***=a*b', 'q');
-- expectMatch	20.5  bLP	{***:\w+}	ab	ab
select * from test_regex('***:\w+', 'ab', 'bLP');
-- expectMatch	20.6  eLP	{***:\w+}	ab	ab
select * from test_regex('***:\w+', 'ab', 'eLP');
-- expectError	20.7  &		***:***=a*b	BADRPT
select * from test_regex('***:***=a*b', '', '');
select * from test_regex('***:***=a*b', '', 'b');
-- expectMatch	20.8  &P	***:(?b)a+b	a+b	a+b
select * from test_regex('***:(?b)a+b', 'a+b', 'P');
select * from test_regex('***:(?b)a+b', 'a+b', 'Pb');
-- expectMatch	20.9  P		(?b)a+b		a+b	a+b
select * from test_regex('(?b)a+b', 'a+b', 'P');
-- expectError	20.10 e		{(?b)\w+}	BADRPT
select * from test_regex('(?b)\w+', '', 'e');
-- expectMatch	20.11 bAS	{(?b)\w+}	(?b)w+	(?b)w+
select * from test_regex('(?b)\w+', '(?b)w+', 'bAS');
-- expectMatch	20.12 iP	(?c)a		a	a
select * from test_regex('(?c)a', 'a', 'iP');
-- expectNomatch	20.13 iP	(?c)a		A
select * from test_regex('(?c)a', 'A', 'iP');
-- expectMatch	20.14 APS	{(?e)\W+}	WW	WW
select * from test_regex('(?e)\W+', 'WW', 'APS');
-- expectMatch	20.15 P		(?i)a+		Aa	Aa
select * from test_regex('(?i)a+', 'Aa', 'P');
-- expectNomatch	20.16 P		"(?m)a.b"	"a\nb"
select * from test_regex('(?m)a.b', E'a\nb', 'P');
-- expectMatch	20.17 P		"(?m)^b"	"a\nb"	"b"
select * from test_regex('(?m)^b', E'a\nb', 'P');
-- expectNomatch	20.18 P		"(?n)a.b"	"a\nb"
select * from test_regex('(?n)a.b', E'a\nb', 'P');
-- expectMatch	20.19 P		"(?n)^b"	"a\nb"	"b"
select * from test_regex('(?n)^b', E'a\nb', 'P');
-- expectNomatch	20.20 P		"(?p)a.b"	"a\nb"
select * from test_regex('(?p)a.b', E'a\nb', 'P');
-- expectNomatch	20.21 P		"(?p)^b"	"a\nb"
select * from test_regex('(?p)^b', E'a\nb', 'P');
-- expectMatch	20.22 P		(?q)a+b		a+b	a+b
select * from test_regex('(?q)a+b', 'a+b', 'P');
-- expectMatch	20.23 nP	"(?s)a.b"	"a\nb"	"a\nb"
select * from test_regex('(?s)a.b', E'a\nb', 'nP');
-- expectMatch	20.24 xP	"(?t)a b"	"a b"	"a b"
select * from test_regex('(?t)a b', 'a b', 'xP');
-- expectMatch	20.25 P		"(?w)a.b"	"a\nb"	"a\nb"
select * from test_regex('(?w)a.b', E'a\nb', 'P');
-- expectMatch	20.26 P		"(?w)^b"	"a\nb"	"b"
select * from test_regex('(?w)^b', E'a\nb', 'P');
-- expectMatch	20.27 P		"(?x)a b"	"ab"	"ab"
select * from test_regex('(?x)a b', 'ab', 'P');
-- expectError	20.28 -		(?z)ab		BADOPT
select * from test_regex('(?z)ab', '', '-');
-- expectMatch	20.29 P		(?ici)a+	Aa	Aa
select * from test_regex('(?ici)a+', 'Aa', 'P');
-- expectError	20.30 P		(?i)(?q)a+	BADRPT
select * from test_regex('(?i)(?q)a+', '', 'P');
-- expectMatch	20.31 P		(?q)(?i)a+	(?i)a+	(?i)a+
select * from test_regex('(?q)(?i)a+', '(?i)a+', 'P');
-- expectMatch	20.32 P		(?qe)a+		a	a
select * from test_regex('(?qe)a+', 'a', 'P');
-- expectMatch	20.33 xP	"(?q)a b"	"a b"	"a b"
select * from test_regex('(?q)a b', 'a b', 'xP');
-- expectMatch	20.34 P		"(?qx)a b"	"a b"	"a b"
select * from test_regex('(?qx)a b', 'a b', 'P');
-- expectMatch	20.35 P		(?qi)ab		Ab	Ab
select * from test_regex('(?qi)ab', 'Ab', 'P');

-- doing 21 "capturing"

-- expectMatch	21.1  -		a(b)c		abc	abc	b
select * from test_regex('a(b)c', 'abc', '-');
-- expectMatch	21.2  P		a(?:b)c		xabc	abc
select * from test_regex('a(?:b)c', 'xabc', 'P');
-- expectMatch	21.3  -		a((b))c		xabcy	abc	b	b
select * from test_regex('a((b))c', 'xabcy', '-');
-- expectMatch	21.4  P		a(?:(b))c	abcy	abc	b
select * from test_regex('a(?:(b))c', 'abcy', 'P');
-- expectMatch	21.5  P		a((?:b))c	abc	abc	b
select * from test_regex('a((?:b))c', 'abc', 'P');
-- expectMatch	21.6  P		a(?:(?:b))c	abc	abc
select * from test_regex('a(?:(?:b))c', 'abc', 'P');
-- expectIndices	21.7  Q		"a(b){0}c"	ac	{0 1}	{-1 -1}
select * from test_regex('a(b){0}c', 'ac', '0Q');
-- expectMatch	21.8  -		a(b)c(d)e	abcde	abcde	b	d
select * from test_regex('a(b)c(d)e', 'abcde', '-');
-- expectMatch	21.9  -		(b)c(d)e	bcde	bcde	b	d
select * from test_regex('(b)c(d)e', 'bcde', '-');
-- expectMatch	21.10 -		a(b)(d)e	abde	abde	b	d
select * from test_regex('a(b)(d)e', 'abde', '-');
-- expectMatch	21.11 -		a(b)c(d)	abcd	abcd	b	d
select * from test_regex('a(b)c(d)', 'abcd', '-');
-- expectMatch	21.12 -		(ab)(cd)	xabcdy	abcd	ab	cd
select * from test_regex('(ab)(cd)', 'xabcdy', '-');
-- expectMatch	21.13 -		a(b)?c		xabcy	abc	b
select * from test_regex('a(b)?c', 'xabcy', '-');
-- expectIndices	21.14 -		a(b)?c		xacy	{1 2}	{-1 -1}
select * from test_regex('a(b)?c', 'xacy', '0-');
-- expectMatch	21.15 -		a(b)?c(d)?e	xabcdey	abcde	b	d
select * from test_regex('a(b)?c(d)?e', 'xabcdey', '-');
-- expectIndices	21.16 -		a(b)?c(d)?e	xacdey	{1 4}	{-1 -1}	{3 3}
select * from test_regex('a(b)?c(d)?e', 'xacdey', '0-');
-- expectIndices	21.17 -		a(b)?c(d)?e	xabcey	{1 4}	{2 2}	{-1 -1}
select * from test_regex('a(b)?c(d)?e', 'xabcey', '0-');
-- expectIndices	21.18 -		a(b)?c(d)?e	xacey	{1 3}	{-1 -1}	{-1 -1}
select * from test_regex('a(b)?c(d)?e', 'xacey', '0-');
-- expectMatch	21.19 -		a(b)*c		xabcy	abc	b
select * from test_regex('a(b)*c', 'xabcy', '-');
-- expectIndices	21.20 -		a(b)*c		xabbbcy	{1 5}	{4 4}
select * from test_regex('a(b)*c', 'xabbbcy', '0-');
-- expectIndices	21.21 -		a(b)*c		xacy	{1 2}	{-1 -1}
select * from test_regex('a(b)*c', 'xacy', '0-');
-- expectMatch	21.22 -		a(b*)c		xabbbcy	abbbc	bbb
select * from test_regex('a(b*)c', 'xabbbcy', '-');
-- expectMatch	21.23 -		a(b*)c		xacy	ac	""
select * from test_regex('a(b*)c', 'xacy', '-');
-- expectNomatch	21.24 -		a(b)+c		xacy
select * from test_regex('a(b)+c', 'xacy', '-');
-- expectMatch	21.25 -		a(b)+c		xabcy	abc	b
select * from test_regex('a(b)+c', 'xabcy', '-');
-- expectIndices	21.26 -		a(b)+c		xabbbcy	{1 5}	{4 4}
select * from test_regex('a(b)+c', 'xabbbcy', '0-');
-- expectMatch	21.27 -		a(b+)c		xabbbcy	abbbc	bbb
select * from test_regex('a(b+)c', 'xabbbcy', '-');
-- expectIndices	21.28 Q		"a(b){2,3}c"	xabbbcy	{1 5}	{4 4}
select * from test_regex('a(b){2,3}c', 'xabbbcy', '0Q');
-- expectIndices	21.29 Q		"a(b){2,3}c"	xabbcy	{1 4}	{3 3}
select * from test_regex('a(b){2,3}c', 'xabbcy', '0Q');
-- expectNomatch	21.30 Q		"a(b){2,3}c"	xabcy
select * from test_regex('a(b){2,3}c', 'xabcy', 'Q');
-- expectMatch	21.31 LP	"\\y(\\w+)\\y"	"-- abc-"	"abc"	"abc"
select * from test_regex('\y(\w+)\y', '-- abc-', 'LP');
-- expectMatch	21.32 -		a((b|c)d+)+	abacdbd	acdbd	bd	b
select * from test_regex('a((b|c)d+)+', 'abacdbd', '-');
-- expectMatch	21.33 N		(.*).*		abc	abc	abc
select * from test_regex('(.*).*', 'abc', 'N');
-- expectMatch	21.34 N		(a*)*		bc	""	""
select * from test_regex('(a*)*', 'bc', 'N');
-- expectMatch	21.35 M		{ TO (([a-z0-9._]+|"([^"]+|"")+")+)}	{asd TO foo}	{ TO foo} foo o {}
select * from test_regex(' TO (([a-z0-9._]+|"([^"]+|"")+")+)', 'asd TO foo', 'M');
-- expectMatch	21.36 RPQ	((.))(\2){0}	xy	x	x	x	{}
select * from test_regex('((.))(\2){0}', 'xy', 'RPQ');
-- expectMatch	21.37 RP	((.))(\2)	xyy	yy	y	y	y
select * from test_regex('((.))(\2)', 'xyy', 'RP');
-- expectMatch	21.38 oRP	((.))(\2)	xyy	yy	{}	{}	{}
select * from test_regex('((.))(\2)', 'xyy', 'oRP');
-- expectNomatch	21.39 PQR	{(.){0}(\1)}	xxx
select * from test_regex('(.){0}(\1)', 'xxx', 'PQR');
-- expectNomatch	21.40 PQR	{((.)){0}(\2)}	xxx
select * from test_regex('((.)){0}(\2)', 'xxx', 'PQR');
-- expectMatch	21.41 NPQR	{((.)){0}(\2){0}}	xyz	{}	{}	{}	{}
select * from test_regex('((.)){0}(\2){0}', 'xyz', 'NPQR');

-- doing 22 "multicharacter collating elements"
-- # again ugh

-- MCCEs are not implemented in Postgres, so we skip all these tests
-- expectMatch	22.1  &+L	{a[c]e}		ace	ace
-- select * from test_regex('a[c]e', 'ace', '+L');
-- select * from test_regex('a[c]e', 'ace', '+Lb');
-- expectNomatch	22.2  &+IL	{a[c]h}		ach
-- select * from test_regex('a[c]h', 'ach', '+IL');
-- select * from test_regex('a[c]h', 'ach', '+ILb');
-- expectMatch	22.3  &+L	{a[[.ch.]]}	ach	ach
-- select * from test_regex('a[[.ch.]]', 'ach', '+L');
-- select * from test_regex('a[[.ch.]]', 'ach', '+Lb');
-- expectNomatch	22.4  &+L	{a[[.ch.]]}	ace
-- select * from test_regex('a[[.ch.]]', 'ace', '+L');
-- select * from test_regex('a[[.ch.]]', 'ace', '+Lb');
-- expectMatch	22.5  &+L	{a[c[.ch.]]}	ac	ac
-- select * from test_regex('a[c[.ch.]]', 'ac', '+L');
-- select * from test_regex('a[c[.ch.]]', 'ac', '+Lb');
-- expectMatch	22.6  &+L	{a[c[.ch.]]}	ace	ac
-- select * from test_regex('a[c[.ch.]]', 'ace', '+L');
-- select * from test_regex('a[c[.ch.]]', 'ace', '+Lb');
-- expectMatch	22.7  &+L	{a[c[.ch.]]}	ache	ach
-- select * from test_regex('a[c[.ch.]]', 'ache', '+L');
-- select * from test_regex('a[c[.ch.]]', 'ache', '+Lb');
-- expectNomatch	22.8  &+L	{a[^c]e}	ace
-- select * from test_regex('a[^c]e', 'ace', '+L');
-- select * from test_regex('a[^c]e', 'ace', '+Lb');
-- expectMatch	22.9  &+L	{a[^c]e}	abe	abe
-- select * from test_regex('a[^c]e', 'abe', '+L');
-- select * from test_regex('a[^c]e', 'abe', '+Lb');
-- expectMatch	22.10 &+L	{a[^c]e}	ache	ache
-- select * from test_regex('a[^c]e', 'ache', '+L');
-- select * from test_regex('a[^c]e', 'ache', '+Lb');
-- expectNomatch	22.11 &+L	{a[^[.ch.]]}	ach
-- select * from test_regex('a[^[.ch.]]', 'ach', '+L');
-- select * from test_regex('a[^[.ch.]]', 'ach', '+Lb');
-- expectMatch	22.12 &+L	{a[^[.ch.]]}	ace	ac
-- select * from test_regex('a[^[.ch.]]', 'ace', '+L');
-- select * from test_regex('a[^[.ch.]]', 'ace', '+Lb');
-- expectMatch	22.13 &+L	{a[^[.ch.]]}	ac	ac
-- select * from test_regex('a[^[.ch.]]', 'ac', '+L');
-- select * from test_regex('a[^[.ch.]]', 'ac', '+Lb');
-- expectMatch	22.14 &+L	{a[^[.ch.]]}	abe	ab
-- select * from test_regex('a[^[.ch.]]', 'abe', '+L');
-- select * from test_regex('a[^[.ch.]]', 'abe', '+Lb');
-- expectNomatch	22.15 &+L	{a[^c[.ch.]]}	ach
-- select * from test_regex('a[^c[.ch.]]', 'ach', '+L');
-- select * from test_regex('a[^c[.ch.]]', 'ach', '+Lb');
-- expectNomatch	22.16 &+L	{a[^c[.ch.]]}	ace
-- select * from test_regex('a[^c[.ch.]]', 'ace', '+L');
-- select * from test_regex('a[^c[.ch.]]', 'ace', '+Lb');
-- expectNomatch	22.17 &+L	{a[^c[.ch.]]}	ac
-- select * from test_regex('a[^c[.ch.]]', 'ac', '+L');
-- select * from test_regex('a[^c[.ch.]]', 'ac', '+Lb');
-- expectMatch	22.18 &+L	{a[^c[.ch.]]}	abe	ab
-- select * from test_regex('a[^c[.ch.]]', 'abe', '+L');
-- select * from test_regex('a[^c[.ch.]]', 'abe', '+Lb');
-- expectMatch	22.19 &+L	{a[^b]}		ac	ac
-- select * from test_regex('a[^b]', 'ac', '+L');
-- select * from test_regex('a[^b]', 'ac', '+Lb');
-- expectMatch	22.20 &+L	{a[^b]}		ace	ac
-- select * from test_regex('a[^b]', 'ace', '+L');
-- select * from test_regex('a[^b]', 'ace', '+Lb');
-- expectMatch	22.21 &+L	{a[^b]}		ach	ach
-- select * from test_regex('a[^b]', 'ach', '+L');
-- select * from test_regex('a[^b]', 'ach', '+Lb');
-- expectNomatch	22.22 &+L	{a[^b]}		abe
-- select * from test_regex('a[^b]', 'abe', '+L');
-- select * from test_regex('a[^b]', 'abe', '+Lb');

-- doing 23 "lookahead constraints"

-- expectMatch	23.1 HP		a(?=b)b*	ab	ab
select * from test_regex('a(?=b)b*', 'ab', 'HP');
-- expectNomatch	23.2 HP		a(?=b)b*	a
select * from test_regex('a(?=b)b*', 'a', 'HP');
-- expectMatch	23.3 HP		a(?=b)b*(?=c)c*	abc	abc
select * from test_regex('a(?=b)b*(?=c)c*', 'abc', 'HP');
-- expectNomatch	23.4 HP		a(?=b)b*(?=c)c*	ab
select * from test_regex('a(?=b)b*(?=c)c*', 'ab', 'HP');
-- expectNomatch	23.5 HP		a(?!b)b*	ab
select * from test_regex('a(?!b)b*', 'ab', 'HP');
-- expectMatch	23.6 HP		a(?!b)b*	a	a
select * from test_regex('a(?!b)b*', 'a', 'HP');
-- expectMatch	23.7 HP		(?=b)b		b	b
select * from test_regex('(?=b)b', 'b', 'HP');
-- expectNomatch	23.8 HP		(?=b)b		a
select * from test_regex('(?=b)b', 'a', 'HP');
-- expectMatch	23.9 HP		...(?!.)	abcde	cde
select * from test_regex('...(?!.)', 'abcde', 'HP');
-- expectNomatch	23.10 HP	...(?=.)	abc
select * from test_regex('...(?=.)', 'abc', 'HP');

-- Postgres addition: lookbehind constraints

-- expectMatch	23.11 HPN		(?<=a)b*	ab	b
select * from test_regex('(?<=a)b*', 'ab', 'HPN');
-- expectNomatch	23.12 HPN		(?<=a)b*	b
select * from test_regex('(?<=a)b*', 'b', 'HPN');
-- expectMatch	23.13 HP		(?<=a)b*(?<=b)c*	abc	bc
select * from test_regex('(?<=a)b*(?<=b)c*', 'abc', 'HP');
-- expectNomatch	23.14 HP		(?<=a)b*(?<=b)c*	ac
select * from test_regex('(?<=a)b*(?<=b)c*', 'ac', 'HP');
-- expectNomatch	23.15 IHP		a(?<!a)b*	ab
select * from test_regex('a(?<!a)b*', 'ab', 'IHP');
-- expectMatch	23.16 HP		a(?<!b)b*	a	a
select * from test_regex('a(?<!b)b*', 'a', 'HP');
-- expectMatch	23.17 HP		(?<=b)b		bb	b
select * from test_regex('(?<=b)b', 'bb', 'HP');
-- expectNomatch	23.18 HP		(?<=b)b		b
select * from test_regex('(?<=b)b', 'b', 'HP');
-- expectMatch	23.19 HP		(?<=.)..	abcde	bc
select * from test_regex('(?<=.)..', 'abcde', 'HP');
-- expectMatch	23.20 HP		(?<=..)a*	aaabb	a
select * from test_regex('(?<=..)a*', 'aaabb', 'HP');
-- expectMatch	23.21 HP		(?<=..)b*	aaabb	{}
-- Note: empty match here is correct, it matches after the first 2 characters
select * from test_regex('(?<=..)b*', 'aaabb', 'HP');
-- expectMatch	23.22 HP		(?<=..)b+	aaabb	bb
select * from test_regex('(?<=..)b+', 'aaabb', 'HP');

-- doing 24 "non-greedy quantifiers"

-- expectMatch	24.1  PT	ab+?		abb	ab
select * from test_regex('ab+?', 'abb', 'PT');
-- expectMatch	24.2  PT	ab+?c		abbc	abbc
select * from test_regex('ab+?c', 'abbc', 'PT');
-- expectMatch	24.3  PT	ab*?		abb	a
select * from test_regex('ab*?', 'abb', 'PT');
-- expectMatch	24.4  PT	ab*?c		abbc	abbc
select * from test_regex('ab*?c', 'abbc', 'PT');
-- expectMatch	24.5  PT	ab??		ab	a
select * from test_regex('ab??', 'ab', 'PT');
-- expectMatch	24.6  PT	ab??c		abc	abc
select * from test_regex('ab??c', 'abc', 'PT');
-- expectMatch	24.7  PQT	"ab{2,4}?"	abbbb	abb
select * from test_regex('ab{2,4}?', 'abbbb', 'PQT');
-- expectMatch	24.8  PQT	"ab{2,4}?c"	abbbbc	abbbbc
select * from test_regex('ab{2,4}?c', 'abbbbc', 'PQT');
-- expectMatch	24.9  -		3z*		123zzzz456	3zzzz
select * from test_regex('3z*', '123zzzz456', '-');
-- expectMatch	24.10 PT	3z*?		123zzzz456	3
select * from test_regex('3z*?', '123zzzz456', 'PT');
-- expectMatch	24.11 -		z*4		123zzzz456	zzzz4
select * from test_regex('z*4', '123zzzz456', '-');
-- expectMatch	24.12 PT	z*?4		123zzzz456	zzzz4
select * from test_regex('z*?4', '123zzzz456', 'PT');
-- expectMatch	24.13 PT	{^([^/]+?)(?:/([^/]+?))(?:/([^/]+?))?$}	{foo/bar/baz}	{foo/bar/baz} {foo} {bar} {baz}
select * from test_regex('^([^/]+?)(?:/([^/]+?))(?:/([^/]+?))?$', 'foo/bar/baz', 'PT');
-- expectMatch	24.14 PRT	{^(.+?)(?:/(.+?))(?:/(.+?)\3)?$}	{foo/bar/baz/quux}	{foo/bar/baz/quux}	{foo}	{bar/baz/quux}	{}
select * from test_regex('^(.+?)(?:/(.+?))(?:/(.+?)\3)?$', 'foo/bar/baz/quux', 'PRT');

-- doing 25 "mixed quantifiers"
-- # this is very incomplete as yet
-- # should include |

-- expectMatch	25.1 PNT	{^(.*?)(a*)$}	"xyza"	xyza	xyz	a
select * from test_regex('^(.*?)(a*)$', 'xyza', 'PNT');
-- expectMatch	25.2 PNT	{^(.*?)(a*)$}	"xyzaa"	xyzaa	xyz	aa
select * from test_regex('^(.*?)(a*)$', 'xyzaa', 'PNT');
-- expectMatch	25.3 PNT	{^(.*?)(a*)$}	"xyz"	xyz	xyz	""
select * from test_regex('^(.*?)(a*)$', 'xyz', 'PNT');

-- doing 26 "tricky cases"

-- # attempts to trick the matcher into accepting a short match
-- expectMatch	26.1 -		(week|wee)(night|knights) \
-- 	"weeknights" weeknights wee knights
select * from test_regex('(week|wee)(night|knights)', 'weeknights', '-');
-- expectMatch	26.2 RP		{a(bc*).*\1}	abccbccb abccbccb	b
select * from test_regex('a(bc*).*\1', 'abccbccb', 'RP');
-- expectMatch	26.3 -		{a(b.[bc]*)+}	abcbd	abcbd	bd
select * from test_regex('a(b.[bc]*)+', 'abcbd', '-');

-- doing 27 "implementation misc."

-- # duplicate arcs are suppressed
-- expectMatch	27.1 P		a(?:b|b)c	abc	abc
select * from test_regex('a(?:b|b)c', 'abc', 'P');
-- # make color/subcolor relationship go back and forth
-- expectMatch	27.2 &		{[ab][ab][ab]}	aba	aba
select * from test_regex('[ab][ab][ab]', 'aba', '');
select * from test_regex('[ab][ab][ab]', 'aba', 'b');
-- expectMatch	27.3 &		{[ab][ab][ab][ab][ab][ab][ab]} \
-- 	"abababa" abababa
select * from test_regex('[ab][ab][ab][ab][ab][ab][ab]', 'abababa', '');
select * from test_regex('[ab][ab][ab][ab][ab][ab][ab]', 'abababa', 'b');

-- doing 28 "boundary busters etc."

-- # color-descriptor allocation changes at 10
-- expectMatch	28.1 &		abcdefghijkl	"abcdefghijkl"	abcdefghijkl
select * from test_regex('abcdefghijkl', 'abcdefghijkl', '');
select * from test_regex('abcdefghijkl', 'abcdefghijkl', 'b');
-- # so does arc allocation
-- expectMatch	28.2 P		a(?:b|c|d|e|f|g|h|i|j|k|l|m)n	"agn"	agn
select * from test_regex('a(?:b|c|d|e|f|g|h|i|j|k|l|m)n', 'agn', 'P');
-- # subexpression tracking also at 10
-- expectMatch	28.3 -		a(((((((((((((b)))))))))))))c \
-- 	"abc" abc b b b b b b b b b b b b b
select * from test_regex('a(((((((((((((b)))))))))))))c', 'abc', '-');
-- # state-set handling changes slightly at unsigned size (might be 64...)
-- # (also stresses arc allocation)
-- expectMatch	28.4  Q		"ab{1,100}c"	abbc	abbc
select * from test_regex('ab{1,100}c', 'abbc', 'Q');
-- expectMatch	28.5  Q		"ab{1,100}c" \
-- 	"abbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbc" \
-- 	abbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbc
select * from test_regex('ab{1,100}c', 'abbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbc', 'Q');
-- expectMatch	28.6  Q		"ab{1,100}c" \
-- 	"abbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbc"\
-- 	abbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbc
select * from test_regex('ab{1,100}c', 'abbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbc', 'Q');
-- # force small cache and bust it, several ways
-- expectMatch	28.7  LP	{\w+abcdefgh}	xyzabcdefgh	xyzabcdefgh
select * from test_regex('\w+abcdefgh', 'xyzabcdefgh', 'LP');
-- expectMatch	28.8  %LP	{\w+abcdefgh}	xyzabcdefgh	xyzabcdefgh
select * from test_regex('\w+abcdefgh', 'xyzabcdefgh', '%LP');
-- expectMatch	28.9  %LP	{\w+abcdefghijklmnopqrst} \
-- 	"xyzabcdefghijklmnopqrst" xyzabcdefghijklmnopqrst
select * from test_regex('\w+abcdefghijklmnopqrst', 'xyzabcdefghijklmnopqrst', '%LP');
-- expectIndices	28.10 %LP	{\w+(abcdefgh)?} xyz	{0 2}	{-1 -1}
select * from test_regex('\w+(abcdefgh)?', 'xyz', '0%LP');
-- expectIndices	28.11 %LP	{\w+(abcdefgh)?} xyzabcdefg	{0 9}	{-1 -1}
select * from test_regex('\w+(abcdefgh)?', 'xyzabcdefg', '0%LP');
-- expectIndices	28.12 %LP	{\w+(abcdefghijklmnopqrst)?} \
-- 	"xyzabcdefghijklmnopqrs" {0 21} {-1 -1}
select * from test_regex('\w+(abcdefghijklmnopqrst)?', 'xyzabcdefghijklmnopqrs', '0%LP');

-- doing 29 "incomplete matches"

-- expectPartial		29.1  t		def	abc	{3 2}	""
select * from test_regex('def', 'abc', '0!t');
-- expectPartial		29.2  t		bcd	abc	{1 2}	""
select * from test_regex('bcd', 'abc', '0!t');
-- expectPartial		29.3  t		abc	abab	{0 3}	""
select * from test_regex('abc', 'abab', '0!t');
-- expectPartial		29.4  t		abc	abdab	{3 4}	""
select * from test_regex('abc', 'abdab', '0!t');
-- expectIndices		29.5  t		abc	abc	{0 2}	{0 2}
select * from test_regex('abc', 'abc', '0t');
-- expectIndices		29.6  t		abc	xyabc	{2 4}	{2 4}
select * from test_regex('abc', 'xyabc', '0t');
-- expectPartial		29.7  t		abc+	xyab	{2 3}	""
select * from test_regex('abc+', 'xyab', '0!t');
-- expectIndices		29.8  t		abc+	xyabc	{2 4}	{2 4}
select * from test_regex('abc+', 'xyabc', '0t');
-- knownBug expectIndices	29.9  t		abc+	xyabcd	{2 4}	{6 5}
select * from test_regex('abc+', 'xyabcd', '0t');
-- expectIndices		29.10 t		abc+	xyabcdd	{2 4}	{7 6}
select * from test_regex('abc+', 'xyabcdd', '0t');
-- expectPartial		29.11 tPT	abc+?	xyab	{2 3}	""
select * from test_regex('abc+?', 'xyab', '0!tPT');
-- # the retain numbers in these two may look wrong, but they aren't
-- expectIndices		29.12 tPT	abc+?	xyabc	{2 4}	{5 4}
select * from test_regex('abc+?', 'xyabc', '0tPT');
-- expectIndices		29.13 tPT	abc+?	xyabcc	{2 4}	{6 5}
select * from test_regex('abc+?', 'xyabcc', '0tPT');
-- expectIndices		29.14 tPT	abc+?	xyabcd	{2 4}	{6 5}
select * from test_regex('abc+?', 'xyabcd', '0tPT');
-- expectIndices		29.15 tPT	abc+?	xyabcdd	{2 4}	{7 6}
select * from test_regex('abc+?', 'xyabcdd', '0tPT');
-- expectIndices		29.16 t		abcd|bc	xyabc	{3 4}	{2 4}
select * from test_regex('abcd|bc', 'xyabc', '0t');
-- expectPartial		29.17 tn	.*k	"xx\nyyy"	{3 5}	""
select * from test_regex('.*k', E'xx\nyyy', '0!tn');

-- doing 30 "misc. oddities and old bugs"

-- expectError	30.1 &		***	BADRPT
select * from test_regex('***', '', '');
select * from test_regex('***', '', 'b');
-- expectMatch	30.2 N		a?b*	abb	abb
select * from test_regex('a?b*', 'abb', 'N');
-- expectMatch	30.3 N		a?b*	bb	bb
select * from test_regex('a?b*', 'bb', 'N');
-- expectMatch	30.4 &		a*b	aab	aab
select * from test_regex('a*b', 'aab', '');
select * from test_regex('a*b', 'aab', 'b');
-- expectMatch	30.5 &		^a*b	aaaab	aaaab
select * from test_regex('^a*b', 'aaaab', '');
select * from test_regex('^a*b', 'aaaab', 'b');
-- expectMatch	30.6 &M		{[0-6][1-2][0-3][0-6][1-6][0-6]} \
-- 	"010010" 010010
select * from test_regex('[0-6][1-2][0-3][0-6][1-6][0-6]', '010010', 'M');
select * from test_regex('[0-6][1-2][0-3][0-6][1-6][0-6]', '010010', 'Mb');
-- # temporary REG_BOSONLY kludge
-- expectMatch	30.7 s		abc	abcd	abc
select * from test_regex('abc', 'abcd', 's');
-- expectNomatch	30.8 s		abc	xabcd
select * from test_regex('abc', 'xabcd', 's');
-- # back to normal stuff
-- expectMatch	30.9 HLP	{(?n)^(?![t#])\S+} \
-- 	"tk\n\n#\n#\nit0"	it0
select * from test_regex('(?n)^(?![t#])\S+', E'tk\n\n#\n#\nit0', 'HLP');

-- # Now for tests *not* written by Henry Spencer

-- # Tests resulting from bugs reported by users
-- test reg-31.1 {[[:xdigit:]] behaves correctly when followed by [[:space:]]} {
--     set str {2:::DebugWin32}
--     set re {([[:xdigit:]])([[:space:]]*)}
--     list [regexp $re $str match xdigit spaces] $match $xdigit $spaces
--     # Code used to produce {1 2:::DebugWin32 2 :::DebugWin32} !!!
-- } {1 2 2 {}}
select * from test_regex('([[:xdigit:]])([[:space:]]*)', '2:::DebugWin32', 'L');

-- test reg-32.1 {canmatch functionality -- at end} testregexp {
--     set pat {blah}
--     set line "asd asd"
--     # can match at the final d, if '%' follows
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 7}
select * from test_regex('blah', 'asd asd', 'c');

-- test reg-32.2 {canmatch functionality -- at end} testregexp {
--     set pat {s%$}
--     set line "asd asd"
--     # can only match after the end of the string
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 7}
select * from test_regex('s%$', 'asd asd', 'c');

-- test reg-32.3 {canmatch functionality -- not last char} testregexp {
--     set pat {[^d]%$}
--     set line "asd asd"
--     # can only match after the end of the string
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 7}
select * from test_regex('[^d]%$', 'asd asd', 'c');

-- test reg-32.3.1 {canmatch functionality -- no match} testregexp {
--     set pat {\Zx}
--     set line "asd asd"
--     # can match the last char, if followed by x
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 -1}
select * from test_regex('\Zx', 'asd asd', 'cIP');

-- test reg-32.4 {canmatch functionality -- last char} {knownBug testregexp} {
--     set pat {.x}
--     set line "asd asd"
--     # can match the last char, if followed by x
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 6}
select * from test_regex('.x', 'asd asd', 'c');

-- test reg-32.4.1 {canmatch functionality -- last char} {knownBug testregexp} {
--     set pat {.x$}
--     set line "asd asd"
--     # can match the last char, if followed by x
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 6}
select * from test_regex('.x$', 'asd asd', 'c');

-- test reg-32.5 {canmatch functionality -- last char} {knownBug testregexp} {
--     set pat {.[^d]x$}
--     set line "asd asd"
--     # can match the last char, if followed by not-d and x.
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 6}
select * from test_regex('.[^d]x$', 'asd asd', 'c');

-- test reg-32.6 {canmatch functionality -- last char} {knownBug testregexp} {
--     set pat {[^a]%[^\r\n]*$}
--     set line "asd asd"
--     # can match at the final d, if '%' follows
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 6}
select * from test_regex('[^a]%[^\r\n]*$', 'asd asd', 'cEP');

-- test reg-32.7 {canmatch functionality -- last char} {knownBug testregexp} {
--     set pat {[^a]%$}
--     set line "asd asd"
--     # can match at the final d, if '%' follows
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 6}
select * from test_regex('[^a]%$', 'asd asd', 'c');

-- test reg-32.8 {canmatch functionality -- last char} {knownBug testregexp} {
--     set pat {[^x]%$}
--     set line "asd asd"
--     # can match at the final d, if '%' follows
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 6}
select * from test_regex('[^x]%$', 'asd asd', 'c');

-- test reg-32.9 {canmatch functionality -- more complex case} {knownBug testregexp} {
--     set pat {((\B\B|\Bh+line)[ \t]*|[^\B]%[^\r\n]*)$}
--     set line "asd asd"
--     # can match at the final d, if '%' follows
--     set res [testregexp -xflags -- c $pat $line resvar]
--     lappend res $resvar
-- } {0 6}
select * from test_regex('((\B\B|\Bh+line)[ \t]*|[^\B]%[^\r\n]*)$', 'asd asd', 'cEP');

-- # Tests reg-33.*: Checks for bug fixes

-- test reg-33.1 {Bug 230589} {
--     regexp {[ ]*(^|[^%])%V} "*%V2" m s
-- } 1
select * from test_regex('[ ]*(^|[^%])%V', '*%V2', '-');

-- test reg-33.2 {Bug 504785} {
--     regexp -inline {([^_.]*)([^.]*)\.(..)(.).*} bbcos_001_c01.q1la
-- } {bbcos_001_c01.q1la bbcos _001_c01 q1 l}
select * from test_regex('([^_.]*)([^.]*)\.(..)(.).*', 'bbcos_001_c01.q1la', '-');

-- test reg-33.3 {Bug 505048} {
--     regexp {\A\s*[^<]*\s*<([^>]+)>} a<a>
-- } 1
select * from test_regex('\A\s*[^<]*\s*<([^>]+)>', 'a<a>', 'LP');

-- test reg-33.4 {Bug 505048} {
--     regexp {\A\s*([^b]*)b} ab
-- } 1
select * from test_regex('\A\s*([^b]*)b', 'ab', 'LP');

-- test reg-33.5 {Bug 505048} {
--     regexp {\A\s*[^b]*(b)} ab
-- } 1
select * from test_regex('\A\s*[^b]*(b)', 'ab', 'LP');

-- test reg-33.6 {Bug 505048} {
--     regexp {\A(\s*)[^b]*(b)} ab
-- } 1
select * from test_regex('\A(\s*)[^b]*(b)', 'ab', 'LP');

-- test reg-33.7 {Bug 505048} {
--     regexp {\A\s*[^b]*b} ab
-- } 1
select * from test_regex('\A\s*[^b]*b', 'ab', 'LP');

-- test reg-33.8 {Bug 505048} {
--     regexp -inline {\A\s*[^b]*b} ab
-- } ab
select * from test_regex('\A\s*[^b]*b', 'ab', 'LP');

-- test reg-33.9 {Bug 505048} {
--     regexp -indices -inline {\A\s*[^b]*b} ab
-- } {{0 1}}
select * from test_regex('\A\s*[^b]*b', 'ab', '0LP');

-- test reg-33.10 {Bug 840258} -body {
--     regsub {(^|\n)+\.*b} \n.b {} tmp
-- } -cleanup {
--     unset tmp
-- } -result 1
select * from test_regex('(^|\n)+\.*b', E'\n.b', 'P');

-- test reg-33.11 {Bug 840258} -body {
--     regsub {(^|[\n\r]+)\.*\?<.*?(\n|\r)+} \
-- 	    "TQ\r\n.?<5000267>Test already stopped\r\n" {} tmp
-- } -cleanup {
--     unset tmp
-- } -result 1
select * from test_regex('(^|[\n\r]+)\.*\?<.*?(\n|\r)+', E'TQ\r\n.?<5000267>Test already stopped\r\n', 'EP');

-- test reg-33.12 {Bug 1810264 - bad read} {
--     regexp {\3161573148} {\3161573148}
-- } 0
select * from test_regex('\3161573148', '\3161573148', 'MP');

-- test reg-33.13 {Bug 1810264 - infinite loop} {
--     regexp {($|^)*} {x}
-- } 1
select * from test_regex('($|^)*', 'x', 'N');

-- # Some environments have small default stack sizes. [Bug 1905562]
-- test reg-33.14 {Bug 1810264 - super-expensive expression} nonPortable {
--     regexp {(x{200}){200}$y} {x}
-- } 0
-- This might or might not work depending on platform, so skip it
-- select * from test_regex('(x{200}){200}$y', 'x', 'IQ');

-- test reg-33.15.1 {Bug 3603557 - an "in the wild" RE} {
--     lindex [regexp -expanded -about {
-- 	^TETRA_MODE_CMD				# Message Type
-- 	([[:blank:]]+)				# Pad
-- 	(ETS_1_1|ETS_1_2|ETS_2_2)		# SystemCode
-- 	([[:blank:]]+)				# Pad
-- 	(CONTINUOUS|CARRIER|MCCH|TRAFFIC)	# SharingMode
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,2})			# ColourCode
-- 	([[:blank:]]+)				# Pad
-- 	(1|2|3|4|6|9|12|18)			# TSReservedFrames
-- 	([[:blank:]]+)				# Pad
-- 	(PASS|TRUE|FAIL|FALSE)			# UPlaneDTX
-- 	([[:blank:]]+)				# Pad
-- 	(PASS|TRUE|FAIL|FALSE)			# Frame18Extension
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,4})			# MCC
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,5})			# MNC
-- 	([[:blank:]]+)				# Pad
-- 	(BOTH|BCAST|ENQRY|NONE)			# NbrCellBcast
-- 	([[:blank:]]+)				# Pad
-- 	(UNKNOWN|LOW|MEDIUM|HIGH)		# CellServiceLevel
-- 	([[:blank:]]+)				# Pad
-- 	(PASS|TRUE|FAIL|FALSE)			# LateEntryInfo
-- 	([[:blank:]]+)				# Pad
-- 	(300|400)				# FrequencyBand
-- 	([[:blank:]]+)				# Pad
-- 	(NORMAL|REVERSE)			# ReverseOperation
-- 	([[:blank:]]+)				# Pad
-- 	(NONE|\+6\.25|\-6\.25|\+12\.5)		# Offset
-- 	([[:blank:]]+)				# Pad
-- 	(10)					# DuplexSpacing
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,4})			# MainCarrierNr
-- 	([[:blank:]]+)				# Pad
-- 	(0|1|2|3)				# NrCSCCH
-- 	([[:blank:]]+)				# Pad
-- 	(15|20|25|30|35|40|45)			# MSTxPwrMax
-- 	([[:blank:]]+)				# Pad
-- 	(\-125|\-120|\-115|\-110|\-105|\-100|\-95|\-90|\-85|\-80|\-75|\-70|\-65|\-60|\-55|\-50)
-- 						# RxLevAccessMin
-- 	([[:blank:]]+)				# Pad
-- 	(\-53|\-51|\-49|\-47|\-45|\-43|\-41|\-39|\-37|\-35|\-33|\-31|\-29|\-27|\-25|\-23)
-- 						# AccessParameter
-- 	([[:blank:]]+)				# Pad
-- 	(DISABLE|[[:digit:]]{3,4})		# RadioDLTimeout
-- 	([[:blank:]]+)				# Pad
-- 	(\-[[:digit:]]{2,3})			# RSSIThreshold
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,5})			# CCKIdSCKVerNr
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,5})			# LocationArea
-- 	([[:blank:]]+)				# Pad
-- 	([(1|0)]{16})				# SubscriberClass
-- 	([[:blank:]]+)				# Pad
-- 	([(1|0)]{12})				# BSServiceDetails
-- 	([[:blank:]]+)				# Pad
-- 	(RANDOMIZE|IMMEDIATE|[[:digit:]]{1,2})	# IMM
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,2})			# WT
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,2})			# Nu
-- 	([[:blank:]]+)				# Pad
-- 	([0-1])					# FrameLngFctr
-- 	([[:blank:]]+)				# Pad
-- 	([[:digit:]]{1,2})			# TSPtr
-- 	([[:blank:]]+)				# Pad
-- 	([0-7])					# MinPriority
-- 	([[:blank:]]+)				# Pad
-- 	(PASS|TRUE|FAIL|FALSE)			# ExtdSrvcsEnabled
-- 	([[:blank:]]+)				# Pad
-- 	(.*)					# ConditionalFields
--     }] 0
-- } 68
select * from test_regex($$
	^TETRA_MODE_CMD				# Message Type
	([[:blank:]]+)				# Pad
	(ETS_1_1|ETS_1_2|ETS_2_2)		# SystemCode
	([[:blank:]]+)				# Pad
	(CONTINUOUS|CARRIER|MCCH|TRAFFIC)	# SharingMode
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,2})			# ColourCode
	([[:blank:]]+)				# Pad
	(1|2|3|4|6|9|12|18)			# TSReservedFrames
	([[:blank:]]+)				# Pad
	(PASS|TRUE|FAIL|FALSE)			# UPlaneDTX
	([[:blank:]]+)				# Pad
	(PASS|TRUE|FAIL|FALSE)			# Frame18Extension
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,4})			# MCC
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,5})			# MNC
	([[:blank:]]+)				# Pad
	(BOTH|BCAST|ENQRY|NONE)			# NbrCellBcast
	([[:blank:]]+)				# Pad
	(UNKNOWN|LOW|MEDIUM|HIGH)		# CellServiceLevel
	([[:blank:]]+)				# Pad
	(PASS|TRUE|FAIL|FALSE)			# LateEntryInfo
	([[:blank:]]+)				# Pad
	(300|400)				# FrequencyBand
	([[:blank:]]+)				# Pad
	(NORMAL|REVERSE)			# ReverseOperation
	([[:blank:]]+)				# Pad
	(NONE|\+6\.25|\-6\.25|\+12\.5)		# Offset
	([[:blank:]]+)				# Pad
	(10)					# DuplexSpacing
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,4})			# MainCarrierNr
	([[:blank:]]+)				# Pad
	(0|1|2|3)				# NrCSCCH
	([[:blank:]]+)				# Pad
	(15|20|25|30|35|40|45)			# MSTxPwrMax
	([[:blank:]]+)				# Pad
	(\-125|\-120|\-115|\-110|\-105|\-100|\-95|\-90|\-85|\-80|\-75|\-70|\-65|\-60|\-55|\-50)
						# RxLevAccessMin
	([[:blank:]]+)				# Pad
	(\-53|\-51|\-49|\-47|\-45|\-43|\-41|\-39|\-37|\-35|\-33|\-31|\-29|\-27|\-25|\-23)
						# AccessParameter
	([[:blank:]]+)				# Pad
	(DISABLE|[[:digit:]]{3,4})		# RadioDLTimeout
	([[:blank:]]+)				# Pad
	(\-[[:digit:]]{2,3})			# RSSIThreshold
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,5})			# CCKIdSCKVerNr
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,5})			# LocationArea
	([[:blank:]]+)				# Pad
	([(1|0)]{16})				# SubscriberClass
	([[:blank:]]+)				# Pad
	([(1|0)]{12})				# BSServiceDetails
	([[:blank:]]+)				# Pad
	(RANDOMIZE|IMMEDIATE|[[:digit:]]{1,2})	# IMM
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,2})			# WT
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,2})			# Nu
	([[:blank:]]+)				# Pad
	([0-1])					# FrameLngFctr
	([[:blank:]]+)				# Pad
	([[:digit:]]{1,2})			# TSPtr
	([[:blank:]]+)				# Pad
	([0-7])					# MinPriority
	([[:blank:]]+)				# Pad
	(PASS|TRUE|FAIL|FALSE)			# ExtdSrvcsEnabled
	([[:blank:]]+)				# Pad
	(.*)					# ConditionalFields
    $$, '', 'xLMPQ');

-- test reg-33.16.1 {Bug [8d2c0da36d]- another "in the wild" RE} {
--     lindex [regexp -about "^MRK:client1: =1339 14HKelly Talisman 10011000 (\[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]*) \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 8 0 8 0 0 0 77 77 1 1 2 0 11 { 1 3 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 13HC6 My Creator 2 3 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 31HC7 Slightly offensive name, huh 3 8 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 23HE-mail:kelly@hotbox.com 4 9 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 17Hcompface must die 5 10 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 0 3HAir 6 12 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 14HPGP public key 7 13 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 16Hkelly@hotbox.com 8 30 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 0 12H2 text/plain 9 30 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 0 13H2 x-kom/basic 10 33 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 1H0 11 14 8 \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* \[0-9\]* 00000000 1 1H3 }\r?"] 0
-- } 1
select * from test_regex(E'^MRK:client1: =1339 14HKelly Talisman 10011000 ([0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]*) [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 8 0 8 0 0 0 77 77 1 1 2 0 11 { 1 3 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 13HC6 My Creator 2 3 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 31HC7 Slightly offensive name, huh 3 8 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 23HE-mail:kelly@hotbox.com 4 9 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 17Hcompface must die 5 10 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 0 3HAir 6 12 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 14HPGP public key 7 13 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 16Hkelly@hotbox.com 8 30 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 0 12H2 text/plain 9 30 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 0 13H2 x-kom/basic 10 33 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 1H0 11 14 8 [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* [0-9]* 00000000 1 1H3 }\r?', '', 'BMS');

-- test reg-33.15 {constraint fixes} {
--     regexp {(^)+^} x
-- } 1
select * from test_regex('(^)+^', 'x', 'N');

-- test reg-33.16 {constraint fixes} {
--     regexp {($^)+} x
-- } 0
select * from test_regex('($^)+', 'x', 'N');

-- test reg-33.17 {constraint fixes} {
--     regexp {(^$)*} x
-- } 1
select * from test_regex('(^$)*', 'x', 'N');

-- test reg-33.18 {constraint fixes} {
--     regexp {(^(?!aa))+} {aa bb cc}
-- } 0
select * from test_regex('(^(?!aa))+', 'aa bb cc', 'HP');

-- test reg-33.19 {constraint fixes} {
--     regexp {(^(?!aa)(?!bb)(?!cc))+} {aa x}
-- } 0
select * from test_regex('(^(?!aa)(?!bb)(?!cc))+', 'aa x', 'HP');

-- test reg-33.20 {constraint fixes} {
--     regexp {(^(?!aa)(?!bb)(?!cc))+} {bb x}
-- } 0
select * from test_regex('(^(?!aa)(?!bb)(?!cc))+', 'bb x', 'HP');

-- test reg-33.21 {constraint fixes} {
--     regexp {(^(?!aa)(?!bb)(?!cc))+} {cc x}
-- } 0
select * from test_regex('(^(?!aa)(?!bb)(?!cc))+', 'cc x', 'HP');

-- test reg-33.22 {constraint fixes} {
--     regexp {(^(?!aa)(?!bb)(?!cc))+} {dd x}
-- } 1
select * from test_regex('(^(?!aa)(?!bb)(?!cc))+', 'dd x', 'HP');

-- test reg-33.23 {} {
--     regexp {abcd(\m)+xyz} x
-- } 0
select * from test_regex('abcd(\m)+xyz', 'x', 'ILP');

-- test reg-33.24 {} {
--     regexp {abcd(\m)+xyz} a
-- } 0
select * from test_regex('abcd(\m)+xyz', 'a', 'ILP');

-- test reg-33.25 {} {
--     regexp {^abcd*(((((^(a c(e?d)a+|)+|)+|)+|)+|a)+|)} x
-- } 0
select * from test_regex('^abcd*(((((^(a c(e?d)a+|)+|)+|)+|)+|a)+|)', 'x', 'S');

-- test reg-33.26 {} {
--     regexp {a^(^)bcd*xy(((((($a+|)+|)+|)+$|)+|)+|)^$} x
-- } 0
select * from test_regex('a^(^)bcd*xy(((((($a+|)+|)+|)+$|)+|)+|)^$', 'x', 'IS');

-- test reg-33.27 {} {
--     regexp {xyz(\Y\Y)+} x
-- } 0
select * from test_regex('xyz(\Y\Y)+', 'x', 'LP');

-- test reg-33.28 {} {
--     regexp {x|(?:\M)+} x
-- } 1
select * from test_regex('x|(?:\M)+', 'x', 'LNP');

-- test reg-33.29 {} {
--     # This is near the limits of the RE engine
--     regexp [string repeat x*y*z* 480] x
-- } 1
-- The runtime cost of this seems out of proportion to the value,
-- so for Postgres purposes reduce the repeat to 200x
select * from test_regex(repeat('x*y*z*', 200), 'x', 'N');

-- test reg-33.30 {Bug 1080042} {
--     regexp {(\Y)+} foo
-- } 1
select * from test_regex('(\Y)+', 'foo', 'LNP');


-- and now, tests not from either Spencer or the Tcl project

-- These cases exercise additional code paths in pushfwd()/push()/combine()
select * from test_regex('a\Y(?=45)', 'a45', 'HLP');
select * from test_regex('a(?=.)c', 'ac', 'HP');
select * from test_regex('a(?=.).*(?=3)3*', 'azz33', 'HP');
select * from test_regex('a(?=\w)\w*(?=.).*', 'az3%', 'HLP');

-- These exercise the bulk-arc-movement paths in moveins() and moveouts();
-- you may need to make them longer if you change BULK_ARC_OP_USE_SORT()
select * from test_regex('ABCDEFGHIJKLMNOPQRSTUVWXYZ(?:\w|a|b|c|d|e|f|0|1|2|3|4|5|6|Q)',
                         'ABCDEFGHIJKLMNOPQRSTUVWXYZ3', 'LP');
select * from test_regex('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789(\Y\Y)+',
                         'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789Z', 'LP');
select * from test_regex('((x|xabcdefghijklmnopqrstuvwxyz0123456789)x*|[^y]z)$',
                         'az', '');
