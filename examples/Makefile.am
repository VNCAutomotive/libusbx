AM_CPPFLAGS = -I$(top_srcdir)/libusb
LDADD = ../libusb/libusb-1.0.la

noinst_PROGRAMS = listdevs xusb

if HAVE_SIGACTION
noinst_PROGRAMS += dpfp
endif

if THREADS_POSIX
if HAVE_SIGACTION
dpfp_threaded_CFLAGS = $(THREAD_CFLAGS) $(AM_CFLAGS)
noinst_PROGRAMS += dpfp_threaded
endif
endif
