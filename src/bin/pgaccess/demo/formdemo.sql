CREATE SEQUENCE "cities_id_seq" start 7 increment 1 maxvalue 2147483647 minvalue 1  cache 1 ;
SELECT nextval ('"cities_id_seq"');
CREATE TABLE "pga_queries" (
	"queryname" character varying(64),
	"querytype" character,
	"querycommand" text,
	"querytables" text,
	"querylinks" text,
	"queryresults" text,
	"querycomments" text
);
CREATE TABLE "pga_forms" (
	"formname" character varying(64),
	"formsource" text
);
CREATE TABLE "pga_scripts" (
	"scriptname" character varying(64),
	"scriptsource" text
);
CREATE TABLE "pga_reports" (
	"reportname" character varying(64),
	"reportsource" text,
	"reportbody" text,
	"reportprocs" text,
	"reportoptions" text
);
CREATE TABLE "phonebook" (
	"name" character varying(32),
	"phone_nr" character varying(16),
	"city" character varying(32),
	"company" bool,
	"continent" character varying(16)
);
CREATE TABLE "pga_layout" (
	"tablename" character varying(64),
	"nrcols" int2,
	"colnames" text,
	"colwidth" text
);
CREATE TABLE "pga_schema" (
	"schemaname" character varying(64),
	"schematables" text,
	"schemalinks" text
);
REVOKE ALL on "pga_schema" from PUBLIC;
GRANT ALL on "pga_schema" to PUBLIC;
CREATE TABLE "cities" (
	"id" int4 DEFAULT nextval('cities_id_seq'::text) NOT NULL,
	"name" character varying(32) NOT NULL,
	"prefix" character varying(16) NOT NULL
);
REVOKE ALL on "cities" from PUBLIC;
GRANT INSERT,SELECT,RULE on "cities" to "teo";
CREATE FUNCTION "getcityprefix" (int4 ) RETURNS varchar AS 'select prefix from cities where id = $1 ' LANGUAGE 'SQL';
COPY "pga_queries" FROM stdin;
Query that can be saved as view	S	select * from phonebook where continent='usa'    	\N	\N	\N	\N
\.
COPY "pga_forms" FROM stdin;
Phone book	pb 28 {FS {}} 444x307+284+246 {label label1 {33 10 68 28} {} Name {} label1 flat #000000 #d9d9d9 1 n} {entry name_entry {87 9 227 27} {} entry2 DataSet(.pb.qs,name) name_entry sunken #000000 #fefefe 1 n} {label label3 {33 37 73 52} {} Phone {} label3 flat #000000 #d9d9d9 1 n} {entry entry4 {87 36 195 54} {} entry4 DataSet(.pb.qs,phone_nr) entry4 sunken #000000 #fefefe 1 n} {label label5 {33 64 78 82} {} City {} label5 flat #000000 #d9d9d9 1 n} {entry entry6 {87 63 195 81} {} entry6 DataSet(.pb.qs,city) entry6 sunken #000000 #fefefe 1 n} {query qs {3 6 33 33} {} query7 {} qs flat {} {} 1 n} {button button8 {174 177 246 203} {namespace eval DataControl(.pb.qs) {\
\	setSQL "select oid,* from phonebook where name ~* '$what' order by name"\
\	open\
\	set nrecs [getRowCount]\
\	updateDataSet\
\	fill .pb.allnames name\
\	bind .pb.allnames <ButtonRelease-1> {\
\	   set ancr [.pb.allnames curselection]\
\	   if {$ancr!=""} {\
\	\	DataControl(.pb.qs)::moveTo $ancr\
\	\	DataControl(.pb.qs)::updateDataSet\
\	   }\
\	}\
}} {Start search} {} button8 raised #000000 #d9d9d9 1 n} {button button9 {363 276 433 300} {DataControl(.pb.qs)::close\
DataControl(.pb.qs)::clearDataSet\
set nrecs {}\
set what {}\
destroy .pb\
} Exit {} button9 raised #000000 #d9d9d9 2 n} {button button10 {291 237 313 257} {namespace eval DataControl(.pb.qs) {\
\	moveFirst\
\	updateDataSet\
}\
} |< {} button10 ridge #000092 #d9d9d9 2 n} {button button11 {324 237 346 257} {namespace eval DataControl(.pb.qs) {\
\	movePrevious\
\	updateDataSet\
}\
} << {} button11 ridge #000000 #d9d9d9 2 n} {button button12 {348 237 370 257} {namespace eval DataControl(.pb.qs) {\
\	moveNext\
\	updateDataSet\
}} >> {} button12 ridge #000000 #d9d9d9 2 n} {button button13 {381 237 403 257} {namespace eval DataControl(.pb.qs) {\
\	moveLast\
\	updateDataSet\
}\
} >| {} button13 ridge #000088 #d9d9d9 2 n} {checkbox checkbox14 {33 87 126 105} {} {Is it a company ?} DataSet(.pb.qs,company) checkbox14 flat #000000 #d9d9d9 1 n} {radio usa {63 108 201 120} {} U.S.A. DataSet(.pb.qs,continent) usa flat #000000 #d9d9d9 1 n} {radio europe {63 126 204 141} {} Europe DataSet(.pb.qs,continent) europe flat #000000 #d9d9d9 1 n} {radio africa {63 144 210 159} {} Africa DataSet(.pb.qs,continent) africa flat #000000 #d9d9d9 1 n} {entry entry18 {129 180 169 198} {} entry18 what entry18 sunken #000000 #fefefe 1 n} {label label19 {108 219 188 234} {} {records found} {} label19 flat #000000 #d9d9d9 1 n} {label label20 {90 219 105 234} {} { } nrecs label20 flat #000000 #d9d9d9 1 n} {label label21 {3 252 33 267} {} OID= {} label21 flat #000000 #d9d9d9 1 n} {label label22 {39 252 87 267} {} { } pbqs(oid) label22 flat #000000 #d9d9d9 1 n} {button button23 {9 276 79 300} {set oid {}\
catch {set oid $DataSet(.pb.qs,oid)}\
if {[string trim $oid]!=""} {\
   sql_exec noquiet "update phonebook set name='$DataSet(.pb.qs,name)', phone_nr='$DataSet(.pb.qs,phone_nr)',city='$DataSet(.pb.qs,city)',company='$DataSet(.pb.qs,company)',continent='$DataSet(.pb.qs,continent)' where oid=$oid"\
} else {\
  tk_messageBox -title Error -message "No record is displayed!"\
}\
\
} Update {} button23 raised #000000 #d9d9d9 1 n} {button button24 {210 276 280 300} {set thisname $DataSet(.pb.qs,name)\
if {[string trim $thisname] != ""} {\
\	sql_exec noquiet "insert into phonebook values (\
\	\	'$DataSet(.pb.qs,name)',\
\	\	'$DataSet(.pb.qs,phone_nr)',\
\	\	'$DataSet(.pb.qs,city)',\
\	\	'$DataSet(.pb.qs,company)',\
\	\	'$DataSet(.pb.qs,continent)'\
\	)"\
\	tk_messageBox -title Information -message "A new record has been added!"\
} else {\
\	tk_messageBox -title Error -message "This one doesn't have a name?"\
}\
\
} {Add record} {} button24 raised #000000 #d9d9d9 1 n} {button button25 {141 276 204 300} {DataControl(.pb.qs)::clearDataSet\
# clearcontrols stillinitialise\
# incorectly booleans controls to {}\
# so I force it to 'f' (false)\
set DataSet(.pb.qs,company) f\
focus .pb.name_entry} {Clear all} {} button25 raised #000000 #d9d9d9 1 n} {listbox allnames {249 6 435 231} {} listbox26 {} allnames sunken #000000 #fefefe 1 n} {label label27 {33 252 90 267} {} {} DataSet(.pb.qs,oid) label27 flat #000000 #d9d9d9 1 n} {label label28 {3 182 128 197} {} {Find name containing} {} {} flat #000000 #d9d9d9 1 n}
Full featured form	full 21 {FS {set entrydemo {nice}\
set color {no color selected}}} 377x418+50+130 {label label1 {3 396 165 411} {} {Status line} {} {} sunken #000000 #d9d9d9 2 n} {label label2 {171 396 369 411} {} {Grooved status line} {} {} groove #000098 #d9d9d9 2 f} {label label3 {108 9 270 31} {} {     Full featured form} {} {} ridge #000000 #d9d9d9 4 {Times 16 bold italic}} {button button4 {15 210 144 243} {.full.lb insert end {it's} a nice demo form} {Java style button} {} {} groove #6161b6 #d9d9d9 2 b} {label label5 {15 42 115 58} {} {Java style label} {} {} flat #6161b6 #d9d9d9 1 b} {entry entry6 {123 39 279 60} {} entry6 entrydemo {} groove #000000 #fefefe 2 {Courier 13}} {listbox lb {12 69 147 201} {} listbox8 {} {} ridge #000000 #ffffc8 2 n} {button button9 {18 264 39 282} {} 1 {} {} flat #000000 #d9d9d9 1 n} {button button10 {48 264 68 282} {} 2 {} {} flat #000000 #d9d9d9 1 n} {button button11 {78 264 234 282} {} {and other hidden buttons} {} {} flat #000000 #d9d9d9 1 n} {text txt {153 69 372 201} {} text12 {} {} sunken #000000 #d4ffff 1 n} {button button13 {150 210 369 243} {.full.txt tag configure bold -font {Helvetica 12 bold}\
.full.txt tag configure italic -font {Helvetica 12 italic}\
.full.txt tag configure large -font {Helvetica -14 bold}\
.full.txt tag configure title -font {Helvetica 12 bold italic} -justify center\
.full.txt tag configure link -font {Helvetica -12 underline} -foreground #000080\
.full.txt tag configure code -font {Courier 13}\
.full.txt tag configure warning -font {Helvetica 12 bold} -foreground #800000\
\
# That't the way help files are written\
\
.full.txt delete 1.0 end\
.full.txt insert end {Centered title} {title} "\
\
You can make different " {} "portions of text bold" {bold} " or italic " {italic} ".\
Some parts of them can be written as follows" {} "\
SELECT * FROM PHONEBOOK" {code} "\
You can also change " {} "colors for some words " {warning} "or underline them" {link} } {Old style button} {} {} raised #000000 #d9d9d9 2 n} {checkbox checkbox14 {48 297 153 309} {} different {} {} flat #00009c #d9d9d9 1 b} {checkbox checkbox15 {48 321 156 336} {} {fonts and} {} {} flat #cc0000 #d9d9d9 1 i} {checkbox checkbox16 {48 345 156 360} {} colors {} {} flat #00b600 #dfb2df 1 f} {radio radio17 {207 297 330 315} {} {red , rosu , rouge} color red flat #9c0000 #d9d9d9 1 n} {radio radio18 {207 321 324 333} {} {green , verde , vert} color green flat #009000 #d9d9d9 1 n} {radio radio19 {207 345 327 363} {} {blue , albastru, bleu} color blue flat #000000 #d9d9d9 1 n} {label selcolor {210 369 345 384} {} {} color {} flat #000000 #d9d9d9 1 n} {button button21 {285 258 363 285} {destroy .full} Exit {} {} raised #7c0000 #dfdbb8 1 b}
Working with Tables namespace	f3 13 {3 4 5 6 7 9 10 11 12 13} 377x263+59+127 {radio usa {36 24 138 36} {} USA selcont} {radio europe {36 45 141 60} {} Europe selcont} {radio africa {36 66 147 81} {} Africa selcont} {label label6 {9 99 339 114} {} {Select one of the above continents and press} {}} {button button7 {270 93 354 117} {Tables::open phonebook "continent='$selcont'" $selorder} {Show them} {}} {button button9 {66 189 312 213} {Tables::design phonebook} {Show me the phonebook table structure} {}} {button button10 {141 228 240 252} {destroy .f3} {Close the form} {}} {button button11 {93 141 282 165} {Tables::open phonebook "company=true"} {Show me only the companies} {}} {radio name {183 24 261 36} {} {Order by name} selorder} {radio phone_nr {183 45 267 57} {} {Order by phone number} selorder}
The simplest form	mf 5 {FS {set thename {}}} 306x136+82+146 {label label {42 45 99 60} {} Name {} label flat #000000 #d9d9d9 1 {Helvetica 12 bold italic}} {entry ename {120 42 219 63} {} entry2 thename ename sunken #000000 #fefefe 1 n} {button button3 {6 96 108 129} {set thename Teo} {Set the name} {} button3 raised #000000 #d9d9d9 1 n} {button button4 {192 96 300 129} {destroy .mf} {Close the form} {} button4 raised #000000 #d9d9d9 1 n} {button button5 {114 96 186 129} {set thename {}} {Clear it} {} button5 raised #000000 #d9d9d9 1 n}
A simple demo form	asdf 14 {FS {set color none}} 370x310+50+75 {label label1 {15 36 99 57} {} {Selected color} {} label1 flat #000000 #d9d9d9 1} {entry entry2 {111 36 225 54} {} entry2 color entry2 sunken #000000 #fefefe 1} {radio red {249 21 342 36} {} {Red as cherry} color red flat #900000 #d9d9d9 1} {radio green {249 45 342 60} {} {Green as a melon} color green flat #008800 #d9d9d9 1} {radio blue {249 69 342 84} {} {Blue as the sky} color blue flat #00008c #d9d9d9 1} {button button6 {45 69 198 99} {set color spooky} {Set a weird color} {} button6 ridge #0000b0 #dfbcdf 2} {label label7 {24 129 149 145} {} {The checkbox's value} {} label7 flat #000000 #d9d9d9 1} {entry entry8 {162 127 172 145} {} entry8 cbvalue entry8 sunken #000000 #fefefe 1} {checkbox checkbox9 {180 126 279 150} {} {Check me :-)} cbvalue checkbox9 flat #000000 #d9d9d9 1} {button button10 {219 273 366 303} {destroy .asdf} {Close that simple form} {} button10 raised #000000 #d9d9d9 1} {button button11 {219 237 366 267} {Forms::open "Phone book"} {Open my phone book} {} button11 raised #000000 #d9d9d9 1} {listbox lb {12 192 162 267} {} listbox12 {} lb sunken #000000 #fefefe 1} {button button13 {12 156 162 186} {.asdf.lb insert end red green blue cyan white navy black purple maroon violet} {Add some information} {} button13 raised #000000 #d9d9d9 1} {button button14 {12 273 162 303} {.asdf.lb delete 0 end} {Clear this listbox} {} button14 raised #000000 #d9d9d9 1}
Working with listboxes	f2 5 {FS {set thestudent ""}} 257x263+139+147 {listbox lb {6 6 246 186} {} listbox1 {} lb sunken #000000 #ffffd4 1} {button button2 {9 234 124 258} {# Populate the listbox with some data\
#\
\
foreach student {John Bill Doe Gigi} {\
\	.f2.lb insert end $student\
}\
\
\
\
# Binding the event left button release to the\
# list box\
\
bind .f2.lb <ButtonRelease-1> {\
\	set idsel [.f2.lb curselection]\
\	if {$idsel!=""} {\
\	\	set thestudent [.f2.lb get $idsel]\
\	}\
}\
\
# Cleaning the variable thestudent\
\
set thestudent {}} {Show students} {} button2 groove #000000 #d9d9d9 2} {button button3 {132 234 247 258} {destroy .f2} {Close the form} {} button3 groove #000000 #d9d9d9 1} {label label4 {9 213 119 228} {} {You have selected} {} label4 flat #000000 #d9d9d9 1} {label label5 {129 213 219 228} {} {} thestudent label5 flat #00009a #d9d9d9 1}
Invoices	inv 0 {FS {frame .inv.f\
place .inv.f -x 5 -y 100 -width 500 -height 300\
set wn [Tables::getNewWindowName]\
Tables::createWindow .inv.f\
set PgAcVar(mw,.inv.f,updatable) 0\
set PgAcVar(mw,.inv.f,layout_found) 0\
set PgAcVar(mw,.inv.f,layout_name) ""\
Tables::selectRecords .inv.f "select * from cities"\
}} 631x439+87+84
\.
COPY "pga_scripts" FROM stdin;
How are forms keeped inside ?	Tables::open pga_forms\
\
\
\

Opening a table with filters	Tables::open phonebook "name ~* 'e'" "name desc"\
\
\

Autoexec	Mainlib::tab_click Forms\
Forms::open {Full featured form}\
\
\

\.
COPY "pga_reports" FROM stdin;
My phone book	phonebook	set PgAcVar(report,tablename) "phonebook" ; set PgAcVar(report,y_rpthdr) 21 ; set PgAcVar(report,y_pghdr) 47 ; set PgAcVar(report,y_detail) 66 ; set PgAcVar(report,y_pgfoo) 96 ; set PgAcVar(report,y_rptfoo) 126 ; .pgaw:ReportBuilder.c create text 10 35 -font -Adobe-Helvetica-Bold-R-Normal--*-120-*-*-*-*-*-* -anchor nw -text {name} -tags {t_l mov ro} ; .pgaw:ReportBuilder.c create text 10 52 -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -anchor nw -text {name} -tags {f-name t_f rg_detail mov ro} ; .pgaw:ReportBuilder.c create text 141 36 -font -Adobe-Helvetica-Bold-R-Normal--*-120-*-*-*-*-*-* -anchor nw -text {city} -tags {t_l mov ro} ; .pgaw:ReportBuilder.c create text 141 51 -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -anchor nw -text {city} -tags {f-city t_f rg_detail mov ro} ; .pgaw:ReportBuilder.c create text 231 35 -font -Adobe-Helvetica-Bold-R-Normal--*-120-*-*-*-*-*-* -anchor nw -text {phone_nr} -tags {t_l mov ro} ; .pgaw:ReportBuilder.c create text 231 51 -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -anchor nw -text {phone_nr} -tags {f-phone_nr t_f rg_detail mov ro}	\N	\N
\.
COPY "phonebook" FROM stdin;
FIAT	623463445		t	europe
Gelu Voican	01-32234	Bucuresti	f	europe
Radu Vasile	01-5523423	Bucuresti	f	europe
MUGADUMBU SRL	+92 534662634	Cairo	t	africa
Jimmy Page	66323452		f	europe
IBM	623346234	\N	t	usa
John Doe	+44 35 2993825	Washington	f	usa
Bill Clinton	+44 35 9283845	New York	f	usa
Monica Levintchi	+44 38 5234526	Dallas	f	usa
Bill Gates	+42 64 4523454	Los Angeles	f	usa
COMPAQ	623462345	\N	t	usa
SUN	784563253	\N	t	usa
DIGITAL	922644516	\N	t	usa
Frank Zappa	6734567	Montreal	f	usa
Constantin Teodorescu	+40 39 611820	Braila	f	europe
Ngbendu Wazabanga	34577345		f	africa
Mugabe Kandalam	7635745		f	africa
Vasile Lupu	52345623	Bucuresti	f	europe
Gica Farafrica	+42 64 4523454	Los Angeles	f	usa
Victor Ciorbea	634567	Bucuresti	f	europe
\.
COPY "pga_layout" FROM stdin;
pga_forms	2	formname formsource	82 713
Usaisti	5	name phone_nr city company continent	150 150 150 150 150
q1	5	name phone_nr city company continent	150 150 150 150 150
view_saved_from_that_query	5	name phone_nr city company continent	150 150 150 150 150
phonebook	5	name phone_nr city company continent	150 105 80 66 104
Query that can be saved as view	5	name phone_nr city company continent	150 150 150 150 150
pg_database	4	datname datdba encoding datpath	150 150 150 150
pg_language	5	lanname lanispl lanpltrusted lanplcallfoid lancompiler	150 150 150 150 150
cities	3	id name prefix	60 150 150
	3	id name prefix	125 150 150
	3	id name prefix	150 150 150
	3	id name prefix	150 150 150
	3	id name prefix	150 150 150
\.
COPY "pga_schema" FROM stdin;
Simple schema	cities 10 10 phonebook 201.0 84.0	{cities name phonebook city}
\.
COPY "cities" FROM stdin;
3	Braila	4039
4	Galati	4036
5	Dallas	5362
6	Cairo	9352
1	Bucuresti	4013
7	Montreal	5325
\.
CREATE UNIQUE INDEX "cities_id_key" on "cities" using btree ( "id" "int4_ops" );
