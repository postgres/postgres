namespace eval Preferences {

proc {load} {} {
global PgAcVar
	setDefaultFonts
	setGUIPreferences
	# Set some default values for preferences
	set PgAcVar(pref,rows) 200
	set PgAcVar(pref,tvfont) clean
	set PgAcVar(pref,autoload) 1
	set PgAcVar(pref,systemtables) 0
	set PgAcVar(pref,lastdb) {}
	set PgAcVar(pref,lasthost) localhost
	set PgAcVar(pref,lastport) 5432
	set PgAcVar(pref,username) {}
	set PgAcVar(pref,password) {}
	set PgAcVar(pref,language) english
	set retval [catch {set fid [open "~/.pgaccessrc" r]} errmsg]
	if {! $retval} {
		while {![eof $fid]} {
			set pair [gets $fid]
			set PgAcVar([lindex $pair 0]) [lindex $pair 1]
		}
		close $fid
		setGUIPreferences
	}
	# The following preferences values will be ignored from the .pgaccessrc file
	set PgAcVar(pref,typecolors) {black red brown #007e00 #004e00 blue orange yellow pink purple cyan  magenta lightblue lightgreen gray lightyellow}
	set PgAcVar(pref,typelist) {text bool bytea float8 float4 int4 char name int8 int2 int28 regproc oid tid xid cid}
	loadInternationalMessages
}
	
	
proc {save} {} {
global PgAcVar
	catch {
		set fid [open "~/.pgaccessrc" w]
		foreach key [array names PgAcVar pref,*] { puts $fid "$key {$PgAcVar($key)}" }
		close $fid
	}
	if {$PgAcVar(activetab)=="Tables"} {
		Mainlib::tab_click Tables
	}
}

proc {configure} {} {
global PgAcVar
	Window show .pgaw:Preferences
	foreach language  [lsort $PgAcVar(AVAILABLE_LANGUAGES)] {.pgaw:Preferences.fpl.flb.llb insert end $language}
	wm transient .pgaw:Preferences .pgaw:Main
}


proc {loadInternationalMessages} {} {
global Messages PgAcVar
	set PgAcVar(AVAILABLE_LANGUAGES) {english}
	foreach filename [glob -nocomplain [file join $PgAcVar(PGACCESS_HOME) lib languages *]] {
		lappend PgAcVar(AVAILABLE_LANGUAGES) [file tail $filename]
	}
	catch { unset Messages }
	catch { source [file join $PgAcVar(PGACCESS_HOME) lib languages $PgAcVar(pref,language)] }
}


proc {changeLanguage} {} {
global PgAcVar
	set sel [.pgaw:Preferences.fpl.flb.llb curselection]
	if {$sel==""} {return}
	set desired [.pgaw:Preferences.fpl.flb.llb get $sel]
	if {$desired==$PgAcVar(pref,language)} {return}
	set PgAcVar(pref,language) $desired
	loadInternationalMessages
	return
	foreach wid [winfo children .pgaw:Main] {
		set wtext {}
		catch { set wtext [$wid cget -text] }
		if {$wtext != ""} {
			$wid configure -text [intlmsg $wtext]
		}
	}
}


proc {setDefaultFonts} {} {
global PgAcVar tcl_platform
if {[string toupper $tcl_platform(platform)]=="WINDOWS"} {
	set PgAcVar(pref,font_normal) {"MS Sans Serif" 8}
	set PgAcVar(pref,font_bold) {"MS Sans Serif" 8 bold}
	set PgAcVar(pref,font_fix) {Terminal 8}
	set PgAcVar(pref,font_italic) {"MS Sans Serif" 8 italic}
} else {
	set PgAcVar(pref,font_normal) -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
	set PgAcVar(pref,font_bold) -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
	set PgAcVar(pref,font_italic) -Adobe-Helvetica-Medium-O-Normal-*-*-120-*-*-*-*-*
	set PgAcVar(pref,font_fix) -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*
}
}


proc {setGUIPreferences} {} {
global PgAcVar
	foreach wid {Label Text Button Listbox Checkbutton Radiobutton} {
		option add *$wid.font $PgAcVar(pref,font_normal)
	}
	option add *Entry.background #fefefe
	option add *Entry.foreground #000000
	option add *Button.BorderWidth 1
}

}


################### END OF NAMESPACE PREFERENCES #################

proc vTclWindow.pgaw:Preferences {base} {
	if {$base == ""} {
		set base .pgaw:Preferences
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 450x360+100+213
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Preferences"]
	bind $base <Key-Escape> "Window destroy .pgaw:Preferences"
	frame $base.fl \
		-height 75 -relief groove -width 10 
	frame $base.fr \
		-height 75 -relief groove -width 10 
	frame $base.f1 \
		-height 80 -relief groove -width 125 
	label $base.f1.l1 \
		-borderwidth 0 -relief raised \
		-text [intlmsg {Max rows displayed in table/query view}]
	entry $base.f1.erows \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(pref,rows) -width 7 
	frame $base.f2 \
		-height 75 -relief groove -width 125 
	label $base.f2.l \
		-borderwidth 0 -relief raised -text [intlmsg {Table viewer font}]
	label $base.f2.ls \
		-borderwidth 0 -relief raised -text {      } 
	radiobutton $base.f2.pgaw:rb1 \
		-borderwidth 1 -text [intlmsg {fixed width}] -value clean \
		-variable PgAcVar(pref,tvfont) 
	radiobutton $base.f2.pgaw:rb2 \
		-borderwidth 1 -text [intlmsg proportional] -value helv -variable PgAcVar(pref,tvfont) 
	frame $base.ff \
		-height 75 -relief groove -width 125 
	label $base.ff.l1 \
		-borderwidth 0 -relief raised -text [intlmsg {Font normal}]
	entry $base.ff.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(pref,font_normal) \
		-width 200 
	label $base.ff.l2 \
		-borderwidth 0 -relief raised -text [intlmsg {Font bold}]
	entry $base.ff.e2 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(pref,font_bold) \
		-width 200 
	label $base.ff.l3 \
		-borderwidth 0 -relief raised -text [intlmsg {Font italic}]
	entry $base.ff.e3 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(pref,font_italic) \
		-width 200 
	label $base.ff.l4 \
		-borderwidth 0 -relief raised -text [intlmsg {Font fixed}]
	entry $base.ff.e4 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(pref,font_fix) \
		-width 200 
	frame $base.fls \
		-borderwidth 1 -height 2 -relief sunken -width 125 
	frame $base.fal \
		-height 75 -relief groove -width 125 
	checkbutton $base.fal.al \
		-borderwidth 1 -text [intlmsg {Auto-load the last opened database at startup}] \
        -variable PgAcVar(pref,autoload) -anchor w
	checkbutton $base.fal.st \
		-borderwidth 1 -text [intlmsg {View system tables}] \
        -variable PgAcVar(pref,systemtables) -anchor w
	frame $base.fpl \
		-height 49 -relief groove -width 125 
	label $base.fpl.lt \
		-borderwidth 0 -relief raised -text [intlmsg {Preferred language}]
	frame $base.fpl.flb \
		-height 75 -relief sunken -width 125 
	listbox $base.fpl.flb.llb \
		-borderwidth 1 -height 6 -yscrollcommand {.pgaw:Preferences.fpl.flb.vsb set} 
	scrollbar $base.fpl.flb.vsb \
		-borderwidth 1 -command {.pgaw:Preferences.fpl.flb.llb yview} -orient vert 
	frame $base.fb \
        -height 75 -relief groove -width 125 
	button $base.fb.btnsave \
		-command {if {$PgAcVar(pref,rows)>200} {
	tk_messageBox -title [intlmsg Warning] -parent .pgaw:Preferences -message [intlmsg "A big number of rows displayed in table view will take a lot of memory!"]
}
Preferences::changeLanguage
Preferences::save
Window destroy .pgaw:Preferences
tk_messageBox -title [intlmsg Warning] -parent .pgaw:Main -message [intlmsg "Changed fonts may appear in the next working session!"]} \
		-padx 9 -pady 3 -text [intlmsg Save]
	button $base.fb.btncancel \
		-command {Window destroy .pgaw:Preferences} -padx 9 -pady 3 -text [intlmsg Cancel]
	pack $base.fl \
		-in .pgaw:Preferences -anchor center -expand 0 -fill y -side left 
	pack $base.fr \
		-in .pgaw:Preferences -anchor center -expand 0 -fill y -side right 
	pack $base.f1 \
		-in .pgaw:Preferences -anchor center -expand 0 -fill x -pady 5 -side top 
	pack $base.f1.l1 \
		-in .pgaw:Preferences.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f1.erows \
		-in .pgaw:Preferences.f1 -anchor center -expand 0 -fill none -side left 
	pack $base.f2 \
		-in .pgaw:Preferences -anchor center -expand 0 -fill x -pady 5 -side top 
	pack $base.f2.l \
		-in .pgaw:Preferences.f2 -anchor center -expand 0 -fill none -side left 
	pack $base.f2.ls \
		-in .pgaw:Preferences.f2 -anchor center -expand 0 -fill none -side left 
	pack $base.f2.pgaw:rb1 \
		-in .pgaw:Preferences.f2 -anchor center -expand 0 -fill none -side left 
	pack $base.f2.pgaw:rb2 \
		-in .pgaw:Preferences.f2 -anchor center -expand 0 -fill none -side left 
	pack $base.ff \
		-in .pgaw:Preferences -anchor center -expand 0 -fill x -side top 
	grid columnconf $base.ff 1 -weight 1
	grid $base.ff.l1 \
		-in .pgaw:Preferences.ff -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.ff.e1 \
		-in .pgaw:Preferences.ff -column 1 -row 0 -columnspan 1 -rowspan 1 -pady 1 
	grid $base.ff.l2 \
		-in .pgaw:Preferences.ff -column 0 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.ff.e2 \
		-in .pgaw:Preferences.ff -column 1 -row 2 -columnspan 1 -rowspan 1 -pady 1 
	grid $base.ff.l3 \
		-in .pgaw:Preferences.ff -column 0 -row 4 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.ff.e3 \
		-in .pgaw:Preferences.ff -column 1 -row 4 -columnspan 1 -rowspan 1 -pady 1 
	grid $base.ff.l4 \
		-in .pgaw:Preferences.ff -column 0 -row 6 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.ff.e4 \
		-in .pgaw:Preferences.ff -column 1 -row 6 -columnspan 1 -rowspan 1 -pady 1 
	pack $base.fls \
		-in .pgaw:Preferences -anchor center -expand 0 -fill x -pady 5 -side top 
	pack $base.fal \
		-in .pgaw:Preferences -anchor center -expand 0 -fill x -side top 
	pack $base.fal.al \
		-in .pgaw:Preferences.fal -anchor center -expand 0 -fill x -side top -anchor w 
	pack $base.fal.st \
		-in .pgaw:Preferences.fal -anchor center -expand 0 -fill x -side top -anchor w
	pack $base.fpl \
		-in .pgaw:Preferences -anchor center -expand 0 -fill x -side top 
	pack $base.fpl.lt \
		-in .pgaw:Preferences.fpl -anchor center -expand 0 -fill none -side top 
	pack $base.fpl.flb \
		-in .pgaw:Preferences.fpl -anchor center -expand 0 -fill none -side top 
	pack $base.fpl.flb.llb \
		-in .pgaw:Preferences.fpl.flb -anchor center -expand 0 -fill none -side left 
	pack $base.fpl.flb.vsb \
		-in .pgaw:Preferences.fpl.flb -anchor center -expand 0 -fill y -side right 
	pack $base.fb \
		-in .pgaw:Preferences -anchor center -expand 0 -fill none -side bottom 
	grid $base.fb.btnsave \
		-in .pgaw:Preferences.fb -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fb.btncancel \
		-in .pgaw:Preferences.fb -column 1 -row 0 -columnspan 1 -rowspan 1 
}

