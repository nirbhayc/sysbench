dnl ---------------------------------------------------------------------------
dnl Macro: AC_CHECK_COMDB2
dnl Check for custom Comdb2 paths in --with-comdb2-* options.
dnl ---------------------------------------------------------------------------

AC_DEFUN([AC_CHECK_COMDB2],[

# Check for custom includes path
if test [ -z "$ac_cv_comdb2_includes" ] 
then 
    AC_ARG_WITH([comdb2-includes], 
                AC_HELP_STRING([--with-comdb2-includes], [path to Comdb2 header files]),
                [ac_cv_comdb2_includes=$withval])
fi
if test [ -n "$ac_cv_comdb2_includes" ]
then
    AC_CACHE_CHECK([Comdb2 includes], [ac_cv_comdb2_includes], [ac_cv_comdb2_includes=""])
    COMDB2_CFLAGS="-I$ac_cv_comdb2_includes"
fi

# Check for custom library path

if test [ -z "$ac_cv_comdb2_libs" ]
then
    AC_ARG_WITH([comdb2-libs], 
                AC_HELP_STRING([--with-comdb2-libs], [path to Comdb2 libraries]),
                [ac_cv_comdb2_libs=$withval])
fi

if test [ -n "$ac_cv_comdb2_libs" ]
then
     AC_CACHE_CHECK([Comdb2 libraries], [ac_cv_comdb2_libs], [ac_cv_comdb2_libs=""])
     COMDB2_LIBS="-L$ac_cv_comdb2_libs -l:libcdb2api.a -lssl -lcrypto -lprotobuf-c -lunwind"
fi
])
