
include $(top_srcdir)/build/Makefile.decl

SUBDIRS = . tests

COMMON = $(top_srcdir)/common

AM_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_srcdir)/common \
	-I$(top_srcdir)/p11-kit \
	-DSRCDIR=\"$(srcdir)\" \
	-DPRIVATEDIR=\"$(privatedir)\" \
	-DP11_KIT_FUTURE_UNSTABLE_API \
	$(NULL)

bin_PROGRAMS = \
	p11-kit

p11_kit_SOURCES = \
	list.c \
	tool.c tool.h \
	$(NULL)

p11_kit_CFLAGS = \
	$(NULL)

p11_kit_LDADD = \
	$(top_builddir)/p11-kit/libp11-kit.la \
	$(top_builddir)/common/libp11-common.la \
	$(LTLIBINTL) \
	$(NULL)

if WITH_ASN1

p11_kit_LDADD += \
	$(top_builddir)/common/libp11-data.la \
	$(LIBTASN1_LIBS)

p11_kit_CFLAGS += \
	$(LIBTASN1_CFLAGS)

p11_kit_SOURCES += \
	extract.c extract.h \
	extract-info.c \
	extract-jks.c \
	extract-openssl.c \
	extract-pem.c \
	extract-x509.c \
	save.c save.h \
	$(NULL)

externaldir = $(privatedir)
external_SCRIPTS = \
	p11-kit-extract-trust

endif # WITH_ASN1
