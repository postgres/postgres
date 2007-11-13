/* $PostgreSQL: pgsql/contrib/fuzzystrmatch/uninstall_fuzzystrmatch.sql,v 1.3 2007/11/13 04:24:28 momjian Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP FUNCTION dmetaphone_alt (text);

DROP FUNCTION dmetaphone (text);

DROP FUNCTION difference(text,text);

DROP FUNCTION text_soundex(text);

DROP FUNCTION soundex(text);

DROP FUNCTION metaphone (text,int);

DROP FUNCTION levenshtein (text,text);
