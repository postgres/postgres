-- Adjust this setting to control where the objects get created.
SET search_path = public;

DROP FUNCTION dmetaphone_alt (text);

DROP FUNCTION dmetaphone (text);

DROP FUNCTION difference(text,text);

DROP FUNCTION text_soundex(text);

DROP FUNCTION soundex(text);

DROP FUNCTION metaphone (text,int);

DROP FUNCTION levenshtein (text,text);
