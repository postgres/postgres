--
-- Regular expression tests
--

-- Don't want to have to double backslashes in regexes
set standard_conforming_strings = on;

-- Test simple quantified backrefs
select 'bbbbb' ~ '^([bc])\1*$' as t;
select 'ccc' ~ '^([bc])\1*$' as t;
select 'xxx' ~ '^([bc])\1*$' as f;
select 'bbc' ~ '^([bc])\1*$' as f;
select 'b' ~ '^([bc])\1*$' as t;

-- Test quantified backref within a larger expression
select 'abc abc abc' ~ '^(\w+)( \1)+$' as t;
select 'abc abd abc' ~ '^(\w+)( \1)+$' as f;
select 'abc abc abd' ~ '^(\w+)( \1)+$' as f;
select 'abc abc abc' ~ '^(.+)( \1)+$' as t;
select 'abc abd abc' ~ '^(.+)( \1)+$' as f;
select 'abc abc abd' ~ '^(.+)( \1)+$' as f;

-- Test some cases that crashed in 9.2beta1 due to pmatch[] array overrun
select substring('asd TO foo' from ' TO (([a-z0-9._]+|"([^"]+|"")+")+)');
select substring('a' from '((a))+');
select substring('a' from '((a)+)');

-- Test conversion of regex patterns to indexable conditions
explain (costs off) select * from pg_proc where proname ~ 'abc';
explain (costs off) select * from pg_proc where proname ~ '^abc';
explain (costs off) select * from pg_proc where proname ~ '^abc$';
explain (costs off) select * from pg_proc where proname ~ '^abcd*e';
explain (costs off) select * from pg_proc where proname ~ '^abc+d';
explain (costs off) select * from pg_proc where proname ~ '^(abc)(def)';
explain (costs off) select * from pg_proc where proname ~ '^(abc)$';
explain (costs off) select * from pg_proc where proname ~ '^(abc)?d';

-- Test for infinite loop in pullback() (CVE-2007-4772)
select 'a' ~ '($|^)*';

-- Test for infinite loop in fixempties() (Tcl bugs 3604074, 3606683)
select 'a' ~ '((((((a)*)*)*)*)*)*';
select 'a' ~ '((((((a+|)+|)+|)+|)+|)+|)';

-- Test backref in combination with non-greedy quantifier
-- https://core.tcl.tk/tcl/tktview/6585b21ca8fa6f3678d442b97241fdd43dba2ec0
select 'Programmer' ~ '(\w).*?\1' as t;
select regexp_matches('Programmer', '(\w)(.*?\1)', 'g');
