namespace eval Scripts {

proc {new} {} {
	design {}
}


proc {open} {scriptname} {
global CurrentDB
	set ss {}
	wpg_select $CurrentDB "select * from pga_scripts where scriptname='$scriptname'" rec {
		set ss $rec(scriptsource)
	}
	if {[string length $ss] > 0} {
		eval $ss
	}
}


proc {design} {scriptname} {
global PgAcVar CurrentDB
	Window show .pgaw:Scripts
	set PgAcVar(script,name) $scriptname
	.pgaw:Scripts.src delete 1.0 end
	if {[string length $scriptname]==0} return;
	wpg_select $CurrentDB "select * from pga_scripts where scriptname='$scriptname'" rec {
		.pgaw:Scripts.src insert end $rec(scriptsource)    
	}
}


proc {execute} {scriptname} {
	# a wrap for execute command
	open $scriptname
}


proc {save} {} {
global PgAcVar
	if {$PgAcVar(script,name)==""} {
		tk_messageBox -title [intlmsg Warning] -parent .pgaw:Scripts -message [intlmsg "The script must have a name!"]
	} else {
	   sql_exec noquiet "delete from pga_scripts where scriptname='$PgAcVar(script,name)'"
	   regsub -all {\\} [.pgaw:Scripts.src get 1.0 end] {\\\\} PgAcVar(script,body)
	   regsub -all ' $PgAcVar(script,body)  \\' PgAcVar(script,body)
	   sql_exec noquiet "insert into pga_scripts values ('$PgAcVar(script,name)','$PgAcVar(script,body)')"
	   Mainlib::tab_click Scripts
	}
}

}


########################## END OF NAMESPACE SCRIPTS ##################

proc vTclWindow.pgaw:Scripts {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:Scripts
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 594x416+192+152
	wm maxsize $base 1009 738
	wm minsize $base 300 300
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base [intlmsg "Design script"]
	frame $base.f1  -height 55 -relief groove -width 125 
	label $base.f1.l1  -borderwidth 0 -text [intlmsg {Script name}]
	entry $base.f1.e1  -background #fefefe -borderwidth 1 -highlightthickness 0 -textvariable PgAcVar(script,name) -width 32 
	text $base.src -background #fefefe -foreground #000000 -font $PgAcVar(pref,font_normal) -height 2  -highlightthickness 1 -selectborderwidth 0 -width 2 
	frame $base.f2  -height 75 -relief groove -width 125 
	button $base.f2.b1  -borderwidth 1 -command {Window destroy .pgaw:Scripts} -text [intlmsg Cancel]
	button $base.f2.b2  -borderwidth 1  -command Scripts::save \
		-text [intlmsg Save] -width 6 
	pack $base.f1  -in .pgaw:Scripts -anchor center -expand 0 -fill x -pady 2 -side top 
	pack $base.f1.l1  -in .pgaw:Scripts.f1 -anchor center -expand 0 -fill none -ipadx 2 -side left 
	pack $base.f1.e1  -in .pgaw:Scripts.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.src  -in .pgaw:Scripts -anchor center -expand 1 -fill both -padx 2 -side top 
	pack $base.f2  -in .pgaw:Scripts -anchor center -expand 0 -fill none -side top 
	pack $base.f2.b1  -in .pgaw:Scripts.f2 -anchor center -expand 0 -fill none -side right 
	pack $base.f2.b2  -in .pgaw:Scripts.f2 -anchor center -expand 0 -fill none -side right
}

