#
# Autoconf macros for configuring the build of Java JDBC Tools
#
# $Header: /cvsroot/pgsql/config/Attic/java.m4,v 1.4 2002/03/29 17:32:54 petere Exp $
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
      echo "configure: failed java program was:" >&AS_MESSAGE_LOG_FD
      cat conftest.java >&AS_MESSAGE_LOG_FD
      echo "configure: failed build file was:" >&AS_MESSAGE_LOG_FD
      cat conftest.xml >&AS_MESSAGE_LOG_FD
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
