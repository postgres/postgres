-- test plperl triggers

CREATE TABLE trigger_test (
        i int,
        v varchar
);

CREATE OR REPLACE FUNCTION trigger_data() RETURNS trigger LANGUAGE plperl AS $$

  # make sure keys are sorted for consistent results - perl no longer
  # hashes in  repeatable fashion across runs

  foreach my $key (sort keys %$_TD)
  {

    my $val = $_TD->{$key};

	# relid is variable, so we can not use it repeatably
	$val = "bogus:12345" if $key eq 'relid';

	if (! defined $val)
	{
	  elog(NOTICE, "\$_TD->\{$key\} = NULL");
	}
	elsif (not ref $val)
    {
	  elog(NOTICE, "\$_TD->\{$key\} = '$val'");
	}
	elsif (ref $val eq 'HASH')
	{
	  my $str = "";
	  foreach my $rowkey (sort keys %$val)
	  {
	    $str .= ", " if $str;
	    my $rowval = $val->{$rowkey};
	    $str .= "'$rowkey' => '$rowval'";
      }
	  elog(NOTICE, "\$_TD->\{$key\} = \{$str\}");
	}
	elsif (ref $val eq 'ARRAY')
	{
	  my $str = "";
	  foreach my $argval (@$val)
	  {
	    $str .= ", " if $str;
	    $str .= "'$argval'";
      }
	  elog(NOTICE, "\$_TD->\{$key\} = \[$str\]");
	}
  }
  return undef; # allow statement to proceed;
$$;

CREATE TRIGGER show_trigger_data_trig 
BEFORE INSERT OR UPDATE OR DELETE ON trigger_test
FOR EACH ROW EXECUTE PROCEDURE trigger_data(23,'skidoo');

insert into trigger_test values(1,'insert');
update trigger_test set v = 'update' where i = 1;
delete from trigger_test;
	  
DROP TRIGGER show_trigger_data_trig on trigger_test;
	  
DROP FUNCTION trigger_data();

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
