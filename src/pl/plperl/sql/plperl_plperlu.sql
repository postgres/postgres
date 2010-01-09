-- test plperl/plperlu interaction

CREATE OR REPLACE FUNCTION bar() RETURNS integer AS $$
    #die 'BANG!'; # causes server process to exit(2)
    # alternative - causes server process to exit(255)
    spi_exec_query("invalid sql statement");
$$ language plperl; -- plperl or plperlu
   
CREATE OR REPLACE FUNCTION foo() RETURNS integer AS $$
    spi_exec_query("SELECT * FROM bar()");
    return 1;
$$ LANGUAGE plperlu; -- must be opposite to language of bar
   
SELECT * FROM bar(); -- throws exception normally
SELECT * FROM foo(); -- used to cause backend crash


