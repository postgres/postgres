CREATE TABLE xmltest (
    id int,
    data xml
);

INSERT INTO xmltest VALUES (1, '<value>one</value>');
INSERT INTO xmltest VALUES (2, '<value>two</value>');
INSERT INTO xmltest VALUES (3, '<wrong');

SELECT * FROM xmltest;


SELECT xmlcomment('test');
SELECT xmlcomment('-test');
SELECT xmlcomment('test-');
SELECT xmlcomment('--test');
SELECT xmlcomment('te st');


SELECT xmlconcat(xmlcomment('hello'),
                 xmlelement(NAME qux, 'foo'),
                 xmlcomment('world'));

SELECT xmlconcat('hello', 'you');
SELECT xmlconcat(1, 2);
SELECT xmlconcat('bad', '<syntax');
SELECT xmlconcat('<foo/>', NULL, '<?xml version="1.1" standalone="no"?><bar/>');
SELECT xmlconcat('<?xml version="1.1"?><foo/>', NULL, '<?xml version="1.1" standalone="no"?><bar/>');
SELECT xmlconcat(NULL);
SELECT xmlconcat(NULL, NULL);


SELECT xmlelement(name element,
                  xmlattributes (1 as one, 'deuce' as two),
                  'content');

SELECT xmlelement(name element,
                  xmlattributes ('unnamed and wrong'));

SELECT xmlelement(name element, xmlelement(name nested, 'stuff'));

SELECT xmlelement(name employee, xmlforest(name, age, salary as pay)) FROM emp;

SELECT xmlelement(name duplicate, xmlattributes(1 as a, 2 as b, 3 as a));

SELECT xmlelement(name num, 37);
SELECT xmlelement(name foo, text 'bar');
SELECT xmlelement(name foo, xml 'bar');
SELECT xmlelement(name foo, text 'b<a/>r');
SELECT xmlelement(name foo, xml 'b<a/>r');
SELECT xmlelement(name foo, array[1, 2, 3]);
SET xmlbinary TO base64;
SELECT xmlelement(name foo, bytea 'bar');
SET xmlbinary TO hex;
SELECT xmlelement(name foo, bytea 'bar');

SELECT xmlelement(name foo, xmlattributes(true as bar));
SELECT xmlelement(name foo, xmlattributes('2009-04-09 00:24:37'::timestamp as bar));
SELECT xmlelement(name foo, xmlattributes('infinity'::timestamp as bar));
SELECT xmlelement(name foo, xmlattributes('<>&"''' as funny, xml 'b<a/>r' as funnier));


SELECT xmlparse(content 'abc');
SELECT xmlparse(content '<abc>x</abc>');
SELECT xmlparse(content '<invalidentity>&</invalidentity>');
SELECT xmlparse(content '<undefinedentity>&idontexist;</undefinedentity>');
SELECT xmlparse(content '<invalidns xmlns=''&lt;''/>');
SELECT xmlparse(content '<relativens xmlns=''relative''/>');
SELECT xmlparse(content '<twoerrors>&idontexist;</unbalanced>');
SELECT xmlparse(content '<nosuchprefix:tag/>');

SELECT xmlparse(document 'abc');
SELECT xmlparse(document '<abc>x</abc>');
SELECT xmlparse(document '<invalidentity>&</abc>');
SELECT xmlparse(document '<undefinedentity>&idontexist;</abc>');
SELECT xmlparse(document '<invalidns xmlns=''&lt;''/>');
SELECT xmlparse(document '<relativens xmlns=''relative''/>');
SELECT xmlparse(document '<twoerrors>&idontexist;</unbalanced>');
SELECT xmlparse(document '<nosuchprefix:tag/>');


SELECT xmlpi(name foo);
SELECT xmlpi(name xml);
SELECT xmlpi(name xmlstuff);
SELECT xmlpi(name foo, 'bar');
SELECT xmlpi(name foo, 'in?>valid');
SELECT xmlpi(name foo, null);
SELECT xmlpi(name xml, null);
SELECT xmlpi(name xmlstuff, null);
SELECT xmlpi(name "xml-stylesheet", 'href="mystyle.css" type="text/css"');
SELECT xmlpi(name foo, '   bar');


SELECT xmlroot(xml '<foo/>', version no value, standalone no value);
SELECT xmlroot(xml '<foo/>', version '2.0');
SELECT xmlroot(xml '<foo/>', version no value, standalone yes);
SELECT xmlroot(xml '<?xml version="1.1"?><foo/>', version no value, standalone yes);
SELECT xmlroot(xmlroot(xml '<foo/>', version '1.0'), version '1.1', standalone no);
SELECT xmlroot('<?xml version="1.1" standalone="yes"?><foo/>', version no value, standalone no);
SELECT xmlroot('<?xml version="1.1" standalone="yes"?><foo/>', version no value, standalone no value);
SELECT xmlroot('<?xml version="1.1" standalone="yes"?><foo/>', version no value);


SELECT xmlroot (
  xmlelement (
    name gazonk,
    xmlattributes (
      'val' AS name,
      1 + 1 AS num
    ),
    xmlelement (
      NAME qux,
      'foo'
    )
  ),
  version '1.0',
  standalone yes
);


SELECT xmlserialize(content data as character varying(20)) FROM xmltest;
SELECT xmlserialize(content 'good' as char(10));
SELECT xmlserialize(document 'bad' as text);


SELECT xml '<foo>bar</foo>' IS DOCUMENT;
SELECT xml '<foo>bar</foo><bar>foo</bar>' IS DOCUMENT;
SELECT xml '<abc/>' IS NOT DOCUMENT;
SELECT xml 'abc' IS NOT DOCUMENT;
SELECT '<>' IS NOT DOCUMENT;


SELECT xmlagg(data) FROM xmltest;
SELECT xmlagg(data) FROM xmltest WHERE id > 10;
SELECT xmlelement(name employees, xmlagg(xmlelement(name name, name))) FROM emp;


-- Check mapping SQL identifier to XML name

SELECT xmlpi(name ":::_xml_abc135.%-&_");
SELECT xmlpi(name "123");


PREPARE foo (xml) AS SELECT xmlconcat('<foo/>', $1);

SET XML OPTION DOCUMENT;
EXECUTE foo ('<bar/>');
EXECUTE foo ('bad');

SET XML OPTION CONTENT;
EXECUTE foo ('<bar/>');
EXECUTE foo ('good');


-- Test backwards parsing

CREATE VIEW xmlview1 AS SELECT xmlcomment('test');
CREATE VIEW xmlview2 AS SELECT xmlconcat('hello', 'you');
CREATE VIEW xmlview3 AS SELECT xmlelement(name element, xmlattributes (1 as ":one:", 'deuce' as two), 'content&');
CREATE VIEW xmlview4 AS SELECT xmlelement(name employee, xmlforest(name, age, salary as pay)) FROM emp;
CREATE VIEW xmlview5 AS SELECT xmlparse(content '<abc>x</abc>');
CREATE VIEW xmlview6 AS SELECT xmlpi(name foo, 'bar');
CREATE VIEW xmlview7 AS SELECT xmlroot(xml '<foo/>', version no value, standalone yes);
CREATE VIEW xmlview8 AS SELECT xmlserialize(content 'good' as char(10));
CREATE VIEW xmlview9 AS SELECT xmlserialize(content 'good' as text);

SELECT table_name, view_definition FROM information_schema.views
  WHERE table_name LIKE 'xmlview%' ORDER BY 1;

-- Text XPath expressions evaluation

SELECT xpath('/value', data) FROM xmltest;
SELECT xpath(NULL, NULL) IS NULL FROM xmltest;
SELECT xpath('', '<!-- error -->');
SELECT xpath('//text()', '<local:data xmlns:local="http://127.0.0.1"><local:piece id="1">number one</local:piece><local:piece id="2" /></local:data>');
SELECT xpath('//loc:piece/@id', '<local:data xmlns:local="http://127.0.0.1"><local:piece id="1">number one</local:piece><local:piece id="2" /></local:data>', ARRAY[ARRAY['loc', 'http://127.0.0.1']]);
SELECT xpath('//b', '<a>one <b>two</b> three <b>etc</b></a>');
SELECT xpath('//text()', '<root>&lt;</root>');
SELECT xpath('//@value', '<root value="&lt;"/>');
SELECT xpath('''<<invalid>>''', '<root/>');
SELECT xpath('count(//*)', '<root><sub/><sub/></root>');
SELECT xpath('count(//*)=0', '<root><sub/><sub/></root>');
SELECT xpath('count(//*)=3', '<root><sub/><sub/></root>');
SELECT xpath('name(/*)', '<root><sub/><sub/></root>');
SELECT xpath('/nosuchtag', '<root/>');

-- Test xmlexists and xpath_exists
SELECT xmlexists('//town[text() = ''Toronto'']' PASSING BY REF '<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>');
SELECT xmlexists('//town[text() = ''Cwmbran'']' PASSING BY REF '<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>');
SELECT xmlexists('count(/nosuchtag)' PASSING BY REF '<root/>');
SELECT xpath_exists('//town[text() = ''Toronto'']','<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>'::xml);
SELECT xpath_exists('//town[text() = ''Cwmbran'']','<towns><town>Bidford-on-Avon</town><town>Cwmbran</town><town>Bristol</town></towns>'::xml);
SELECT xpath_exists('count(/nosuchtag)', '<root/>'::xml);

INSERT INTO xmltest VALUES (4, '<menu><beers><name>Budvar</name><cost>free</cost><name>Carling</name><cost>lots</cost></beers></menu>'::xml);
INSERT INTO xmltest VALUES (5, '<menu><beers><name>Molson</name><cost>free</cost><name>Carling</name><cost>lots</cost></beers></menu>'::xml);
INSERT INTO xmltest VALUES (6, '<myns:menu xmlns:myns="http://myns.com"><myns:beers><myns:name>Budvar</myns:name><myns:cost>free</myns:cost><myns:name>Carling</myns:name><myns:cost>lots</myns:cost></myns:beers></myns:menu>'::xml);
INSERT INTO xmltest VALUES (7, '<myns:menu xmlns:myns="http://myns.com"><myns:beers><myns:name>Molson</myns:name><myns:cost>free</myns:cost><myns:name>Carling</myns:name><myns:cost>lots</myns:cost></myns:beers></myns:menu>'::xml);

SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beer' PASSING data);
SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beer' PASSING BY REF data BY REF);
SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beers' PASSING BY REF data);
SELECT COUNT(id) FROM xmltest WHERE xmlexists('/menu/beers/name[text() = ''Molson'']' PASSING BY REF data);

SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/menu/beer',data);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/menu/beers',data);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/menu/beers/name[text() = ''Molson'']',data);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/myns:menu/myns:beer',data,ARRAY[ARRAY['myns','http://myns.com']]);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/myns:menu/myns:beers',data,ARRAY[ARRAY['myns','http://myns.com']]);
SELECT COUNT(id) FROM xmltest WHERE xpath_exists('/myns:menu/myns:beers/myns:name[text() = ''Molson'']',data,ARRAY[ARRAY['myns','http://myns.com']]);

CREATE TABLE query ( expr TEXT );
INSERT INTO query VALUES ('/menu/beers/cost[text() = ''lots'']');
SELECT COUNT(id) FROM xmltest, query WHERE xmlexists(expr PASSING BY REF data);

-- Test xml_is_well_formed and variants

SELECT xml_is_well_formed_document('<foo>bar</foo>');
SELECT xml_is_well_formed_document('abc');
SELECT xml_is_well_formed_content('<foo>bar</foo>');
SELECT xml_is_well_formed_content('abc');

SET xmloption TO DOCUMENT;
SELECT xml_is_well_formed('abc');
SELECT xml_is_well_formed('<>');
SELECT xml_is_well_formed('<abc/>');
SELECT xml_is_well_formed('<foo>bar</foo>');
SELECT xml_is_well_formed('<foo>bar</foo');
SELECT xml_is_well_formed('<foo><bar>baz</foo>');
SELECT xml_is_well_formed('<local:data xmlns:local="http://127.0.0.1"><local:piece id="1">number one</local:piece><local:piece id="2" /></local:data>');
SELECT xml_is_well_formed('<pg:foo xmlns:pg="http://postgresql.org/stuff">bar</my:foo>');
SELECT xml_is_well_formed('<pg:foo xmlns:pg="http://postgresql.org/stuff">bar</pg:foo>');
SELECT xml_is_well_formed('<invalidentity>&</abc>');
SELECT xml_is_well_formed('<undefinedentity>&idontexist;</abc>');
SELECT xml_is_well_formed('<invalidns xmlns=''&lt;''/>');
SELECT xml_is_well_formed('<relativens xmlns=''relative''/>');
SELECT xml_is_well_formed('<twoerrors>&idontexist;</unbalanced>');

SET xmloption TO CONTENT;
SELECT xml_is_well_formed('abc');

-- Since xpath() deals with namespaces, it's a bit stricter about
-- what's well-formed and what's not. If we don't obey these rules
-- (i.e. ignore namespace-related errors from libxml), xpath()
-- fails in subtle ways. The following would for example produce
-- the xml value
--   <invalidns xmlns='<'/>
-- which is invalid because '<' may not appear un-escaped in
-- attribute values.
-- Since different libxml versions emit slightly different
-- error messages, we suppress the DETAIL in this test.
\set VERBOSITY terse
SELECT xpath('/*', '<invalidns xmlns=''&lt;''/>');
\set VERBOSITY default

-- Again, the XML isn't well-formed for namespace purposes
SELECT xpath('/*', '<nosuchprefix:tag/>');

-- XPath deprecates relative namespaces, but they're not supposed to
-- throw an error, only a warning.
SELECT xpath('/*', '<relativens xmlns=''relative''/>');

-- External entity references should not leak filesystem information.
SELECT XMLPARSE(DOCUMENT '<!DOCTYPE foo [<!ENTITY c SYSTEM "/etc/passwd">]><foo>&c;</foo>');
SELECT XMLPARSE(DOCUMENT '<!DOCTYPE foo [<!ENTITY c SYSTEM "/etc/no.such.file">]><foo>&c;</foo>');
-- This might or might not load the requested DTD, but it mustn't throw error.
SELECT XMLPARSE(DOCUMENT '<!DOCTYPE chapter PUBLIC "-//OASIS//DTD DocBook XML V4.1.2//EN" "http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd"><chapter>&nbsp;</chapter>');
