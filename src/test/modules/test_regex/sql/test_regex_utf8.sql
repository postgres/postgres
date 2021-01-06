/*
 * This test must be run in a database with UTF-8 encoding,
 * because other encodings don't support all the characters used.
 */

SELECT getdatabaseencoding() <> 'UTF8'
       AS skip_test \gset
\if :skip_test
\quit
\endif

set client_encoding = utf8;

set standard_conforming_strings = on;


-- Run the Tcl test cases that require Unicode

-- expectMatch	9.44 EMP*	{a[\u00fe-\u0507][\u00ff-\u0300]b} \
-- 	"a\u0102\u02ffb"	"a\u0102\u02ffb"
select * from test_regex('a[\u00fe-\u0507][\u00ff-\u0300]b', E'a\u0102\u02ffb', 'EMP*');

-- expectMatch	13.27 P		"a\\U00001234x"	"a\u1234x"	"a\u1234x"
select * from test_regex('a\U00001234x', E'a\u1234x', 'P');
-- expectMatch	13.28 P		{a\U00001234x}	"a\u1234x"	"a\u1234x"
select * from test_regex('a\U00001234x', E'a\u1234x', 'P');
-- expectMatch	13.29 P		"a\\U0001234x"	"a\u1234x"	"a\u1234x"
-- Tcl has relaxed their code to allow 1-8 hex digits, but Postgres hasn't
select * from test_regex('a\U0001234x', E'a\u1234x', 'P');
-- expectMatch	13.30 P		{a\U0001234x}	"a\u1234x"	"a\u1234x"
-- Tcl has relaxed their code to allow 1-8 hex digits, but Postgres hasn't
select * from test_regex('a\U0001234x', E'a\u1234x', 'P');
-- expectMatch	13.31 P		"a\\U000012345x"	"a\u12345x"	"a\u12345x"
select * from test_regex('a\U000012345x', E'a\u12345x', 'P');
-- expectMatch	13.32 P		{a\U000012345x}	"a\u12345x"	"a\u12345x"
select * from test_regex('a\U000012345x', E'a\u12345x', 'P');
-- expectMatch	13.33 P		"a\\U1000000x"	"a\ufffd0x"	"a\ufffd0x"
-- Tcl allows this as a standalone character, but Postgres doesn't
select * from test_regex('a\U1000000x', E'a\ufffd0x', 'P');
-- expectMatch	13.34 P		{a\U1000000x}	"a\ufffd0x"	"a\ufffd0x"
-- Tcl allows this as a standalone character, but Postgres doesn't
select * from test_regex('a\U1000000x', E'a\ufffd0x', 'P');


-- Additional tests, not derived from Tcl

-- Exercise logic around high character ranges a bit more
select * from test_regex('a
  [\u1000-\u1100]*
  [\u3000-\u3100]*
  [\u1234-\u25ff]+
  [\u2000-\u35ff]*
  [\u2600-\u2f00]*
  \u1236\u1236x',
  E'a\u1234\u1236\u1236x', 'xEMP');

select * from test_regex('[[:alnum:]]*[[:upper:]]*[\u1000-\u2000]*\u1237',
  E'\u1500\u1237', 'ELMP');
select * from test_regex('[[:alnum:]]*[[:upper:]]*[\u1000-\u2000]*\u1237',
  E'A\u1239', 'ELMP');
