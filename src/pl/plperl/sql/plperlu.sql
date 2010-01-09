-- Use ONLY plperlu tests here. For plperl/plerlu combined tests
-- see plperl_plperlu.sql

--
-- Test compilation of unicode regex - regardless of locale.
-- This code fails in plain plperl in a non-UTF8 database.
--
CREATE OR REPLACE FUNCTION perl_unicode_regex(text) RETURNS INTEGER AS $$
  return ($_[0] =~ /\x{263A}|happy/i) ? 1 : 0; # unicode smiley
$$ LANGUAGE plperlu;
