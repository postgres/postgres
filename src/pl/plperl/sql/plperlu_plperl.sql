--
-- Test that recursing between plperl and plperlu doesn't allow plperl to perform unsafe ops
--

-- recurse between a plperl and plperlu function that are identical except that
-- each calls the other. Each also checks if an unsafe opcode can be executed.

CREATE OR REPLACE FUNCTION recurse_plperl(i int) RETURNS SETOF TEXT LANGUAGE plperl
AS $$
	my $i = shift;
	return unless $i > 0;
	return_next "plperl  $i entry: ".((eval "stat;1") ? "ok" : $@);
	return_next $_
		for map { $_->{recurse_plperlu} }
			@{spi_exec_query("select * from recurse_plperlu($i-1)")->{rows}};
	return;
$$;

CREATE OR REPLACE FUNCTION recurse_plperlu(i int) RETURNS SETOF TEXT LANGUAGE plperlu
AS $$
	my $i = shift;
	return unless $i > 0;
	return_next "plperlu $i entry: ".((eval "stat;1") ? "ok" : $@);
	return_next $_
		for map { $_->{recurse_plperl} }
			@{spi_exec_query("select * from recurse_plperl($i-1)")->{rows}};
	return;
$$;

SELECT * FROM recurse_plperl(5);
SELECT * FROM recurse_plperlu(5);

--
-- Make sure we can't use/require things in plperl
--

CREATE OR REPLACE FUNCTION use_plperlu() RETURNS void LANGUAGE plperlu
AS $$
use Errno;
$$;

CREATE OR REPLACE FUNCTION use_plperl() RETURNS void LANGUAGE plperl
AS $$
use Errno;
$$;

-- make sure our overloaded require op gets restored/set correctly
select use_plperlu();

CREATE OR REPLACE FUNCTION use_plperl() RETURNS void LANGUAGE plperl
AS $$
use Errno;
$$;
