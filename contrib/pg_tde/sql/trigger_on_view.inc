CREATE extension pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');

--
-- 2 -- Test triggers on a join view
--
SET default_table_access_method TO ':tde_am';

DROP VIEW IF EXISTS city_view CASCADE;
DROP TABLE IF exists country_table CASCADE;
DROP TABLE IF exists city_table cascade;

 CREATE TABLE country_table (
     country_id        serial primary key,
     country_name    text unique not null,
     continent        text not null
 ) using :tde_am;
 
 INSERT INTO country_table (country_name, continent)
     VALUES ('Japan', 'Asia'),
            ('UK', 'Europe'),
            ('USA', 'North America')
     RETURNING *;
 
 CREATE TABLE city_table (
     city_id        serial primary key,
     city_name    text not null,
     population    bigint, 
     country_id    int references country_table
 ) using :tde_am;
 
 CREATE VIEW city_view AS
     SELECT city_id, city_name, population, country_name, continent
     FROM city_table ci
     LEFT JOIN country_table co ON co.country_id = ci.country_id;
 
CREATE OR REPLACE FUNCTION city_insert() RETURNS trigger LANGUAGE plpgsql AS $$
 declare
     ctry_id int;
 begin
     if NEW.country_name IS NOT NULL then
         SELECT country_id, continent INTO ctry_id, NEW.continent
             FROM country_table WHERE country_name = NEW.country_name;
         if NOT FOUND then 
             raise exception 'No such country: "%"', NEW.country_name;
         end if;
     else
         NEW.continent := NULL;
     end if;
 
     if NEW.city_id IS NOT NULL then
         INSERT INTO city_table
             VALUES(NEW.city_id, NEW.city_name, NEW.population, ctry_id);
     else
         INSERT INTO city_table(city_name, population, country_id)
             VALUES(NEW.city_name, NEW.population, ctry_id)
             RETURNING city_id INTO NEW.city_id;
     end if;
 
     RETURN NEW;
 end;
 $$;

  CREATE TRIGGER city_insert_trig INSTEAD OF INSERT ON city_view
 FOR EACH ROW EXECUTE PROCEDURE city_insert();
 
 CREATE OR REPLACE FUNCTION city_delete() RETURNS trigger LANGUAGE plpgsql AS $$
 begin
     DELETE FROM city_table WHERE city_id = OLD.city_id;
     if NOT FOUND then RETURN NULL; end if;
     RETURN OLD;
 end;
 $$;
 
 CREATE TRIGGER city_delete_trig INSTEAD OF DELETE ON city_view
 FOR EACH ROW EXECUTE PROCEDURE city_delete();
 
 CREATE OR REPLACE FUNCTION city_update() RETURNS trigger LANGUAGE plpgsql AS $$
 declare
     ctry_id int;
 begin
     if NEW.country_name IS DISTINCT FROM OLD.country_name then
         SELECT country_id, continent INTO ctry_id, NEW.continent
             FROM country_table WHERE country_name = NEW.country_name;
         if NOT FOUND then
             raise exception 'No such country: "%"', NEW.country_name;
         end if;
 
         UPDATE city_table SET city_name = NEW.city_name,
                               population = NEW.population,
                               country_id = ctry_id
             WHERE city_id = OLD.city_id;
     else
         UPDATE city_table SET city_name = NEW.city_name,
                               population = NEW.population
             WHERE city_id = OLD.city_id;
         NEW.continent := OLD.continent;
     end if;
 
     if NOT FOUND then RETURN NULL; end if;
     RETURN NEW;
 end;
 $$;

 CREATE TRIGGER city_update_trig INSTEAD OF UPDATE ON city_view
 FOR EACH ROW EXECUTE PROCEDURE city_update();
 
-- INSERT .. RETURNING
 INSERT INTO city_view(city_name) VALUES('Tokyo') RETURNING *;
 INSERT INTO city_view(city_name, population) VALUES('London', 7556900) RETURNING *;
 INSERT INTO city_view(city_name, country_name) VALUES('Washington DC', 'USA') RETURNING *;
 INSERT INTO city_view(city_id, city_name) VALUES(123456, 'New York') RETURNING *;
 INSERT INTO city_view VALUES(234567, 'Birmingham', 1016800, 'UK', 'EU') RETURNING *;
 
 -- UPDATE .. RETURNING
 UPDATE city_view SET country_name = 'Japon' WHERE city_name = 'Tokyo'; -- error
 UPDATE city_view SET country_name = 'Japan' WHERE city_name = 'Takyo'; -- no match
 UPDATE city_view SET country_name = 'Japan' WHERE city_name = 'Tokyo' RETURNING *; -- OK
 
 UPDATE city_view SET population = 13010279 WHERE city_name = 'Tokyo' RETURNING *;
 UPDATE city_view SET country_name = 'UK' WHERE city_name = 'New York' RETURNING *;
 UPDATE city_view SET country_name = 'USA', population = 8391881 WHERE city_name = 'New York' RETURNING *;
 UPDATE city_view SET continent = 'EU' WHERE continent = 'Europe' RETURNING *;
 UPDATE city_view v1 SET country_name = v2.country_name FROM city_view v2
     WHERE v2.city_name = 'Birmingham' AND v1.city_name = 'London' RETURNING *;
 
 -- DELETE .. RETURNING
 DELETE FROM city_view WHERE city_name = 'Birmingham' RETURNING *;
 

DROP extension pg_tde CASCADE;
