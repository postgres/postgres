namespace eval Functions {

proc {new} {} {
global PgAcVar
	Window show .pgaw:Function
	set PgAcVar(function,name) {}
	set PgAcVar(function,nametodrop) {}
	set PgAcVar(function,parameters) {}
	set PgAcVar(function,returns) {}
	set PgAcVar(function,language) {}
	.pgaw:Function.fs.text1 delete 1.0 end
	focus .pgaw:Function.fp.e1
	wm transient .pgaw:Function .pgaw:Main
}


proc {design} {functionname} {
global PgAcVar CurrentDB
	Window show .pgaw:Function
	.pgaw:Function.fs.text1 delete 1.0 end
	wpg_select $CurrentDB "select * from pg_proc where proname='$functionname'" rec {
		set PgAcVar(function,name) $functionname
		set temppar $rec(proargtypes)
		set PgAcVar(function,returns) [Database::getPgType $rec(prorettype)]
		set funcnrp $rec(pronargs)
		set prolanguage $rec(prolang)
		.pgaw:Function.fs.text1 insert end $rec(prosrc)
	}
	wpg_select $CurrentDB "select lanname from pg_language where oid=$prolanguage" rec {
		set PgAcVar(function,language) $rec(lanname)
	}
	if { $PgAcVar(function,language)=="C" || $PgAcVar(function,language)=="c" } {
	    wpg_select $CurrentDB "select probin from pg_proc where proname='$functionname'" rec {
		.pgaw:Function.fs.text1 delete 1.0 end
		.pgaw:Function.fs.text1 insert end $rec(probin)
	    }
	}
	set PgAcVar(function,parameters) {}
	for {set i 0} {$i<$funcnrp} {incr i} {
		lappend PgAcVar(function,parameters) [Database::getPgType [lindex $temppar $i]]
	}
	set PgAcVar(function,parameters) [join $PgAcVar(function,parameters) ,]
	set PgAcVar(function,nametodrop) "$PgAcVar(function,name) ($PgAcVar(function,parameters))"
}


proc {save} {} {
global PgAcVar
	if {$PgAcVar(function,name)==""} {
		focus .pgaw:Function.fp.e1
		showError [intlmsg "You must supply a name for this function!"]
	} elseif {$PgAcVar(function,returns)==""} {
		focus .pgaw:Function.fp.e3
		showError [intlmsg "You must supply a return type!"]
	} elseif {$PgAcVar(function,language)==""} {
		focus .pgaw:Function.fp.e4
		showError [intlmsg "You must supply the function language!"]
	} else {
		set funcbody [.pgaw:Function.fs.text1 get 1.0 end]
		# regsub -all "\n" $funcbody " " funcbody
		regsub -all {'} $funcbody {''} funcbody
		if {$PgAcVar(function,nametodrop) != ""} {
			if {! [sql_exec noquiet "drop function $PgAcVar(function,nametodrop)"]} {
				return
			}
		}
		if {[sql_exec noquiet "create function $PgAcVar(function,name) ($PgAcVar(function,parameters)) returns $PgAcVar(function,returns) as '$funcbody' language '$PgAcVar(function,language)'"]} {
			Window destroy .pgaw:Function
			tk_messageBox -title PostgreSQL -parent .pgaw:Main -message [intlmsg "Function saved!"]
			Mainlib::tab_click Functions
		}						
	}
}

}

proc vTclWindow.pgaw:Function {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:Function
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 480x330+98+212
	wm maxsize $base 1009 738
	wm minsize $base 480 330
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base [intlmsg "Function"]
	bind $base <Key-F1> "Help::load functions"
	frame $base.fp \
		-height 88 -relief groove -width 125 
	label $base.fp.l1 \
		-borderwidth 0 -relief raised -text [intlmsg Name]
	entry $base.fp.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(function,name) 
	bind $base.fp.e1 <Key-Return> {
		focus .pgaw:Function.fp.e2
	}
	label $base.fp.l2 \
		-borderwidth 0 -relief raised -text [intlmsg Parameters]
	entry $base.fp.e2 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(function,parameters) -width 15 
	bind $base.fp.e2 <Key-Return> {
		focus .pgaw:Function.fp.e3
	}
	label $base.fp.l3 \
		-borderwidth 0 -relief raised -text [intlmsg Returns]
	entry $base.fp.e3 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(function,returns) 
	bind $base.fp.e3 <Key-Return> {
		focus .pgaw:Function.fp.e4
	}
	label $base.fp.l4 \
		-borderwidth 0 -relief raised -text [intlmsg Language]
	entry $base.fp.e4 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(function,language) -width 15 
	bind $base.fp.e4 <Key-Return> {
		focus .pgaw:Function.fs.text1
	}
	label $base.fp.lspace \
		-borderwidth 0 -relief raised -text {    } 
	frame $base.fs \
		-borderwidth 2 -height 75 -relief groove -width 125 
	text $base.fs.text1 \
		-background #fefefe -foreground #000000 -borderwidth 1 -font $PgAcVar(pref,font_fix) -height 16 \
		-tabs {20 40 60 80 100 120} -width 43 -yscrollcommand {.pgaw:Function.fs.vsb set} 
	scrollbar $base.fs.vsb \
		-borderwidth 1 -command {.pgaw:Function.fs.text1 yview} -orient vert 
	frame $base.fb \
		-borderwidth 2 -height 75 -width 125 
	frame $base.fb.fbc \
		-borderwidth 2 -height 75 -width 125 
	button $base.fb.fbc.btnsave -command {Functions::save} \
		-borderwidth 1 -padx 9 -pady 3 -text [intlmsg Save]
	button $base.fb.fbc.btnhelp -command {Help::load functions} \
		-borderwidth 1 -padx 9 -pady 3 -text [intlmsg Help]
	button $base.fb.fbc.btncancel \
		-borderwidth 1 -command {Window destroy .pgaw:Function} -padx 9 -pady 3 \
		-text [intlmsg Cancel]
	pack $base.fp \
		-in .pgaw:Function -anchor center -expand 0 -fill x -side top 
	grid $base.fp.l1 \
		-in .pgaw:Function.fp -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.fp.e1 \
		-in .pgaw:Function.fp -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fp.l2 \
		-in .pgaw:Function.fp -column 3 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.fp.e2 \
		-in .pgaw:Function.fp -column 4 -row 0 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.fp.l3 \
		-in .pgaw:Function.fp -column 0 -row 4 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.fp.e3 \
		-in .pgaw:Function.fp -column 1 -row 4 -columnspan 1 -rowspan 1 
	grid $base.fp.l4 \
		-in .pgaw:Function.fp -column 3 -row 4 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.fp.e4 \
		-in .pgaw:Function.fp -column 4 -row 4 -columnspan 1 -rowspan 1 -pady 3 
	grid $base.fp.lspace \
		-in .pgaw:Function.fp -column 2 -row 4 -columnspan 1 -rowspan 1 
	pack $base.fs \
		-in .pgaw:Function -anchor center -expand 1 -fill both -side top 
	pack $base.fs.text1 \
		-in .pgaw:Function.fs -anchor center -expand 1 -fill both -side left 
	pack $base.fs.vsb \
		-in .pgaw:Function.fs -anchor center -expand 0 -fill y -side right 
	pack $base.fb \
		-in .pgaw:Function -anchor center -expand 0 -fill x -side bottom 
	pack $base.fb.fbc \
		-in .pgaw:Function.fb -anchor center -expand 0 -fill none -side top 
	pack $base.fb.fbc.btnsave \
		-in .pgaw:Function.fb.fbc -anchor center -expand 0 -fill none -side left 
	pack $base.fb.fbc.btnhelp \
		-in .pgaw:Function.fb.fbc -anchor center -expand 0 -fill none -side left 
	pack $base.fb.fbc.btncancel \
		-in .pgaw:Function.fb.fbc -anchor center -expand 0 -fill none -side right 
}

