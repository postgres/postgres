namespace eval Users {

proc {new} {} {
global PgAcVar
	Window show .pgaw:User
	wm transient .pgaw:User .pgaw:Main
	set PgAcVar(user,action) "CREATE"
	set PgAcVar(user,name) {}
	set PgAcVar(user,password) {}
	set PgAcVar(user,createdb) NOCREATEDB
	set PgAcVar(user,createuser) NOCREATEUSER
	set PgAcVar(user,verifypassword) {}
	set PgAcVar(user,validuntil) {}
	focus .pgaw:User.e1
}

proc {design} {username} {
global PgAcVar CurrentDB
	Window show .pgaw:User
	tkwait visibility .pgaw:User
	wm transient .pgaw:User .pgaw:Main
	wm title .pgaw:User [intlmsg "Change user"]
	set PgAcVar(user,action) "ALTER"
	set PgAcVar(user,name) $username
	set PgAcVar(user,password) {} ; set PgAcVar(user,verifypassword) {}
	pg_select $CurrentDB "select *,date(valuntil) as valdata from pg_user where usename='$username'" tup {
		if {$tup(usesuper)=="t"} {
			set PgAcVar(user,createuser) CREATEUSER
		} else {
			set PgAcVar(user,createuser) NOCREATEUSER
		}
		if {$tup(usecreatedb)=="t"} {
			set PgAcVar(user,createdb) CREATEDB
		} else {
			set PgAcVar(user,createdb) NOCREATEDB
		}
		if {$tup(valuntil)!=""} {
			set PgAcVar(user,validuntil) $tup(valdata)
		} else {
			set PgAcVar(user,validuntil) {}
		}
	}
	.pgaw:User.e1 configure -state disabled
	.pgaw:User.b1 configure -text [intlmsg Save]
	focus .pgaw:User.e2
}

proc {save} {} {
global PgAcVar CurrentDB
	set PgAcVar(user,name) [string trim $PgAcVar(user,name)]
	set PgAcVar(user,password) [string trim $PgAcVar(user,password)]
	set PgAcVar(user,verifypassword) [string trim $PgAcVar(user,verifypassword)]
	if {$PgAcVar(user,name)==""} {
		showError [intlmsg "User without name?"]
		focus .pgaw:User.e1
		return
	}
	if {$PgAcVar(user,password)!=$PgAcVar(user,verifypassword)} {
		showError [intlmsg "Passwords do not match!"]
		set PgAcVar(user,password) {} ; set PgAcVar(user,verifypassword) {}
		focus .pgaw:User.e2
		return
	}
	set cmd "$PgAcVar(user,action) user \"$PgAcVar(user,name)\""
	if {$PgAcVar(user,password)!=""} {
		set cmd "$cmd WITH PASSWORD '$PgAcVar(user,password)' "
	}
	set cmd "$cmd $PgAcVar(user,createdb) $PgAcVar(user,createuser)"
	if {$PgAcVar(user,validuntil)!=""} {
		set cmd "$cmd VALID UNTIL '$PgAcVar(user,validuntil)'"
	}
	if {[sql_exec noquiet $cmd]} {
		Window destroy .pgaw:User
		Mainlib::cmd_Users
	}
}

}

proc vTclWindow.pgaw:User {base} {
	if {$base == ""} {
		set base .pgaw:User
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 263x220+233+165
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Define new user"]
	label $base.l1 \
		-borderwidth 0 -anchor w -text [intlmsg "User name"]
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(user,name) 
	bind $base.e1 <Key-Return> "focus .pgaw:User.e2"
	bind $base.e1 <Key-KP_Enter> "focus .pgaw:User.e2"
	label $base.l2 \
		-borderwidth 0 -text [intlmsg Password]
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -show * -textvariable PgAcVar(user,password) 
	bind $base.e2 <Key-Return> "focus .pgaw:User.e3"
	bind $base.e2 <Key-KP_Enter> "focus .pgaw:User.e3"
	label $base.l3 \
		-borderwidth 0 -text [intlmsg {verify password}]
	entry $base.e3 \
		-background #fefefe -borderwidth 1 -show * -textvariable PgAcVar(user,verifypassword) 
	bind $base.e3 <Key-Return> "focus .pgaw:User.cb1"
	bind $base.e3 <Key-KP_Enter> "focus .pgaw:User.cb1"
	checkbutton $base.cb1 \
		-borderwidth 1 -offvalue NOCREATEDB -onvalue CREATEDB \
		-text [intlmsg {Allow user to create databases}] -variable PgAcVar(user,createdb) 
	checkbutton $base.cb2 \
		-borderwidth 1 -offvalue NOCREATEUSER -onvalue CREATEUSER \
		-text [intlmsg {Allow user to create other users}] -variable PgAcVar(user,createuser) 
	label $base.l4 \
		-borderwidth 0 -anchor w -text [intlmsg {Valid until (date)}]
	entry $base.e4 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(user,validuntil)
	bind $base.e4 <Key-Return> "focus .pgaw:User.b1"
	bind $base.e4 <Key-KP_Enter> "focus .pgaw:User.b1"
	button $base.b1 \
		-borderwidth 1 -command Users::save -text [intlmsg Create]
	button $base.b2 \
		-borderwidth 1 -command {Window destroy .pgaw:User} -text [intlmsg Cancel]
	place $base.l1 \
		-x 5 -y 7 -height 16 -anchor nw -bordermode ignore 
	place $base.e1 \
		-x 109 -y 5 -width 146 -height 20 -anchor nw -bordermode ignore 
	place $base.l2 \
		-x 5 -y 35 -anchor nw -bordermode ignore 
	place $base.e2 \
		-x 109 -y 32 -width 146 -height 20 -anchor nw -bordermode ignore 
	place $base.l3 \
		-x 5 -y 60 -anchor nw -bordermode ignore 
	place $base.e3 \
		-x 109 -y 58 -width 146 -height 20 -anchor nw -bordermode ignore 
	place $base.cb1 \
		-x 5 -y 90 -anchor nw -bordermode ignore 
	place $base.cb2 \
		-x 5 -y 115 -anchor nw -bordermode ignore 
	place $base.l4 \
		-x 5 -y 145 -height 16 -anchor nw -bordermode ignore 
	place $base.e4 \
		-x 110 -y 143 -width 146 -height 20 -anchor nw -bordermode ignore 
	place $base.b1 \
		-x 45 -y 185 -anchor nw -width 70 -height 25 -bordermode ignore 
	place $base.b2 \
		-x 140 -y 185 -anchor nw -width 70 -height 25 -bordermode ignore 
}

