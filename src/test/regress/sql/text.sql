-- *************testing built-in type text  ****************

SELECT 'this is a text string'::text = 'this is a text string'::text AS true;

SELECT 'this is a text string'::text = 'this is a text strin'::text AS false;

CREATE TABLE TEXT_TBL (f1 text);

INSERT INTO TEXT_TBL VALUES ('doh!');
INSERT INTO TEXT_TBL VALUES ('hi de ho neighbor');

SELECT '' AS two, * FROM TEXT_TBL;

