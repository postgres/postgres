# Project Manager test
#
# Ensure that the person who is on the project as a manager
# is flagged as a project manager in the person table.
#
# Any overlap between the transactions must cause a serialization failure.

setup
{
 CREATE TABLE person (person_id int NOT NULL PRIMARY KEY, name text NOT NULL, is_project_manager bool NOT NULL);
 INSERT INTO person VALUES (1, 'Robert Haas', true);
 CREATE TABLE project (project_no int NOT NULL PRIMARY KEY, description text NOT NULL, project_manager int NOT NULL);
}

teardown
{
 DROP TABLE person, project;
}

session "s1"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "rx1"	{ SELECT count(*) FROM person WHERE person_id = 1 AND is_project_manager; }
step "wy1"	{ INSERT INTO project VALUES (101, 'Build Great Wall', 1); }
step "c1"	{ COMMIT; }

session "s2"
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "ry2"	{ SELECT count(*) FROM project WHERE project_manager = 1; }
step "wx2"	{ UPDATE person SET is_project_manager = false WHERE person_id = 1; }
step "c2"	{ COMMIT; }
