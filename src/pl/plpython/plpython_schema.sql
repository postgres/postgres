
CREATE TABLE users (
	fname text not null,
	lname text not null,
	username text,
	userid serial,
	PRIMARY KEY(lname, fname) 
	) ;

CREATE INDEX users_username_idx ON users(username);
CREATE INDEX users_fname_idx ON users(fname);
CREATE INDEX users_lname_idx ON users(lname);
CREATE INDEX users_userid_idx ON users(userid);


CREATE TABLE taxonomy (
	id serial primary key,
	name text unique
	) ;

CREATE TABLE entry (
	accession text not null primary key,
	eid serial,
	txid int2 not null references taxonomy(id)
	) ;

CREATE TABLE sequences (
	eid int4 not null references entry(eid),
	pid serial primary key,
	product text not null,
	sequence text not null,
	multipart bool default 'false'
	) ;
CREATE INDEX sequences_product_idx ON sequences(product) ;

CREATE TABLE xsequences (
	pid int4 not null references sequences(pid),
	sequence text not null
	) ;
CREATE INDEX xsequences_pid_idx ON xsequences(pid) ;


