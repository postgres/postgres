CREATE EXTENSION test_predtest;

-- Make output more legible
\pset expanded on

-- Test data

-- all combinations of four boolean values
create table booleans as
select
  case i%3 when 0 then true when 1 then false else null end as x,
  case (i/3)%3 when 0 then true when 1 then false else null end as y,
  case (i/9)%3 when 0 then true when 1 then false else null end as z,
  case (i/27)%3 when 0 then true when 1 then false else null end as w
from generate_series(0, 3*3*3*3-1) i;

-- all combinations of two integers 0..9, plus null
create table integers as
select
  case i%11 when 10 then null else i%11 end as x,
  case (i/11)%11 when 10 then null else (i/11)%11 end as y
from generate_series(0, 11*11-1) i;

-- and a simple strict function that's opaque to the optimizer
create function strictf(bool, bool) returns bool
language plpgsql as $$begin return $1 and not $2; end$$ strict;

-- Basic proof rules for single boolean variables

select * from test_predtest($$
select x, x
from booleans
$$);

select * from test_predtest($$
select x, not x
from booleans
$$);

select * from test_predtest($$
select not x, x
from booleans
$$);

select * from test_predtest($$
select not x, not x
from booleans
$$);

select * from test_predtest($$
select x is not null, x
from booleans
$$);

select * from test_predtest($$
select x is not null, x is null
from integers
$$);

select * from test_predtest($$
select x is null, x is not null
from integers
$$);

select * from test_predtest($$
select x is not true, x
from booleans
$$);

select * from test_predtest($$
select x, x is not true
from booleans
$$);

select * from test_predtest($$
select x is false, x
from booleans
$$);

select * from test_predtest($$
select x, x is false
from booleans
$$);

select * from test_predtest($$
select x is unknown, x
from booleans
$$);

select * from test_predtest($$
select x, x is unknown
from booleans
$$);

-- Assorted not-so-trivial refutation rules

select * from test_predtest($$
select x is null, x
from booleans
$$);

select * from test_predtest($$
select x, x is null
from booleans
$$);

select * from test_predtest($$
select strictf(x,y), x is null
from booleans
$$);

select * from test_predtest($$
select (x is not null) is not true, x
from booleans
$$);

select * from test_predtest($$
select strictf(x,y), (x is not null) is false
from booleans
$$);

select * from test_predtest($$
select x > y, (y < x) is false
from integers
$$);

-- Tests involving AND/OR constructs

select * from test_predtest($$
select x, x and y
from booleans
$$);

select * from test_predtest($$
select not x, x and y
from booleans
$$);

select * from test_predtest($$
select x, not x and y
from booleans
$$);

select * from test_predtest($$
select x or y, x
from booleans
$$);

select * from test_predtest($$
select x and y, x
from booleans
$$);

select * from test_predtest($$
select x and y, not x
from booleans
$$);

select * from test_predtest($$
select x and y, y and x
from booleans
$$);

select * from test_predtest($$
select not y, y and x
from booleans
$$);

select * from test_predtest($$
select x or y, y or x
from booleans
$$);

select * from test_predtest($$
select x or y or z, x or z
from booleans
$$);

select * from test_predtest($$
select x and z, x and y and z
from booleans
$$);

select * from test_predtest($$
select z or w, x or y
from booleans
$$);

select * from test_predtest($$
select z and w, x or y
from booleans
$$);

select * from test_predtest($$
select x, (x and y) or (x and z)
from booleans
$$);

select * from test_predtest($$
select (x and y) or z, y and x
from booleans
$$);

select * from test_predtest($$
select (not x or not y) and z, y and x
from booleans
$$);

select * from test_predtest($$
select y or x, (x or y) and z
from booleans
$$);

select * from test_predtest($$
select not x and not y, (x or y) and z
from booleans
$$);

-- Tests using btree operator knowledge

select * from test_predtest($$
select x <= y, x < y
from integers
$$);

select * from test_predtest($$
select x <= y, x > y
from integers
$$);

select * from test_predtest($$
select x <= y, y >= x
from integers
$$);

select * from test_predtest($$
select x <= y, y > x and y < x+2
from integers
$$);

select * from test_predtest($$
select x <= 5, x <= 7
from integers
$$);

select * from test_predtest($$
select x <= 5, x > 7
from integers
$$);

select * from test_predtest($$
select x <= 5, 7 > x
from integers
$$);

select * from test_predtest($$
select 5 >= x, 7 > x
from integers
$$);

select * from test_predtest($$
select 5 >= x, x > 7
from integers
$$);

select * from test_predtest($$
select 5 = x, x = 7
from integers
$$);

select * from test_predtest($$
select x is not null, x > 7
from integers
$$);

select * from test_predtest($$
select x is not null, int4lt(x,8)
from integers
$$);

select * from test_predtest($$
select x is null, x > 7
from integers
$$);

select * from test_predtest($$
select x is null, int4lt(x,8)
from integers
$$);

select * from test_predtest($$
select x is not null, x < 'foo'
from (values
  ('aaa'::varchar), ('zzz'::varchar), (null)) as v(x)
$$);

-- Cases using ScalarArrayOpExpr

select * from test_predtest($$
select x <= 5, x in (1,3,5)
from integers
$$);

select * from test_predtest($$
select x <= 5, x in (1,3,5,7)
from integers
$$);

select * from test_predtest($$
select x <= 5, x in (1,3,5,null)
from integers
$$);

select * from test_predtest($$
select x in (null,1,3,5,7), x in (1,3,5)
from integers
$$);

select * from test_predtest($$
select x <= 5, x < all(array[1,3,5])
from integers
$$);

select * from test_predtest($$
select x <= y, x = any(array[1,3,y])
from integers
$$);
