dnl
dnl Enable all known GCC compiler warnings, except for those
dnl we can't yet cope with
dnl
AC_DEFUN([GTK_VNC_COMPILE_WARNINGS],[
    dnl ******************************
    dnl More compiler warnings
    dnl ******************************

    AC_ARG_ENABLE([werror],
                  AS_HELP_STRING([--enable-werror], [Use -Werror (if supported)]),
                  [set_werror="$enableval"],
                  [if test -d $srcdir/.git; then
                     is_git_version=true
                     set_werror=yes
                   else
                     set_werror=no
                   fi])

    # List of warnings that are not relevant / wanted

    # Don't care about C++ compiler compat
    dontwarn="$dontwarn -Wc++-compat"
    dontwarn="$dontwarn -Wabi"
    dontwarn="$dontwarn -Wdeprecated"
    # Don't care about ancient C standard compat
    dontwarn="$dontwarn -Wtraditional"
    # Don't care about ancient C standard compat
    dontwarn="$dontwarn -Wtraditional-conversion"
    # Ignore warnings in /usr/include
    dontwarn="$dontwarn -Wsystem-headers"
    # Happy for compiler to add struct padding
    dontwarn="$dontwarn -Wpadded"
    # GCC very confused with -O2
    dontwarn="$dontwarn -Wunreachable-code"
    # Due to d3des violations
    dontwarn="$dontwarn -Wstrict-overflow"
    # Allow vars decl in the middle of blocks
    dontwarn="$dontwarn -Wdeclaration-after-statement"
    # Using long long is fine
    dontwarn="$dontwarn -Wlong-long"
    # Some code issues
    dontwarn="$dontwarn -Wunsafe-loop-optimizations"
    # Flaw in gtkitemfactory.h
    dontwarn="$dontwarn -Wstrict-prototypes"
    # Generated vncmarshal.c file :-(
    dontwarn="$dontwarn -Wunused-macros"
    # glib atomic access
    dontwarn="$dontwarn -Wbad-function-cast"
    # Broken code in glib2's gparam.h
    dontwarn="$dontwarn -Wshift-overflow=2"
    # CLang trips up on this see bz #765389
    dontwarn="$dontwarn -Wcast-align"
    # Get all possible GCC warnings
    gl_MANYWARN_ALL_GCC([maybewarn])

    # Remove the ones we don't want, blacklisted earlier
    gl_MANYWARN_COMPLEMENT([wantwarn], [$maybewarn], [$dontwarn])

    # Check for $CC support of each warning
    for w in $wantwarn; do
      gl_WARN_ADD([$w])
    done

    gl_WARN_ADD([-Wno-sign-conversion])
    gl_WARN_ADD([-Wno-conversion])
    gl_WARN_ADD([-Wno-sign-compare])

    # GNULIB expects this to be part of -Wc++-compat, but we turn
    # that one off, so we need to manually enable this again
    gl_WARN_ADD([-Wjump-misses-init])

    # This should be < 256 really. Currently we're down to 4096,
    # but using 1024 bytes sized buffers (mostly for virStrerror)
    # stops us from going down further
    gl_WARN_ADD([-Wframe-larger-than=4096])

    # Use improved glibc headers
    AH_VERBATIM([FORTIFY_SOURCE],
    [/* Enable compile-time and run-time bounds-checking, and some warnings,
        without upsetting newer glibc. */
     #if defined __OPTIMIZE__ && __OPTIMIZE__
     # define _FORTIFY_SOURCE 2
     #endif
    ])

    # Extra special flags
    dnl -fstack-protector stuff passes gl_WARN_ADD with gcc
    dnl on Mingw32, but fails when actually used
    case $host in
       *-*-linux*)
       dnl Fedora only uses -fstack-protector, but doesn't seem to
       dnl be great overhead in adding -fstack-protector-all instead
       dnl gl_WARN_ADD([-fstack-protector])
       gl_WARN_ADD([-fstack-protector-all])
       gl_WARN_ADD([--param=ssp-buffer-size=4])
       ;;
    esac
    gl_WARN_ADD([-fexceptions])
    gl_WARN_ADD([-fasynchronous-unwind-tables])
    gl_WARN_ADD([-fdiagnostics-show-option])
    gl_WARN_ADD([-funit-at-a-time])

    # Need -fipa-pure-const in order to make -Wsuggest-attribute=pure
    # fire even without -O.
    gl_WARN_ADD([-fipa-pure-const])

    # We should eventually enable this, but right now there are at
    # least 75 functions triggering warnings.
    gl_WARN_ADD([-Wno-suggest-attribute=pure])
    gl_WARN_ADD([-Wno-suggest-attribute=const])

    # GLib deprecated some APIs with no ABI compatible
    # replacement. As such we can't change & must ignore
    # the warnings
    gl_WARN_ADD([-Wno-deprecated-declarations])

    if test "$set_werror" = "yes"
    then
      gl_WARN_ADD([-Werror])
    fi

    WARN_LDFLAGS=$WARN_CFLAGS
    AC_SUBST([WARN_CFLAGS])
    AC_SUBST([WARN_LDFLAGS])
])
