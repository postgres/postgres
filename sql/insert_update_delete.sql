CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');

CREATE TABLE albums (
    id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    artist VARCHAR(256),
    title TEXT NOT NULL,
    released DATE NOT NULL
) USING pg_tde;

INSERT INTO albums (artist, title, released) VALUES
			('Graindelavoix', 'Jisquin The Undead', '2021-06-12'),
			('Graindelavoix', 'Tenebrae Responsoria - Carlo Gesualdo', '2019-08-06'),
			('Graindelavoix', 'Cypriot Vespers', '2015-12-20'),
			('John Coltrane', 'Blue Train', '1957-09-15'),
			('V/A Analog Africa', 'Space Echo - The Mystery Behind the Cosmic Sound of Cabo Verde Finally Revealed', '2016-05-27'),
			('Incapacitants', 'As Loud As Possible', '2022-09-15'),
			('Chris Corsano & Bill Orcutt', 'Made Out Of Sound', '2021-03-26'),
			('Jürg Frey (Quatuor Bozzini / Konus Quartett)', 'Continuit​é​, fragilit​é​, r​é​sonance', '2023-04-01'),
			('clipping.', 'Visions of Bodies Being Burned', '2020-10-23'),
			('clipping.', 'There Existed an Addiction to Blood', '2019-10-19'),
			('Autechre', 'elseq 1–5', '2016-05-19'),
			('Decapitated', 'Winds of Creation', '2000-04-17'),
			('Ulthar', 'Anthronomicon', '2023-02-17'),
			('Τζίμης Πανούσης', 'Κάγκελα Παντού', '1986-01-01'),
			('Воплі Відоплясова', 'Музіка', '1997-01-01');

SELECT * FROM albums;

DELETE FROM albums WHERE id % 4 = 0;
SELECT * FROM albums;

UPDATE albums SET title='Jisquin The Undead: Laments, Deplorations and Dances of Death', released='2021-10-01' WHERE id=1;
UPDATE albums SET released='2020-04-01' WHERE id=2;

SELECT * FROM albums;

DROP TABLE albums;
DROP EXTENSION pg_tde;
