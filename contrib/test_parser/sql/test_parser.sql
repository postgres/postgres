CREATE EXTENSION test_parser;

-- make test configuration using parser

CREATE TEXT SEARCH CONFIGURATION testcfg (PARSER = testparser);

ALTER TEXT SEARCH CONFIGURATION testcfg ADD MAPPING FOR word WITH simple;

-- ts_parse

SELECT * FROM ts_parse('testparser', 'That''s simple parser can''t parse urls like http://some.url/here/');

SELECT to_tsvector('testcfg','That''s my first own parser');

SELECT to_tsquery('testcfg', 'star');

SELECT ts_headline('testcfg','Supernovae stars are the brightest phenomena in galaxies',
       to_tsquery('testcfg', 'stars'));
