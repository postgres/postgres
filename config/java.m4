#
# Autoconf macros for configuring the build of Java JDBC Tools
#
# $Header: /cvsroot/pgsql/config/Attic/java.m4,v 1.3 2001/07/04 21:22:55 petere Exp $
#


# _PGAC_PROG_ANT_WORKS
# --------------------
AC_DEFUN([_PGAC_PROG_ANT_WORKS],
[
  AC_CACHE_CHECK([whether $ANT works], [pgac_cv_prog_ant_works],
  [
    cat > conftest.java << EOF
public class conftest {
    int testmethod(int a, int b) {
        return a + b;
    }
}
EOF

    cat > conftest.xml << EOF
<project name="conftest" default="conftest">
 <target name="conftest">
  <javac srcdir="." includes="conftest.java">
  </javac>
 </target>
</project>
EOF

    pgac_cmd='$ANT -buildfile conftest.xml 1>&2'
    AC_TRY_EVAL(pgac_cmd)
    pgac_save_status=$?
    if test $? = 0 && test -f ./conftest.class ; then
      pgac_cv_prog_ant_works=yes
    else
      echo "configure: failed java program was:" >&AC_FD_CC
      cat conftest.java >&AC_FD_CC
      echo "configure: failed build file was:" >&AC_FD_CC
      cat conftest.xml >&AC_FD_CC
      pgac_cv_prog_ant_works=no
    fi

    rm -f conftest* core core.* *.core
  ])

  if test "$pgac_cv_prog_ant_works" != yes; then
    AC_MSG_ERROR([ant does not work])
  fi
])


# PGAC_PATH_ANT
# -------------
# Look for the ANT tool and set the output variable 'ANT' to 'ant'
# if found, empty otherwise
AC_DEFUN([PGAC_PATH_ANT],
[
  AC_PATH_PROGS(ANT, [jakarta-ant ant ant.sh ant.bat])
  _PGAC_PROG_ANT_WORKS
])
