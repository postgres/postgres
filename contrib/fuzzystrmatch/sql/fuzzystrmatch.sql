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
