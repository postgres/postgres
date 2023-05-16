CREATE EXTENSION fuzzystrmatch;


SELECT soundex('hello world!');

SELECT soundex('Anne'), soundex('Ann'), difference('Anne', 'Ann');
SELECT soundex('Anne'), soundex('Andrew'), difference('Anne', 'Andrew');
SELECT soundex('Anne'), soundex('Margaret'), difference('Anne', 'Margaret');
SELECT soundex(''), difference('', '');


SELECT levenshtein('GUMBO', 'GAMBOL');
SELECT levenshtein('GUMBO', 'GAMBOL', 2, 1, 1);
SELECT levenshtein_less_equal('extensive', 'exhaustive', 2);
SELECT levenshtein_less_equal('extensive', 'exhaustive', 4);


SELECT metaphone('GUMBO', 4);


SELECT dmetaphone('gumbo');
SELECT dmetaphone_alt('gumbo');

-- Wovels
SELECT daitch_mokotoff('Augsburg');
SELECT daitch_mokotoff('Breuer');
SELECT daitch_mokotoff('Freud');

-- The letter "H"
SELECT daitch_mokotoff('Halberstadt');
SELECT daitch_mokotoff('Mannheim');

-- Adjacent sounds
SELECT daitch_mokotoff('Chernowitz');

-- Adjacent letters with identical adjacent code digits
SELECT daitch_mokotoff('Cherkassy');
SELECT daitch_mokotoff('Kleinman');

-- More than one word
SELECT daitch_mokotoff('Nowy Targ');

-- Padded with "0"
SELECT daitch_mokotoff('Berlin');

-- Other examples from https://www.avotaynu.com/soundex.htm
SELECT daitch_mokotoff('Ceniow');
SELECT daitch_mokotoff('Tsenyuv');
SELECT daitch_mokotoff('Holubica');
SELECT daitch_mokotoff('Golubitsa');
SELECT daitch_mokotoff('Przemysl');
SELECT daitch_mokotoff('Pshemeshil');
SELECT daitch_mokotoff('Rosochowaciec');
SELECT daitch_mokotoff('Rosokhovatsets');

-- Ignored characters
SELECT daitch_mokotoff('''OBrien');
SELECT daitch_mokotoff('O''Brien');

-- "Difficult" cases, likely to cause trouble for other implementations.
SELECT daitch_mokotoff('CJC');
SELECT daitch_mokotoff('BESST');
SELECT daitch_mokotoff('BOUEY');
SELECT daitch_mokotoff('HANNMANN');
SELECT daitch_mokotoff('MCCOYJR');
SELECT daitch_mokotoff('ACCURSO');
SELECT daitch_mokotoff('BIERSCHBACH');
