setup
{
  CREATE TABLE foo (a int PRIMARY KEY, b text);
  CREATE TABLE bar (a int NOT NULL REFERENCES foo);
  INSERT INTO foo VALUES (42);
}

teardown
{
  DROP TABLE foo, bar;
}

session "s1"
setup		{ BEGIN; }
step "ins"	{ INSERT INTO bar VALUES (42); }
step "com"	{ COMMIT; }

session "s2"
step "upd"	{ UPDATE foo SET b = 'Hello World'; }
