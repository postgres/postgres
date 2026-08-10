CREATE EXTENSION fuzzystrmatch;


SELECT soundex('hello world!');

SELECT soundex('Anne'), soundex('Ann'), difference('Anne', 'Ann');
SELECT soundex('Anne'), soundex('Andrew'), difference('Anne', 'Andrew');
SELECT soundex('Anne'), soundex('Margaret'), difference('Anne', 'Margaret');
SELECT soundex(''), difference('', '');


SELECT levenshtein('GUMBO', 'GAMBOL');
SELECT levenshtein('GUMBO', 'GAMBOL', 2, 1, 1);
SELECT levenshtein('GUMBO', 'GAMBOL', 1, 1, 2000000000);
SELECT levenshtein('GUMBO', 'GAMBOL', 2000000000, 2000000000, 2000000000);
SELECT levenshtein_less_equal('extensive', 'exhaustive', 2);
SELECT levenshtein_less_equal('extensive', 'exhaustive', 4);
SELECT levenshtein_less_equal('aaa', 'aaaaa', 1073741824, 0, 1073741824, 10);


SELECT metaphone('GUMBO', 4);


SELECT dmetaphone('gumbo');
SELECT dmetaphone_alt('gumbo');
