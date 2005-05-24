-- test plperl triggers

CREATE TABLE trigger_test (
        i int,
        v varchar
);

CREATE OR REPLACE FUNCTION valid_id() RETURNS trigger AS $$

    if (($_TD->{new}{i}>=100) || ($_TD->{new}{i}<=0))
    {
        return "SKIP";   # Skip INSERT/UPDATE command
    } 
    elsif ($_TD->{new}{v} ne "immortal") 
    {
        $_TD->{new}{v} .= "(modified by trigger)";
        return "MODIFY"; # Modify tuple and proceed INSERT/UPDATE command
    } 
    else 
    {
        return;          # Proceed INSERT/UPDATE command
    }
$$ LANGUAGE plperl;

CREATE TRIGGER "test_valid_id_trig" BEFORE INSERT OR UPDATE ON trigger_test
FOR EACH ROW EXECUTE PROCEDURE "valid_id"();

INSERT INTO trigger_test (i, v) VALUES (1,'first line');
INSERT INTO trigger_test (i, v) VALUES (2,'second line');
INSERT INTO trigger_test (i, v) VALUES (3,'third line');
INSERT INTO trigger_test (i, v) VALUES (4,'immortal');

INSERT INTO trigger_test (i, v) VALUES (101,'bad id');

SELECT * FROM trigger_test;

UPDATE trigger_test SET i = 5 where i=3;

UPDATE trigger_test SET i = 100 where i=1;

SELECT * FROM trigger_test;

CREATE OR REPLACE FUNCTION immortal() RETURNS trigger AS $$
    if ($_TD->{old}{v} eq $_TD->{args}[0])
    {
        return "SKIP"; # Skip DELETE command
    } 
    else 
    { 
        return;        # Proceed DELETE command
    };
$$ LANGUAGE plperl;

CREATE TRIGGER "immortal_trig" BEFORE DELETE ON trigger_test
FOR EACH ROW EXECUTE PROCEDURE immortal('immortal');

DELETE FROM trigger_test;


SELECT * FROM trigger_test;

