--
-- TEXT
--

SELECT text 'this is a text string' = text 'this is a text string' AS true;

SELECT text 'this is a text string' = text 'this is a text strin' AS false;

CREATE TABLE TEXT_TBL (f1 text);

INSERT INTO TEXT_TBL VALUES ('doh!');
INSERT INTO TEXT_TBL VALUES ('hi de ho neighbor');

SELECT '' AS two, * FROM TEXT_TBL;

-- As of 8.3 we have removed most implicit casts to text, so that for example
-- this no longer works:

select length(42);

-- But as a special exception for usability's sake, we still allow implicit
-- casting to text in concatenations, so long as the other input is text or
-- an unknown literal.  So these work:

select 'four: '::text || 2+2;
select 'four: ' || 2+2;

-- but not this:

select 3 || 4.0;

/*
 * various string functions
 */
select concat('one');
select concat(1,2,3,'hello',true, false, to_date('20100309','YYYYMMDD'));
select concat_ws('#','one');
select concat_ws('#',1,2,3,'hello',true, false, to_date('20100309','YYYYMMDD'));
select concat_ws(',',10,20,null,30);
select concat_ws('',10,20,null,30);
select concat_ws(NULL,10,20,null,30) is null;
select reverse('abcde');
select i, left('ahoj', i), right('ahoj', i) from generate_series(-5, 5) t(i) order by i;
select quote_literal('');
select quote_literal('abc''');
select quote_literal(e'\\');

/*
 * format
 */
select format(NULL);
select format('Hello');
select format('Hello %s', 'World');
select format('Hello %%');
select format('Hello %%%%');
-- should fail
select format('Hello %s %s', 'World');
select format('Hello %s');
select format('Hello %x', 20);
-- check literal and sql identifiers
select format('INSERT INTO %I VALUES(%L,%L)', 'mytab', 10, 'Hello');
select format('%s%s%s','Hello', NULL,'World');
select format('INSERT INTO %I VALUES(%L,%L)', 'mytab', 10, NULL);
select format('INSERT INTO %I VALUES(%L,%L)', 'mytab', NULL, 'Hello');
-- should fail, sql identifier cannot be NULL
select format('INSERT INTO %I VALUES(%L,%L)', NULL, 10, 'Hello');
-- check positional placeholders
select format('%1$s %3$s', 1, 2, 3);
select format('%1$s %12$s', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
-- should fail
select format('%1$s %4$s', 1, 2, 3);
select format('%1$s %13$s', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
select format('%1s', 1);
select format('%1$', 1);
select format('%1$1', 1);
--checkk mix of positional and ordered placeholders
select format('Hello %s %1$s %s', 'World', 'Hello again');
select format('Hello %s %s, %2$s %2$s', 'World', 'Hello again');
