AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_builddir)/include
AM_CFLAGS = -fno-strict-aliasing

lib_LTLIBRARIES = liblttng-ust-libc-wrapper.la
liblttng_ust_libc_wrapper_la_SOURCES = \
	lttng-ust-malloc.c \
	ust_libc.h
liblttng_ust_libc_wrapper_la_LIBADD = -ldl \
	-L$(top_builddir)/liblttng-ust/.libs \
	-llttng-ust

noinst_SCRIPTS = run
EXTRA_DIST = run
