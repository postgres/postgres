CREATE EXTENSION dict_xsyn;

-- default configuration - match first word and return it among with all synonyms
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=true, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=false);

--lexize
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- the same, but return only synonyms
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=false);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any word and return all words
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=true, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any word and return all words except first one
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=true, KEEPSYNONYMS=true, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any synonym but not first word, and return first word instead
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=true, MATCHORIG=false, KEEPSYNONYMS=false, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- do not match or return anything
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=false, KEEPSYNONYMS=false, MATCHSYNONYMS=false);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');

-- match any word but return nothing
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false, MATCHORIG=true, KEEPSYNONYMS=false, MATCHSYNONYMS=true);
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'sn');
SELECT ts_lexize('xsyn', 'grb');
