-- copied from equivclass.sql
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


-- copied from alter_table.sql
create type ctype as (f1 int, f2 text);

create function same(ctype, ctype) returns boolean language sql
as 'select $1.f1 is not distinct from $2.f1 and $1.f2 is not distinct from $2.f2';

create operator =(procedure = same, leftarg  = ctype, rightarg = ctype);

create operator class ctype_hash_ops
  default for type ctype using hash as
  operator 1 =(ctype, ctype);
