--
-- Tests for the planner's "equivalence class" mechanism
--

-- One thing that's not tested well during normal querying is the logic
-- for handling "broken" ECs.  This is because an EC can only become broken
-- if its underlying btree operator family doesn't include a complete set
-- of cross-type equality operators.  There are not (and should not be)
-- any such families built into Postgres; so we have to hack things up
-- to create one.  We do this by making two alias types that are really
-- int8 (so we need no new C code) and adding only some operators for them
-- into the standard integer_ops opfamily.

create type int8alias1;
create function int8alias1in(cstring) returns int8alias1
  strict immutable language internal as 'int8in';
create function int8alias1out(int8alias1) returns cstring
  strict immutable language internal as 'int8out';
create type int8alias1 (
    input = int8alias1in,
    output = int8alias1out,
    like = int8
);

create type int8alias2;
create function int8alias2in(cstring) returns int8alias2
  strict immutable language internal as 'int8in';
create function int8alias2out(int8alias2) returns cstring
  strict immutable language internal as 'int8out';
create type int8alias2 (
    input = int8alias2in,
    output = int8alias2out,
    like = int8
);

create cast (int8 as int8alias1) without function;
create cast (int8 as int8alias2) without function;
create cast (int8alias1 as int8) without function;
create cast (int8alias2 as int8) without function;

create function int8alias1eq(int8alias1, int8alias1) returns bool
  strict immutable language internal as 'int8eq';
create operator = (
    procedure = int8alias1eq,
    leftarg = int8alias1, rightarg = int8alias1,
    commutator = =,
    restrict = eqsel, join = eqjoinsel,
    merges
);
alter operator family integer_ops using btree add
  operator 3 = (int8alias1, int8alias1);

create function int8alias2eq(int8alias2, int8alias2) returns bool
  strict immutable language internal as 'int8eq';
create operator = (
    procedure = int8alias2eq,
    leftarg = int8alias2, rightarg = int8alias2,
    commutator = =,
    restrict = eqsel, join = eqjoinsel,
    merges
);
alter operator family integer_ops using btree add
  operator 3 = (int8alias2, int8alias2);

create function int8alias1eq(int8, int8alias1) returns bool
  strict immutable language internal as 'int8eq';
create operator = (
    procedure = int8alias1eq,
    leftarg = int8, rightarg = int8alias1,
    restrict = eqsel, join = eqjoinsel,
    merges
);
alter operator family integer_ops using btree add
  operator 3 = (int8, int8alias1);

create function int8alias1eq(int8alias1, int8alias2) returns bool
  strict immutable language internal as 'int8eq';
create operator = (
    procedure = int8alias1eq,
    leftarg = int8alias1, rightarg = int8alias2,
    restrict = eqsel, join = eqjoinsel,
    merges
);
alter operator family integer_ops using btree add
  operator 3 = (int8alias1, int8alias2);

create function int8alias1lt(int8alias1, int8alias1) returns bool
  strict immutable language internal as 'int8lt';
create operator < (
    procedure = int8alias1lt,
    leftarg = int8alias1, rightarg = int8alias1
);
alter operator family integer_ops using btree add
  operator 1 < (int8alias1, int8alias1);

create function int8alias1cmp(int8, int8alias1) returns int
  strict immutable language internal as 'btint8cmp';
alter operator family integer_ops using btree add
  function 1 int8alias1cmp (int8, int8alias1);

create table ec0 (ff int8 primary key, f1 int8, f2 int8);
create table ec1 (ff int8 primary key, f1 int8alias1, f2 int8alias2);
create table ec2 (xf int8 primary key, x1 int8alias1, x2 int8alias2);

-- for the moment we only want to look at nestloop plans
set enable_hashjoin = off;
set enable_mergejoin = off;

--
-- Note that for cases where there's a missing operator, we don't care so
-- much whether the plan is ideal as that we don't fail or generate an
-- outright incorrect plan.
--

explain (costs off)
  select * from ec0 where ff = f1 and f1 = '42'::int8;
explain (costs off)
  select * from ec0 where ff = f1 and f1 = '42'::int8alias1;
explain (costs off)
  select * from ec1 where ff = f1 and f1 = '42'::int8alias1;
explain (costs off)
  select * from ec1 where ff = f1 and f1 = '42'::int8alias2;

explain (costs off)
  select * from ec1, ec2 where ff = x1 and ff = '42'::int8;
explain (costs off)
  select * from ec1, ec2 where ff = x1 and ff = '42'::int8alias1;
explain (costs off)
  select * from ec1, ec2 where ff = x1 and '42'::int8 = x1;
explain (costs off)
  select * from ec1, ec2 where ff = x1 and x1 = '42'::int8alias1;
explain (costs off)
  select * from ec1, ec2 where ff = x1 and x1 = '42'::int8alias2;

create unique index ec1_expr1 on ec1((ff + 1));
create unique index ec1_expr2 on ec1((ff + 2 + 1));
create unique index ec1_expr3 on ec1((ff + 3 + 1));
create unique index ec1_expr4 on ec1((ff + 4));

explain (costs off)
  select * from ec1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss1
  where ss1.x = ec1.f1 and ec1.ff = 42::int8;

explain (costs off)
  select * from ec1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss1
  where ss1.x = ec1.f1 and ec1.ff = 42::int8 and ec1.ff = ec1.f1;

explain (costs off)
  select * from ec1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss2
  where ss1.x = ec1.f1 and ss1.x = ss2.x and ec1.ff = 42::int8;

-- let's try that as a mergejoin
set enable_mergejoin = on;
set enable_nestloop = off;

explain (costs off)
  select * from ec1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss2
  where ss1.x = ec1.f1 and ss1.x = ss2.x and ec1.ff = 42::int8;

-- check partially indexed scan
set enable_nestloop = on;
set enable_mergejoin = off;

drop index ec1_expr3;

explain (costs off)
  select * from ec1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss1
  where ss1.x = ec1.f1 and ec1.ff = 42::int8;

-- let's try that as a mergejoin
set enable_mergejoin = on;
set enable_nestloop = off;

explain (costs off)
  select * from ec1,
    (select ff + 1 as x from
       (select ff + 2 as ff from ec1
        union all
        select ff + 3 as ff from ec1) ss0
     union all
     select ff + 4 as x from ec1) as ss1
  where ss1.x = ec1.f1 and ec1.ff = 42::int8;

-- check effects of row-level security
set enable_nestloop = on;
set enable_mergejoin = off;

alter table ec1 enable row level security;
create policy p1 on ec1 using (f1 < '5'::int8alias1);

create user regress_user_ectest;
grant select on ec0 to regress_user_ectest;
grant select on ec1 to regress_user_ectest;

-- without any RLS, we'll treat {a.ff, b.ff, 43} as an EquivalenceClass
explain (costs off)
  select * from ec0 a, ec1 b
  where a.ff = b.ff and a.ff = 43::bigint::int8alias1;

set session authorization regress_user_ectest;

-- with RLS active, the non-leakproof a.ff = 43 clause is not treated
-- as a suitable source for an EquivalenceClass; currently, this is true
-- even though the RLS clause has nothing to do directly with the EC
explain (costs off)
  select * from ec0 a, ec1 b
  where a.ff = b.ff and a.ff = 43::bigint::int8alias1;

reset session authorization;

revoke select on ec0 from regress_user_ectest;
revoke select on ec1 from regress_user_ectest;

drop user regress_user_ectest;

-- check that X=X is converted to X IS NOT NULL when appropriate
explain (costs off)
  select * from tenk1 where unique1 = unique1 and unique2 = unique2;

-- this could be converted, but isn't at present
explain (costs off)
  select * from tenk1 where unique1 = unique1 or unique2 = unique2;

-- check that we recognize equivalence with dummy domains in the way
create temp table undername (f1 name, f2 int);
create temp view overview as
  select f1::information_schema.sql_identifier as sqli, f2 from undername;
explain (costs off)  -- this should not require a sort
  select * from overview where sqli = 'foo' order by sqli;
