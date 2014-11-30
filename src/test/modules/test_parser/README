test_parser is an example of a custom parser for full-text
search.  It doesn't do anything especially useful, but can serve as
a starting point for developing your own parser.

test_parser recognizes words separated by white space,
and returns just two token types:

mydb=# SELECT * FROM ts_token_type('testparser');
 tokid | alias |  description
-------+-------+---------------
     3 | word  | Word
    12 | blank | Space symbols
(2 rows)

These token numbers have been chosen to be compatible with the default
parser's numbering.  This allows us to use its headline()
function, thus keeping the example simple.

Usage
=====

Installing the test_parser extension creates a text search
parser testparser.  It has no user-configurable parameters.

You can test the parser with, for example,

mydb=# SELECT * FROM ts_parse('testparser', 'That''s my first own parser');
 tokid | token
-------+--------
     3 | That's
    12 |
     3 | my
    12 |
     3 | first
    12 |
     3 | own
    12 |
     3 | parser

Real-world use requires setting up a text search configuration
that uses the parser.  For example,

mydb=# CREATE TEXT SEARCH CONFIGURATION testcfg ( PARSER = testparser );
CREATE TEXT SEARCH CONFIGURATION

mydb=# ALTER TEXT SEARCH CONFIGURATION testcfg
mydb-#   ADD MAPPING FOR word WITH english_stem;
ALTER TEXT SEARCH CONFIGURATION

mydb=#  SELECT to_tsvector('testcfg', 'That''s my first own parser');
          to_tsvector
-------------------------------
 'that':1 'first':3 'parser':5
(1 row)

mydb=# SELECT ts_headline('testcfg', 'Supernovae stars are the brightest phenomena in galaxies',
mydb(#                    to_tsquery('testcfg', 'star'));
                           ts_headline
-----------------------------------------------------------------
 Supernovae <b>stars</b> are the brightest phenomena in galaxies
(1 row)
