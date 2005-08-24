--
-- checkpoint so that if we have a crash in the tests, replay of the
-- just-completed CREATE DATABASE won't discard the core dump file
--
checkpoint;

--
-- Test result value processing
--

CREATE OR REPLACE FUNCTION perl_int(int) RETURNS INTEGER AS $$
return undef;
$$ LANGUAGE plperl;

SELECT perl_int(11);
SELECT * FROM perl_int(42);

CREATE OR REPLACE FUNCTION perl_int(int) RETURNS INTEGER AS $$
return $_[0] + 1;
$$ LANGUAGE plperl;

SELECT perl_int(11);
SELECT * FROM perl_int(42);


CREATE OR REPLACE FUNCTION perl_set_int(int) RETURNS SETOF INTEGER AS $$
return undef;
$$ LANGUAGE plperl;

SELECT perl_set_int(5);
SELECT * FROM perl_set_int(5);

CREATE OR REPLACE FUNCTION perl_set_int(int) RETURNS SETOF INTEGER AS $$
return [0..$_[0]];
$$ LANGUAGE plperl;

SELECT perl_set_int(5);
SELECT * FROM perl_set_int(5);


CREATE TYPE testrowperl AS (f1 integer, f2 text, f3 text);

CREATE OR REPLACE FUNCTION perl_row() RETURNS testrowperl AS $$
    return undef;
$$ LANGUAGE plperl;

SELECT perl_row();
SELECT * FROM perl_row();

CREATE OR REPLACE FUNCTION perl_row() RETURNS testrowperl AS $$
    return {f2 => 'hello', f1 => 1, f3 => 'world'};
$$ LANGUAGE plperl;

SELECT perl_row();
SELECT * FROM perl_row();


CREATE OR REPLACE FUNCTION perl_set() RETURNS SETOF testrowperl AS $$
    return undef;
$$  LANGUAGE plperl;

SELECT perl_set();
SELECT * FROM perl_set();

CREATE OR REPLACE FUNCTION perl_set() RETURNS SETOF testrowperl AS $$
    return [
        { f1 => 1, f2 => 'Hello', f3 =>  'World' },
        undef,
        { f1 => 3, f2 => 'Hello', f3 =>  'PL/Perl' }
    ];
$$  LANGUAGE plperl;

SELECT perl_set();
SELECT * FROM perl_set();

CREATE OR REPLACE FUNCTION perl_set() RETURNS SETOF testrowperl AS $$
    return [
        { f1 => 1, f2 => 'Hello', f3 =>  'World' },
        { f1 => 2, f2 => 'Hello', f3 =>  'PostgreSQL' },
        { f1 => 3, f2 => 'Hello', f3 =>  'PL/Perl' }
    ];
$$  LANGUAGE plperl;

SELECT perl_set();
SELECT * FROM perl_set();



CREATE OR REPLACE FUNCTION perl_record() RETURNS record AS $$
    return undef;
$$ LANGUAGE plperl;

SELECT perl_record();
SELECT * FROM perl_record();
SELECT * FROM perl_record() AS (f1 integer, f2 text, f3 text);

CREATE OR REPLACE FUNCTION perl_record() RETURNS record AS $$
    return {f2 => 'hello', f1 => 1, f3 => 'world'};
$$ LANGUAGE plperl;

SELECT perl_record();
SELECT * FROM perl_record();
SELECT * FROM perl_record() AS (f1 integer, f2 text, f3 text);


CREATE OR REPLACE FUNCTION perl_record_set() RETURNS SETOF record AS $$
    return undef;
$$  LANGUAGE plperl;

SELECT perl_record_set();
SELECT * FROM perl_record_set();
SELECT * FROM perl_record_set() AS (f1 integer, f2 text, f3 text);

CREATE OR REPLACE FUNCTION perl_record_set() RETURNS SETOF record AS $$
    return [
        { f1 => 1, f2 => 'Hello', f3 =>  'World' },
        undef,
        { f1 => 3, f2 => 'Hello', f3 =>  'PL/Perl' }
    ];
$$  LANGUAGE plperl;

SELECT perl_record_set();
SELECT * FROM perl_record_set();
SELECT * FROM perl_record_set() AS (f1 integer, f2 text, f3 text);

CREATE OR REPLACE FUNCTION perl_record_set() RETURNS SETOF record AS $$
    return [
        { f1 => 1, f2 => 'Hello', f3 =>  'World' },
        { f1 => 2, f2 => 'Hello', f3 =>  'PostgreSQL' },
        { f1 => 3, f2 => 'Hello', f3 =>  'PL/Perl' }
    ];
$$  LANGUAGE plperl;

SELECT perl_record_set();
SELECT * FROM perl_record_set();
SELECT * FROM perl_record_set() AS (f1 integer, f2 text, f3 text);

CREATE OR REPLACE FUNCTION
perl_out_params(f1 out integer, f2 out text, f3 out text) AS $$
    return {f2 => 'hello', f1 => 1, f3 => 'world'};
$$ LANGUAGE plperl;

SELECT perl_out_params();
SELECT * FROM perl_out_params();
SELECT (perl_out_params()).f2;

CREATE OR REPLACE FUNCTION
perl_out_params_set(out f1 integer, out f2 text, out f3 text)
RETURNS SETOF record AS $$
    return [
        { f1 => 1, f2 => 'Hello', f3 =>  'World' },
        { f1 => 2, f2 => 'Hello', f3 =>  'PostgreSQL' },
        { f1 => 3, f2 => 'Hello', f3 =>  'PL/Perl' }
    ];
$$  LANGUAGE plperl;

SELECT perl_out_params_set();
SELECT * FROM perl_out_params_set();
SELECT (perl_out_params_set()).f3;

--
-- Check behavior with erroneous return values
--

CREATE TYPE footype AS (x INTEGER, y INTEGER);

CREATE OR REPLACE FUNCTION foo_good() RETURNS SETOF footype AS $$
return [
    {x => 1, y => 2},
    {x => 3, y => 4}
];
$$ LANGUAGE plperl;

SELECT * FROM foo_good();

CREATE OR REPLACE FUNCTION foo_bad() RETURNS footype AS $$
    return {y => 3, z => 4};
$$ LANGUAGE plperl;

SELECT * FROM foo_bad();

CREATE OR REPLACE FUNCTION foo_bad() RETURNS footype AS $$
return 42;
$$ LANGUAGE plperl;

SELECT * FROM foo_bad();

CREATE OR REPLACE FUNCTION foo_bad() RETURNS footype AS $$
return [
    [1, 2],
    [3, 4]
];
$$ LANGUAGE plperl;

SELECT * FROM foo_bad();

CREATE OR REPLACE FUNCTION foo_set_bad() RETURNS SETOF footype AS $$
    return 42;
$$ LANGUAGE plperl;

SELECT * FROM foo_set_bad();

CREATE OR REPLACE FUNCTION foo_set_bad() RETURNS SETOF footype AS $$
    return {y => 3, z => 4};
$$ LANGUAGE plperl;

SELECT * FROM foo_set_bad();

CREATE OR REPLACE FUNCTION foo_set_bad() RETURNS SETOF footype AS $$
return [
    [1, 2],
    [3, 4]
];
$$ LANGUAGE plperl;

SELECT * FROM foo_set_bad();

CREATE OR REPLACE FUNCTION foo_set_bad() RETURNS SETOF footype AS $$
return [
    {y => 3, z => 4}
];
$$ LANGUAGE plperl;

SELECT * FROM foo_set_bad();

--
-- Check passing a tuple argument
--

CREATE OR REPLACE FUNCTION perl_get_field(footype, text) RETURNS integer AS $$
    return $_[0]->{$_[1]};
$$ LANGUAGE plperl;

SELECT perl_get_field((11,12), 'x');
SELECT perl_get_field((11,12), 'y');
SELECT perl_get_field((11,12), 'z');

--
-- Test return_next
--

CREATE OR REPLACE FUNCTION perl_srf_rn() RETURNS SETOF RECORD AS $$
my $i = 0;
for ("World", "PostgreSQL", "PL/Perl") {
    return_next({f1=>++$i, f2=>'Hello', f3=>$_});
}
return;
$$ language plperl;
SELECT * from perl_srf_rn() AS (f1 INTEGER, f2 TEXT, f3 TEXT);

--
-- Test spi_query/spi_fetchrow
--

CREATE OR REPLACE FUNCTION perl_spi_func() RETURNS SETOF INTEGER AS $$
my $x = spi_query("select 1 as a union select 2 as a");
while (defined (my $y = spi_fetchrow($x))) {
    return_next($y->{a});
}
return;
$$ LANGUAGE plperl;
SELECT * from perl_spi_func();


---
--- Test recursion via SPI
---


CREATE OR REPLACE FUNCTION recurse(i int) RETURNS SETOF TEXT LANGUAGE plperl
AS $$

  my $i = shift;
  foreach my $x (1..$i)
  {
    return_next "hello $x";
  }
  if ($i > 2)
  {
    my $z = $i-1;
    my $cursor = spi_query("select * from recurse($z)");
    while (defined(my $row = spi_fetchrow($cursor)))
    {
      return_next "recurse $i: $row->{recurse}";
    }
  }
  return undef;

$$;

SELECT * FROM recurse(2);
SELECT * FROM recurse(3);


---
--- Test arrary return
---
CREATE OR REPLACE FUNCTION  array_of_text() RETURNS TEXT[][] 
LANGUAGE plperl as $$ 
    return [['a"b','c,d'],['e\\f','g']]; 
$$;

SELECT array_of_text(); 
