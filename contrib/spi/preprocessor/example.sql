--  Note the syntax is strict because i have no time to write better perl filter.
--
--  [blank] is 1 blank 
--  at the end of an interesting line must be a [,]  or [--]
--  [ending] must be a , or --  
--  
--  foreign[blank]key[blank]([blank]keyname,..,keyname[blank])[blank]references[blank]table[blank][ending] 
--
--  step1 < example.sql | step2.pl > foreign_key_triggers.sql
--   
--  step1.c is a simple program that UPPERCASE ALL . I know that is simple implementing in Perl
--  bu i haven't time


CREATE TABLE 
gruppo
(
codice_gruppo  		int4 			NOT NULL,
descrizione         	varchar(32)    		NOT NULL
primary key ( codice_gruppo ) 

) ;

--
-- fa_parte : Appartenenza di una Azienda Conatto o Cliente ad un certo GRUPPO
--

CREATE TABLE 
fa_parte 
(
codice_gruppo   		int4	NOT NULL,
codice_contatto         	int4   	NOT NULL,

primary key ( codice_gruppo,codice_contatto ) ,
foreign key ( codice_gruppo ) references gruppo --
);

