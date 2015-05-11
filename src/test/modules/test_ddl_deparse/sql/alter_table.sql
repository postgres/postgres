CREATE TABLE parent (
	a int
);

CREATE TABLE child () INHERITS (parent);

CREATE TABLE grandchild () INHERITS (child);

ALTER TABLE parent ADD COLUMN b serial;

ALTER TABLE parent RENAME COLUMN b TO c;

ALTER TABLE parent ADD CONSTRAINT a_pos CHECK (a > 0);
