INSERT INTO users (fname, lname, username) VALUES ('jane', 'doe', 'j_doe');
INSERT INTO users (fname, lname, username) VALUES ('john', 'doe', 'johnd');
INSERT INTO users (fname, lname, username) VALUES ('willem', 'doe', 'w_doe');
INSERT INTO users (fname, lname, username) VALUES ('rick', 'smith', 'slash');


-- multi table tests
--

INSERT INTO taxonomy (name) VALUES ('HIV I') ;
INSERT INTO taxonomy (name) VALUES ('HIV II') ;
INSERT INTO taxonomy (name) VALUES ('HCV') ;

INSERT INTO entry (accession, txid) VALUES ('A00001', '1') ;
INSERT INTO entry (accession, txid) VALUES ('A00002', '1') ;
INSERT INTO entry (accession, txid) VALUES ('A00003', '1') ;
INSERT INTO entry (accession, txid) VALUES ('A00004', '2') ;
INSERT INTO entry (accession, txid) VALUES ('A00005', '2') ;
INSERT INTO entry (accession, txid) VALUES ('A00006', '3') ;

INSERT INTO sequences (sequence, eid, product, multipart) VALUES ('ABCDEF', 1, 'env', 'true') ;
INSERT INTO xsequences (sequence, pid) VALUES ('GHIJKL', 1) ;
INSERT INTO sequences (sequence, eid, product) VALUES ('ABCDEF', 2, 'env') ;
INSERT INTO sequences (sequence, eid, product) VALUES ('ABCDEF', 3, 'env') ;
INSERT INTO sequences (sequence, eid, product) VALUES ('ABCDEF', 4, 'gag') ;
INSERT INTO sequences (sequence, eid, product) VALUES ('ABCDEF', 5, 'env') ;
INSERT INTO sequences (sequence, eid, product) VALUES ('ABCDEF', 6, 'ns1') ;
