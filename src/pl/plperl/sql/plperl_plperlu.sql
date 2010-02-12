-- test plperl/plperlu interaction

-- the language and call ordering of this test sequence is useful

CREATE OR REPLACE FUNCTION bar() RETURNS integer AS $$
    #die 'BANG!'; # causes server process to exit(2)
    # alternative - causes server process to exit(255)
    spi_exec_query("invalid sql statement");
$$ language plperl; -- compile plperl code
   
CREATE OR REPLACE FUNCTION foo() RETURNS integer AS $$
    spi_exec_query("SELECT * FROM bar()");
    return 1;
$$ LANGUAGE plperlu; -- compile plperlu code
   
SELECT * FROM bar(); -- throws exception normally (running plperl)
SELECT * FROM foo(); -- used to cause backend crash (after switching to plperlu)

-- test redefinition of specific SP switching languages
-- http://archives.postgresql.org/pgsql-bugs/2010-01/msg00116.php

-- plperl first
create or replace function foo(text) returns text language plperl  as 'shift';
select foo('hey');
create or replace function foo(text) returns text language plperlu as 'shift';
select foo('hey');
create or replace function foo(text) returns text language plperl  as 'shift';
select foo('hey');

-- plperlu first
create or replace function bar(text) returns text language plperlu as 'shift';
select bar('hey');
create or replace function bar(text) returns text language plperl  as 'shift';
select bar('hey');
create or replace function bar(text) returns text language plperlu as 'shift';
select bar('hey');

