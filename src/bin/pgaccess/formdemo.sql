CREATE TABLE pga_queries (queryname varchar(64), querytype char(1), querycommand text);
CREATE TABLE pga_forms (formname varchar(64), formsource text);
CREATE TABLE pga_scripts (scriptname varchar(64), scriptsource text);
CREATE TABLE pga_reports (reportname varchar(64), reportsource text, reportbody text, reportprocs text, reportoptions text);
CREATE TABLE phonebook (name varchar(32), phone_nr varchar(16), city varchar(32), company bool, continent char16);
CREATE TABLE pga_layout (tablename varchar(64), nrcols int2, colnames text, colwidth text);
COPY pga_queries FROM stdin;
\.
COPY pga_forms FROM stdin;
Phone book	pb 26 {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26} 444x310+284+246 {label label1 {33 12 63 30} {} Name {}} {entry name_entry {87 9 217 30} {} entry2 pbqs(name)} {label label3 {33 39 73 54} {} Phone {}} {entry entry4 {87 36 195 57} {} entry4 pbqs(phone_nr)} {label label5 {33 66 78 84} {} City {}} {entry entry6 {87 63 195 84} {} entry6 pbqs(city)} {query qs {3 6 33 33} {} query7 {}} {button button8 {126 177 198 203} {.pb.qs:setsql "select oid,* from phonebook where name ~* '$what' order by name"\
.pb.qs:open\
set nrecs [.pb.qs:nrecords]\
.pb.qs:updatecontrols\
.pb.qs:fill .pb.allnames name} Find {}} {button button9 {159 276 229 302} {.pb.qs:close\
.pb.qs:clearcontrols\
set nrecs {}\
set what {}\
destroy .pb\
} Exit {}} {button button10 {102 249 124 269} {.pb.qs:movefirst\
.pb.qs:updatecontrols\
} |< {}} {button button11 {129 249 151 269} {.pb.qs:moveprevious\
.pb.qs:updatecontrols\
} << {}} {button button12 {156 249 178 269} {.pb.qs:movenext\
.pb.qs:updatecontrols} >> {}} {button button13 {183 249 205 269} {.pb.qs:movelast\
.pb.qs:updatecontrols\
} >| {}} {checkbox checkbox14 {33 87 126 105} {} {Is it a company ?} pbqs(company)} {radio usa {63 108 201 120} {} U.S.A. pbqs(continent)} {radio europe {63 126 204 141} {} Europe pbqs(continent)} {radio africa {63 144 210 159} {} Africa pbqs(continent)} {entry entry18 {30 180 117 198} {} entry18 what} {label label19 {108 219 188 234} {} {records found} {}} {label label20 {90 219 105 234} {} { } nrecs} {label label21 {3 252 33 267} {} OID= {}} {label label22 {39 252 87 267} {} { } pbqs(oid)} {button button23 {9 276 79 302} {set oid {}\
catch {set oid $pbqs(oid)}\
if {[string trim $oid]!=""} {\
   sql_exec noquiet "update phonebook set name='$pbqs(name)', phone_nr='$pbqs(phone_nr)',city='$pbqs(city)',company='$pbqs(company)',continent='$pbqs(continent)' where oid=$oid"\
} else {\
  tk_messageBox -title Error -message "No record is displayed!"\
}\
\
} Update {}} {button button24 {84 276 154 302} {set thisname {}\
catch {set thisname $pbqs(name)}\
if {[string trim $thisname]!=""} {\
sql_exec noquiet "insert into phonebook values ('$pbqs(name)','$pbqs(phone_nr)','$pbqs(city)','$pbqs(company)','$pbqs(continent)')"\
tk_messageBox -title Information -message "A new record has been added!"\
} else {\
  tk_messageBox -title Error -message "This one doesn't have a name ?\
}\
\
} {Add new} {}} {button button25 {168 111 231 135} {.pb.qs:clearcontrols\
# clearcontrols stillinitialise\
# incorectly booleans controls to {}\
# so I force it to 'f' (false)\
set pbqs(company) f\
focus .pb.name_entry} New {}} {listbox allnames {246 12 432 240} {} listbox26 {}}
A simple demo form	asdf 14 {1 2 3 4 5 6 7 8 9 10 11 12 13 14} 377x315+170+155 {label label1 {15 36 99 57} {} {Selected color} {}} {entry entry2 {111 36 225 54} {} entry2 color} {radio red {249 21 342 36} {} {Red as cherry} color} {radio green {249 45 342 60} {} {Green as a melon} color} {radio blue {249 69 342 84} {} {Blue as the sky} color} {button button6 {45 69 198 99} {set color spooky} {Set a weird color} {}} {label label7 {24 129 138 147} {} {The checkbox's value} {}} {entry entry8 {162 129 172 147} {} entry8 cbvalue} {checkbox checkbox9 {180 126 279 150} {} {Check me :-)} cbvalue} {button button10 {219 273 366 303} {destroy .asdf} {Close that simple form} {}} {button button11 {219 237 366 267} {open_form "Phone book"} {Open my phone book} {}} {listbox lb {12 192 162 267} {} listbox12 {}} {button button13 {12 156 162 186} {.asdf.lb insert end red green blue cyan white navy black purple maroon violet} {Add some information} {}} {button button14 {12 273 162 303} {.asdf.lb delete 0 end} {Clear this listbox} {}}
\.
COPY pga_scripts FROM stdin;
How are forms keeped inside ?	open_table pga_forms\
\

\.
COPY pga_reports FROM stdin;
\.
COPY phonebook FROM stdin;
IBM	623346234	\N	t	usa
John Doe	+44 35 2993825	Washington	f	usa
Bill Clinton	+44 35 9283845	New York	f	usa
Monica Levintchi	+44 38 5234526	Dallas	f	usa
Bill Gates	+42 64 4523454	Los Angeles	f	usa
COMPAQ	623462345	\N	t	usa
SUN	784563253	\N	t	usa
DIGITAL	922644516	\N	t	usa
FIAT	623463445	\N	t	europe
MUGADUMBU	+92 534662634	\N	t	africa
Frank Zappa	6734567	Montreal	f	usa
Jimmy Page	66323452		f	europe
Constantin Teodorescu	+40 39 611820	Braila	f	europe
NGBENDU Wazabanga	34577345	\N	f	africa
\.
COPY pga_layout FROM stdin;
pga_forms	2	formname formsource	82 713
phonebook	5	name phone_nr city company continent	150 105 80 66 85
\.
