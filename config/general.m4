# config/general.m4

# This file defines new macros to process configure command line
# arguments, to replace the brain-dead AC_ARG_WITH and AC_ARG_ENABLE.
# The flaw in these is particularly that they only differentiate
# between "given" and "not given" and do not provide enough help to
# process arguments that only accept "yes/no", that require an
# argument (other than "yes/no"), etc.
#
# The point of this implementation is to reduce code size and
# redundancy in configure.ac and to improve robustness and consistency
# in the option evaluation code.


# Convert type and name to shell variable name (e.g., "enable_long_strings")
m4_define([pgac_arg_to_variable],
          [$1[]_[]patsubst($2, -, _)])


# PGAC_ARG(TYPE, NAME, HELP-STRING-LHS-EXTRA, HELP-STRING-RHS,
#          [ACTION-IF-YES], [ACTION-IF-NO], [ACTION-IF-ARG],
#          [ACTION-IF-OMITTED])
# ------------------------------------------------------------
# This is the base layer. TYPE is either "with" or "enable", depending
# on what you like.  NAME is the rest of the option name.
# HELP-STRING-LHS-EXTRA is a string to append to the option name on
# the left-hand side of the help output, e.g., an argument name.  If
# set to "-", append nothing, but let the option appear in the
# negative form (disable/without).  HELP-STRING-RHS is the option
# description, for the right-hand side of the help output.
# ACTION-IF-YES is executed if the option is given without an argument
# (or "yes", which is the same); similar for ACTION-IF-NO.

AC_DEFUN([PGAC_ARG],
[
m4_case([$1],

enable, [
AC_ARG_ENABLE([$2], [AS_HELP_STRING([--]m4_if($3, -, disable, enable)[-$2]m4_if($3, -, , $3), [$4])], [
  case [$]enableval in
    yes)
      m4_default([$5], :)
      ;;
    no)
      m4_default([$6], :)
      ;;
    *)
      $7
      ;;
  esac
],
[$8])[]dnl AC_ARG_ENABLE
],

with, [
AC_ARG_WITH([$2], [AS_HELP_STRING([--]m4_if($3, -, without, with)[-$2]m4_if($3, -, , $3), [$4])], [
  case [$]withval in
    yes)
      m4_default([$5], :)
      ;;
    no)
      m4_default([$6], :)
      ;;
    *)
      $7
      ;;
  esac
],
[$8])[]dnl AC_ARG_WITH
],

[m4_fatal([first argument of $0 must be 'enable' or 'with', not '$1'])]
)
])# PGAC_ARG


# PGAC_ARG_BOOL(TYPE, NAME, DEFAULT, HELP-STRING-RHS,
#               [ACTION-IF-YES], [ACTION-IF-NO])
# ---------------------------------------------------
# Accept a boolean option, that is, one that only takes yes or no.
# ("no" is equivalent to "disable" or "without"). DEFAULT is what
# should be done if the option is omitted; it should be "yes" or "no".
# (Consequently, one of ACTION-IF-YES and ACTION-IF-NO will always
# execute.)

AC_DEFUN([PGAC_ARG_BOOL],
[dnl The following hack is necessary because in a few instances this
dnl macro is called twice for the same option with different default
dnl values.  But we only want it to appear once in the help.  We achieve
dnl that by making the help string look the same, which is why we need to
dnl save the default that was passed in previously.
m4_define([_pgac_helpdefault], m4_ifdef([pgac_defined_$1_$2_bool], [m4_defn([pgac_defined_$1_$2_bool])], [$3]))dnl
PGAC_ARG([$1], [$2], [m4_if(_pgac_helpdefault, yes, -)], [$4], [$5], [$6],
          [AC_MSG_ERROR([no argument expected for --$1-$2 option])],
          [m4_case([$3],
                   yes, [pgac_arg_to_variable([$1], [$2])=yes
$5],
                   no,  [pgac_arg_to_variable([$1], [$2])=no
$6],
                   [m4_fatal([third argument of $0 must be 'yes' or 'no', not '$3'])])])[]dnl
m4_define([pgac_defined_$1_$2_bool], [$3])dnl
])# PGAC_ARG_BOOL


# PGAC_ARG_REQ(TYPE, NAME, HELP-ARGNAME, HELP-STRING-RHS,
#              [ACTION-IF-GIVEN], [ACTION-IF-NOT-GIVEN])
# -------------------------------------------------------
# This option will require an argument; "yes" or "no" will not be
# accepted.  HELP-ARGNAME is a name for the argument for the help output.

AC_DEFUN([PGAC_ARG_REQ],
[PGAC_ARG([$1], [$2], [=$3], [$4],
          [AC_MSG_ERROR([argument required for --$1-$2 option])],
          [AC_MSG_ERROR([argument required for --$1-$2 option])],
          [$5],
          [$6])])# PGAC_ARG_REQ


# PGAC_ARG_OPTARG(TYPE, NAME, HELP-ARGNAME, HELP-STRING-RHS,
#                 [DEFAULT-ACTION], [ARG-ACTION],
#                 [ACTION-ENABLED], [ACTION-DISABLED])
# ----------------------------------------------------------
# This will create an option that behaves as follows: If omitted, or
# called with "no", then set the enable_variable to "no" and do
# nothing else. If called with "yes", then execute DEFAULT-ACTION. If
# called with argument, set enable_variable to "yes" and execute
# ARG-ACTION. Additionally, execute ACTION-ENABLED if we ended up with
# "yes" either way, else ACTION-DISABLED.
#
# The intent is to allow enabling a feature, and optionally pass an
# additional piece of information.

AC_DEFUN([PGAC_ARG_OPTARG],
[PGAC_ARG([$1], [$2], [@<:@=$3@:>@], [$4], [$5], [],
          [pgac_arg_to_variable([$1], [$2])=yes
$6],
          [pgac_arg_to_variable([$1], [$2])=no])
dnl Add this code only if there's a ACTION-ENABLED or ACTION-DISABLED.
m4_ifval([$7[]$8],
[
if test "[$]pgac_arg_to_variable([$1], [$2])" = yes; then
  m4_default([$7], :)
m4_ifval([$8],
[else
  $8
])[]dnl
fi
])[]dnl
])# PGAC_ARG_OPTARG
