---
--- CREATE_RULE
---


CREATE RULE rule_1 AS
  ON INSERT
  TO datatype_table
  DO NOTHING;

CREATE RULE rule_2 AS
  ON UPDATE
  TO datatype_table
  DO INSERT INTO unlogged_table (id) VALUES(NEW.id);

CREATE RULE rule_3 AS
  ON DELETE
  TO datatype_table
  DO ALSO NOTHING;

CREATE RULE "_RETURN" AS
  ON SELECT
  TO like_datatype_table
  DO INSTEAD
    SELECT * FROM datatype_view;

CREATE RULE rule_3 AS
  ON DELETE
  TO like_datatype_table
  WHERE id < 100
  DO ALSO NOTHING;
