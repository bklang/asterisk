#
# Asterisk -- A telephony toolkit for Linux.
# 
# Top level Makefile
#
# Copyright (C) 1999-2006, Digium, Inc.
#
# Mark Spencer <markster@digium.com>
#
# This program is free software, distributed under the terms of
# the GNU General Public License
#

# All Makefiles use the following variables:
#
# ASTCFLAGS - compiler options provided by the user (if any)
# _ASTCFLAGS - compiler options provided by the build system
# ASTLDFLAGS - linker flags (not libraries) provided by the user (if any)
# _ASTLDFLAGS - linker flags (not libraries) provided by the build system
# LIBS - additional libraries, at top-level for all links,
#      on a single object just for that object
# SOLINK - linker flags used only for creating shared objects (.so files),
#      used for all .so links
#
# Values for ASTCFLAGS and ASTLDFLAGS can be specified in the
# environment when running make, as follows:
#
#	$ ASTCFLAGS="-Werror" make ...
#
# or as a variable value on the make command line itself:
#
#	$ make ASTCFLAGS="-Werror" ...

export ASTTOPDIR		# Top level dir, used in subdirs' Makefiles
export ASTERISKVERSION
export ASTERISKVERSIONNUM

#--- values used for default paths

# DESTDIR is the staging (or final) directory where files are copied
# during the install process. Define it before 'export', otherwise
# export will set it to the empty string making ?= fail.
# WARNING: do not put spaces or comments after the value.
DESTDIR?=$(INSTALL_PATH)
export DESTDIR

export INSTALL_PATH	# Additional prefix for the following paths
export ASTETCDIR		# Path for config files
export ASTVARRUNDIR
export MODULES_DIR
export ASTSPOOLDIR
export ASTVARLIBDIR
export ASTDATADIR
export ASTDBDIR
export ASTLOGDIR
export ASTLIBDIR
export ASTMANDIR
export ASTHEADERDIR
export ASTBINDIR
export ASTSBINDIR
export AGI_DIR
export ASTCONFPATH

export OSARCH			# Operating system
export PROC			# Processor type

export NOISY_BUILD		# Used in Makefile.rules
export MENUSELECT_CFLAGS	# Options selected in menuselect.
export AST_DEVMODE		# Set to "yes" for additional compiler
                                # and runtime checks

export SOLINK			# linker flags for shared objects
export STATIC_BUILD		# Additional cflags, set to -static
                                # for static builds. Probably
                                # should go directly to ASTLDFLAGS

#--- paths to various commands
export CC
export CXX
export AR
export RANLIB
export HOST_CC
export INSTALL
export STRIP
export DOWNLOAD
export AWK
export GREP
export ID
export MD5
export WGET_EXTRA_ARGS

# even though we could use '-include makeopts' here, use a wildcard
# lookup anyway, so that make won't try to build makeopts if it doesn't
# exist (other rules will force it to be built if needed)
ifneq ($(wildcard makeopts),)
  include makeopts
endif

# start the primary CFLAGS and LDFLAGS with any that were provided
# to the configure script
_ASTCFLAGS:=$(CONFIG_CFLAGS)
_ASTLDFLAGS:=$(CONFIG_LDFLAGS)

# Some build systems, such as the one in openwrt, like to pass custom target
# CFLAGS and LDFLAGS in the COPTS and LDOPTS variables; these should also
# go before any build-system computed flags, since they are defaults, not
# overrides
_ASTCFLAGS+=$(COPTS)
_ASTLDFLAGS+=$(LDOPTS)

# libxml2 cflags
_ASTCFLAGS+=$(LIBXML2_INCLUDE)

#Uncomment this to see all build commands instead of 'quiet' output
#NOISY_BUILD=yes

empty:=
space:=$(empty) $(empty)
ASTTOPDIR:=$(subst $(space),\$(space),$(CURDIR))

# Overwite config files on "make samples"
OVERWRITE=y

# Include debug and macro symbols in the executables (-g) and profiling info (-pg)
DEBUG=-g3


# Define standard directories for various platforms
# These apply if they are not redefined in asterisk.conf 
ifeq ($(OSARCH),SunOS)
  ASTETCDIR=/var/etc/asterisk
  ASTLIBDIR=/opt/asterisk/lib
  ASTVARLIBDIR=/var/opt/asterisk
  ASTDBDIR=$(ASTVARLIBDIR)
  ASTKEYDIR=$(ASTVARLIBDIR)
  ASTSPOOLDIR=/var/spool/asterisk
  ASTLOGDIR=/var/log/asterisk
  ASTHEADERDIR=/opt/asterisk/include
  ASTBINDIR=/opt/asterisk/bin
  ASTSBINDIR=/opt/asterisk/sbin
  ASTVARRUNDIR=/var/run/asterisk
  ASTMANDIR=/opt/asterisk/man
else
  ASTETCDIR=$(sysconfdir)/asterisk
  ASTLIBDIR=$(libdir)/asterisk
  ASTHEADERDIR=$(includedir)/asterisk
  ASTBINDIR=$(bindir)
  ASTSBINDIR=$(sbindir)
  ASTSPOOLDIR=$(localstatedir)/spool/asterisk
  ASTLOGDIR=$(localstatedir)/log/asterisk
  ASTVARRUNDIR=$(localstatedir)/run/asterisk
  ASTMANDIR=$(mandir)
ifneq ($(findstring BSD,$(OSARCH)),)
  ASTVARLIBDIR=$(prefix)/share/asterisk
  ASTVARRUNDIR=$(localstatedir)/run/asterisk
  ASTDBDIR=$(localstatedir)/db/asterisk
else
  ASTVARLIBDIR=$(localstatedir)/lib/asterisk
  ASTDBDIR=$(ASTVARLIBDIR)
endif
  ASTKEYDIR=$(ASTVARLIBDIR)
endif
ifeq ($(ASTDATADIR),)
  ASTDATADIR:=$(ASTVARLIBDIR)
endif

# Asterisk.conf is located in ASTETCDIR or by using the -C flag
# when starting Asterisk
ASTCONFPATH=$(ASTETCDIR)/asterisk.conf
MODULES_DIR=$(ASTLIBDIR)/modules
AGI_DIR=$(ASTDATADIR)/agi-bin

# If you use Apache, you may determine by a grep 'DocumentRoot' of your httpd.conf file
HTTP_DOCSDIR=/var/www/html
# Determine by a grep 'ScriptAlias' of your Apache httpd.conf file
HTTP_CGIDIR=/var/www/cgi-bin

# Uncomment this to use the older DSP routines
#_ASTCFLAGS+=-DOLD_DSP_ROUTINES

# If the file .asterisk.makeopts is present in your home directory, you can
# include all of your favorite menuselect options so that every time you download
# a new version of Asterisk, you don't have to run menuselect to set them. 
# The file /etc/asterisk.makeopts will also be included but can be overridden
# by the file in your home directory.

GLOBAL_MAKEOPTS=$(wildcard /etc/asterisk.makeopts)
USER_MAKEOPTS=$(wildcard ~/.asterisk.makeopts)

MOD_SUBDIR_CFLAGS=-I$(ASTTOPDIR)/include
OTHER_SUBDIR_CFLAGS=-I$(ASTTOPDIR)/include

# Create OPTIONS variable, but probably we can assign directly to ASTCFLAGS
OPTIONS=

ifeq ($(OSARCH),linux-gnu)
  ifeq ($(PROC),x86_64)
    # You must have GCC 3.4 to use k8, otherwise use athlon
    PROC=k8
    #PROC=athlon
  endif

  ifeq ($(PROC),sparc64)
    #The problem with sparc is the best stuff is in newer versions of gcc (post 3.0) only.
    #This works for even old (2.96) versions of gcc and provides a small boost either way.
    #A ultrasparc cpu is really v9 but the stock debian stable 3.0 gcc doesn't support it.
    #So we go lowest common available by gcc and go a step down, still a step up from
    #the default as we now have a better instruction set to work with. - Belgarath
    PROC=ultrasparc
    OPTIONS+=$(shell if $(CC) -mtune=$(PROC) -S -o /dev/null -xc /dev/null >/dev/null 2>&1; then echo "-mtune=$(PROC)"; fi)
    OPTIONS+=$(shell if $(CC) -mcpu=v8 -S -o /dev/null -xc /dev/null >/dev/null 2>&1; then echo "-mcpu=v8"; fi)
    OPTIONS+=-fomit-frame-pointer
  endif

  ifeq ($(PROC),arm)
    # The Cirrus logic is the only heavily shipping arm processor with a real floating point unit
    ifeq ($(SUB_PROC),maverick)
      OPTIONS+=-fsigned-char -mcpu=ep9312
    else
      ifeq ($(SUB_PROC),xscale)
        OPTIONS+=-fsigned-char -mcpu=xscale
      else
        OPTIONS+=-fsigned-char 
      endif
    endif
  endif
endif

ifeq ($(findstring -save-temps,$(_ASTCFLAGS) $(ASTCFLAGS)),)
  ifeq ($(findstring -pipe,$(_ASTCFLAGS) $(ASTCFLAGS)),)
    _ASTCFLAGS+=-pipe
  endif
endif

ifeq ($(findstring -Wall,$(_ASTCFLAGS) $(ASTCFLAGS)),)
  _ASTCFLAGS+=-Wall
endif

_ASTCFLAGS+=-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations $(DEBUG)

ifeq ($(AST_DEVMODE),yes)
  _ASTCFLAGS+=-Werror
  _ASTCFLAGS+=-Wunused
  _ASTCFLAGS+=$(AST_DECLARATION_AFTER_STATEMENT)
  _ASTCFLAGS+=$(AST_FORTIFY_SOURCE)
  _ASTCFLAGS+=-Wundef 
  _ASTCFLAGS+=-Wmissing-format-attribute
  _ASTCFLAGS+=-Wformat=2
endif

ifneq ($(findstring BSD,$(OSARCH)),)
  _ASTCFLAGS+=-isystem /usr/local/include
endif

ifeq ($(findstring -march,$(_ASTCFLAGS) $(ASTCFLAGS)),)
  ifneq ($(PROC),ultrasparc)
    _ASTCFLAGS+=$(shell if $(CC) -march=$(PROC) -S -o /dev/null -xc /dev/null >/dev/null 2>&1; then echo "-march=$(PROC)"; fi)
  endif
endif

ifeq ($(PROC),ppc)
  _ASTCFLAGS+=-fsigned-char
endif

ifeq ($(OSARCH),FreeBSD)
  # -V is understood by BSD Make, not by GNU make.
  BSDVERSION=$(shell make -V OSVERSION -f /usr/share/mk/bsd.port.subdir.mk)
  _ASTCFLAGS+=$(shell if test $(BSDVERSION) -lt 500016 ; then echo "-D_THREAD_SAFE"; fi)
endif

ifeq ($(OSARCH),NetBSD)
  _ASTCFLAGS+=-pthread -I/usr/pkg/include
endif

ifeq ($(OSARCH),OpenBSD)
  _ASTCFLAGS+=-pthread -ftrampolines
endif

ifeq ($(OSARCH),SunOS)
  _ASTCFLAGS+=-Wcast-align -DSOLARIS -I../include/solaris-compat -I/opt/ssl/include -I/usr/local/ssl/include -D_XPG4_2 -D__EXTENSIONS__
endif

ASTERISKVERSION:=$(shell GREP=$(GREP) AWK=$(AWK) build_tools/make_version .)

ifneq ($(wildcard .version),)
  ASTERISKVERSIONNUM:=$(shell $(AWK) -F. '{printf "%01d%02d%02d", $$1, $$2, $$3}' .version)
endif

ifneq ($(wildcard .svn),)
  ASTERISKVERSIONNUM:=999999
endif

_ASTCFLAGS+=$(OPTIONS)

MOD_SUBDIRS:=channels pbx apps codecs formats cdr bridges funcs tests main res $(LOCAL_MOD_SUBDIRS)
OTHER_SUBDIRS:=utils agi
SUBDIRS:=$(OTHER_SUBDIRS) $(MOD_SUBDIRS)
SUBDIRS_INSTALL:=$(SUBDIRS:%=%-install)
SUBDIRS_CLEAN:=$(SUBDIRS:%=%-clean)
SUBDIRS_DIST_CLEAN:=$(SUBDIRS:%=%-dist-clean)
SUBDIRS_UNINSTALL:=$(SUBDIRS:%=%-uninstall)
MOD_SUBDIRS_EMBED_LDSCRIPT:=$(MOD_SUBDIRS:%=%-embed-ldscript)
MOD_SUBDIRS_EMBED_LDFLAGS:=$(MOD_SUBDIRS:%=%-embed-ldflags)
MOD_SUBDIRS_EMBED_LIBS:=$(MOD_SUBDIRS:%=%-embed-libs)
MOD_SUBDIRS_MENUSELECT_TREE:=$(MOD_SUBDIRS:%=%-menuselect-tree)

ifneq ($(findstring darwin,$(OSARCH)),)
  _ASTCFLAGS+=-D__Darwin__
  SOLINK=-dynamic -bundle -Xlinker -macosx_version_min -Xlinker 10.4 -Xlinker -undefined -Xlinker dynamic_lookup -force_flat_namespace
  ifeq ($(shell /usr/bin/sw_vers -productVersion | cut -c1-4),10.6)
    SOLINK+=/usr/lib/bundle1.o
  endif
else
# These are used for all but Darwin
  SOLINK=-shared
  ifneq ($(findstring BSD,$(OSARCH)),)
    _ASTLDFLAGS+=-L/usr/local/lib
  endif
endif

ifeq ($(OSARCH),SunOS)
  SOLINK=-shared -fpic -L/usr/local/ssl/lib -lrt
endif

ifeq ($(OSARCH),OpenBSD)
  SOLINK=-shared -fpic
endif

# comment to print directories during submakes
#PRINT_DIR=yes

SILENTMAKE:=$(MAKE) --quiet --no-print-directory
ifneq ($(PRINT_DIR)$(NOISY_BUILD),)
SUBMAKE:=$(MAKE)
else
SUBMAKE:=$(MAKE) --quiet --no-print-directory
endif

# This is used when generating the doxygen documentation
ifneq ($(DOT),:)
  HAVEDOT=yes
else
  HAVEDOT=no
endif

# $(MAKE) is printed in several places, and we want it to be a
# fixed size string. Define a variable whose name has also the
# same size, so we can easily align text.
ifeq ($(MAKE), gmake)
	mK="gmake"
else
	mK=" make"
endif

all: _all
	@echo " +--------- Asterisk Build Complete ---------+"  
	@echo " + Asterisk has successfully been built, and +"  
	@echo " + can be installed by running:              +"
	@echo " +                                           +"
	@echo " +               $(mK) install               +"  
	@echo " +-------------------------------------------+"  

_all: cleantest makeopts $(SUBDIRS) doc/core-en_US.xml

makeopts: configure
	@echo "****"
	@echo "**** The configure script must be executed before running '$(MAKE)'." 
	@echo "****               Please run \"./configure\"."
	@echo "****"
	@exit 1

menuselect.makeopts: menuselect/menuselect menuselect-tree makeopts build_tools/menuselect-deps $(GLOBAL_MAKEOPTS) $(USER_MAKEOPTS)
ifeq ($(filter %menuselect,$(MAKECMDGOALS)),)
	menuselect/menuselect --check-deps $@
	menuselect/menuselect --check-deps $@ $(GLOBAL_MAKEOPTS) $(USER_MAKEOPTS)
endif

$(MOD_SUBDIRS_EMBED_LDSCRIPT):
	+@echo "EMBED_LDSCRIPTS+="`$(SILENTMAKE) -C $(@:-embed-ldscript=) SUBDIR=$(@:-embed-ldscript=) __embed_ldscript` >> makeopts.embed_rules

$(MOD_SUBDIRS_EMBED_LDFLAGS):
	+@echo "EMBED_LDFLAGS+="`$(SILENTMAKE) -C $(@:-embed-ldflags=) SUBDIR=$(@:-embed-ldflags=) __embed_ldflags` >> makeopts.embed_rules

$(MOD_SUBDIRS_EMBED_LIBS):
	+@echo "EMBED_LIBS+="`$(SILENTMAKE) -C $(@:-embed-libs=) SUBDIR=$(@:-embed-libs=) __embed_libs` >> makeopts.embed_rules

$(MOD_SUBDIRS_MENUSELECT_TREE):
	+@$(SUBMAKE) -C $(@:-menuselect-tree=) SUBDIR=$(@:-menuselect-tree=) moduleinfo
	+@$(SUBMAKE) -C $(@:-menuselect-tree=) SUBDIR=$(@:-menuselect-tree=) makeopts

makeopts.embed_rules: menuselect.makeopts
	@echo "Generating embedded module rules ..."
	@rm -f $@
	+@$(SUBMAKE) $(MOD_SUBDIRS_EMBED_LDSCRIPT)
	+@$(SUBMAKE) $(MOD_SUBDIRS_EMBED_LDFLAGS)
	+@$(SUBMAKE) $(MOD_SUBDIRS_EMBED_LIBS)

$(SUBDIRS): main/version.c include/asterisk/version.h include/asterisk/build.h include/asterisk/buildopts.h defaults.h makeopts.embed_rules

ifeq ($(findstring $(OSARCH), mingw32 cygwin ),)
    # Non-windows:
    # ensure that all module subdirectories are processed before 'main' during
    # a parallel build, since if there are modules selected to be embedded the
    # directories containing them must be completed before the main Asterisk
    # binary can be built
main: $(filter-out main,$(MOD_SUBDIRS))
else
    # Windows: we need to build main (i.e. the asterisk dll) first,
    # followed by res, followed by the other directories, because
    # dll symbols must be resolved during linking and not at runtime.
D1:= $(filter-out main,$(MOD_SUBDIRS))
D1:= $(filter-out res,$(D1))

$(D1): res
res:	main
endif

$(MOD_SUBDIRS):
	+@_ASTCFLAGS="$(MOD_SUBDIR_CFLAGS) $(_ASTCFLAGS)" ASTCFLAGS="$(ASTCFLAGS)" _ASTLDFLAGS="$(_ASTLDFLAGS)" ASTLDFLAGS="$(ASTLDFLAGS)" $(SUBMAKE) --no-builtin-rules -C $@ SUBDIR=$@ all

$(OTHER_SUBDIRS):
	+@_ASTCFLAGS="$(OTHER_SUBDIR_CFLAGS) $(_ASTCFLAGS)" ASTCFLAGS="$(ASTCFLAGS)" _ASTLDFLAGS="$(_ASTLDFLAGS)" ASTLDFLAGS="$(ASTLDFLAGS)" $(SUBMAKE) --no-builtin-rules -C $@ SUBDIR=$@ all

defaults.h: makeopts
	@build_tools/make_defaults_h > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

main/version.c: FORCE
	@build_tools/make_version_c > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

include/asterisk/version.h: FORCE
	@build_tools/make_version_h > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

include/asterisk/buildopts.h: menuselect.makeopts
	@build_tools/make_buildopts_h > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

include/asterisk/build.h:
	@build_tools/make_build_h > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(SUBDIRS_CLEAN):
	+@$(SUBMAKE) -C $(@:-clean=) clean

$(SUBDIRS_DIST_CLEAN):
	+@$(SUBMAKE) -C $(@:-dist-clean=) dist-clean

clean: $(SUBDIRS_CLEAN) _clean

_clean:
	rm -f defaults.h
	rm -f include/asterisk/build.h
	rm -f main/version.c
	rm -f include/asterisk/version.h
	@$(MAKE) -C menuselect clean
	cp -f .cleancount .lastclean

dist-clean: distclean

distclean: $(SUBDIRS_DIST_CLEAN) _clean
	@$(MAKE) -C menuselect dist-clean
	@$(MAKE) -C sounds dist-clean
	rm -f menuselect.makeopts makeopts menuselect-tree menuselect.makedeps
	rm -f makeopts.embed_rules
	rm -f config.log config.status config.cache
	rm -rf autom4te.cache
	rm -f include/asterisk/autoconfig.h
	rm -f include/asterisk/buildopts.h
	rm -rf doc/api
	rm -f build_tools/menuselect-deps

datafiles: _all
	if [ `$(ID) -u` = 0 ]; then \
		CFLAGS="$(_ASTCFLAGS) $(ASTCFLAGS)" build_tools/mkpkgconfig $(DESTDIR)/usr/lib/pkgconfig; \
	fi
# Should static HTTP be installed during make samples or even with its own target ala
# webvoicemail?  There are portions here that *could* be customized but might also be
# improved a lot.  I'll put it here for now.
	mkdir -p $(DESTDIR)$(ASTDATADIR)/static-http
	for x in static-http/*; do \
		$(INSTALL) -m 644 $$x $(DESTDIR)$(ASTDATADIR)/static-http ; \
	done
	if [ -d doc/tex/asterisk ] ; then \
			mkdir -p $(DESTDIR)$(ASTDATADIR)/static-http/docs ; \
			for n in doc/tex/asterisk/* ; do \
				$(INSTALL) -m 644 $$n $(DESTDIR)$(ASTDATADIR)/static-http/docs ; \
			done \
	fi
	mkdir -p $(DESTDIR)$(ASTDATADIR)/images
	for x in images/*.jpg; do \
		$(INSTALL) -m 644 $$x $(DESTDIR)$(ASTDATADIR)/images ; \
	done
	mkdir -p $(DESTDIR)$(AGI_DIR)
	$(MAKE) -C sounds install

doc/core-en_US.xml: $(foreach dir,$(MOD_SUBDIRS),$(shell $(GREP) -l "language=\"en_US\"" $(dir)/*.c $(dir)/*.cc 2>/dev/null))
	@printf "Building Documentation For: "
	@echo "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" > $@
	@echo "<!DOCTYPE docs SYSTEM \"appdocsxml.dtd\">" >> $@
	@echo "<docs xmlns:xi=\"http://www.w3.org/2001/XInclude\">" >> $@
	@for x in $(MOD_SUBDIRS); do \
		printf "$$x " ; \
		for i in $$x/*.c; do \
			$(AWK) -f build_tools/get_documentation $$i >> $@ ; \
		done ; \
	done
	@echo
	@echo "</docs>" >> $@

validate-docs: doc/core-en_US.xml
ifeq ($(XMLSTARLET),:)
	@echo "---------------------------------------------------------------"
	@echo "--- Please install xmlstarlet to validate the documentation ---"
	@echo "---------------------------------------------------------------"
else
	$(XMLSTARLET) val -d doc/appdocsxml.dtd $<
endif

update: 
	@if [ -d .svn ]; then \
		echo "Updating from Subversion..." ; \
		fromrev="`svn info | $(AWK) '/Revision: / {print $$2}'`"; \
		svn update | tee update.out; \
		torev="`svn info | $(AWK) '/Revision: / {print $$2}'`"; \
		echo "`date`  Updated from revision $${fromrev} to $${torev}." >> update.log; \
		rm -f .version; \
		if [ `grep -c ^C update.out` -gt 0 ]; then \
			echo ; echo "The following files have conflicts:" ; \
			grep ^C update.out | cut -b4- ; \
		fi ; \
		rm -f update.out; \
	else \
		echo "Not under version control";  \
	fi

NEWHEADERS=$(notdir $(wildcard include/asterisk/*.h))
OLDHEADERS=$(filter-out $(NEWHEADERS),$(notdir $(wildcard $(DESTDIR)$(ASTHEADERDIR)/*.h)))

installdirs:
	mkdir -p $(DESTDIR)$(MODULES_DIR)
	mkdir -p $(DESTDIR)$(ASTSBINDIR)
	mkdir -p $(DESTDIR)$(ASTETCDIR)
	mkdir -p $(DESTDIR)$(ASTBINDIR)
	mkdir -p $(DESTDIR)$(ASTVARRUNDIR)
	mkdir -p $(DESTDIR)$(ASTSPOOLDIR)/voicemail
	mkdir -p $(DESTDIR)$(ASTSPOOLDIR)/dictate
	mkdir -p $(DESTDIR)$(ASTSPOOLDIR)/system
	mkdir -p $(DESTDIR)$(ASTSPOOLDIR)/tmp
	mkdir -p $(DESTDIR)$(ASTSPOOLDIR)/meetme
	mkdir -p $(DESTDIR)$(ASTSPOOLDIR)/monitor

bininstall: _all installdirs $(SUBDIRS_INSTALL)
	$(INSTALL) -m 755 main/asterisk $(DESTDIR)$(ASTSBINDIR)/
	$(LN) -sf asterisk $(DESTDIR)$(ASTSBINDIR)/rasterisk
	$(INSTALL) -m 755 contrib/scripts/astgenkey $(DESTDIR)$(ASTSBINDIR)/
	$(INSTALL) -m 755 contrib/scripts/autosupport $(DESTDIR)$(ASTSBINDIR)/
	if [ ! -f $(DESTDIR)$(ASTSBINDIR)/safe_asterisk ]; then \
		cat contrib/scripts/safe_asterisk | sed 's|__ASTERISK_SBIN_DIR__|$(ASTSBINDIR)|;s|__ASTERISK_VARRUN_DIR__|$(ASTVARRUNDIR)|;' > $(DESTDIR)$(ASTSBINDIR)/safe_asterisk ;\
		chmod 755 $(DESTDIR)$(ASTSBINDIR)/safe_asterisk;\
	fi
	$(INSTALL) -d $(DESTDIR)$(ASTHEADERDIR)
	$(INSTALL) -m 644 include/asterisk.h $(DESTDIR)$(includedir)
	$(INSTALL) -m 644 include/asterisk/*.h $(DESTDIR)$(ASTHEADERDIR)
	if [ -n "$(OLDHEADERS)" ]; then \
		rm -f $(addprefix $(DESTDIR)$(ASTHEADERDIR)/,$(OLDHEADERS)) ;\
	fi
	mkdir -p $(DESTDIR)$(ASTDATADIR)/documentation
	mkdir -p $(DESTDIR)$(ASTDATADIR)/documentation/thirdparty
	mkdir -p $(DESTDIR)$(ASTLOGDIR)/cdr-csv
	mkdir -p $(DESTDIR)$(ASTLOGDIR)/cdr-custom
	mkdir -p $(DESTDIR)$(ASTDATADIR)/keys
	mkdir -p $(DESTDIR)$(ASTDATADIR)/firmware
	mkdir -p $(DESTDIR)$(ASTDATADIR)/firmware/iax
	mkdir -p $(DESTDIR)$(ASTMANDIR)/man8
	$(INSTALL) -m 644 doc/core-*.xml $(DESTDIR)$(ASTDATADIR)/documentation
	$(INSTALL) -m 644 doc/appdocsxml.dtd $(DESTDIR)$(ASTDATADIR)/documentation
	$(INSTALL) -m 644 keys/iaxtel.pub $(DESTDIR)$(ASTDATADIR)/keys
	$(INSTALL) -m 644 keys/freeworlddialup.pub $(DESTDIR)$(ASTDATADIR)/keys
	$(INSTALL) -m 644 doc/asterisk.8 $(DESTDIR)$(ASTMANDIR)/man8
	$(INSTALL) -m 644 contrib/scripts/astgenkey.8 $(DESTDIR)$(ASTMANDIR)/man8
	$(INSTALL) -m 644 contrib/scripts/autosupport.8 $(DESTDIR)$(ASTMANDIR)/man8
	$(INSTALL) -m 644 contrib/scripts/safe_asterisk.8 $(DESTDIR)$(ASTMANDIR)/man8
	if [ -f contrib/firmware/iax/iaxy.bin ] ; then \
		$(INSTALL) -m 644 contrib/firmware/iax/iaxy.bin $(DESTDIR)$(ASTDATADIR)/firmware/iax/iaxy.bin; \
	fi

$(SUBDIRS_INSTALL):
	+@DESTDIR="$(DESTDIR)" ASTSBINDIR="$(ASTSBINDIR)" $(SUBMAKE) -C $(@:-install=) install

NEWMODS:=$(foreach d,$(MOD_SUBDIRS),$(notdir $(wildcard $(d)/*.so)))
OLDMODS=$(filter-out $(NEWMODS),$(notdir $(wildcard $(DESTDIR)$(MODULES_DIR)/*.so)))

oldmodcheck:
	@if [ -n "$(OLDMODS)" ]; then \
		echo " WARNING WARNING WARNING" ;\
		echo "" ;\
		echo " Your Asterisk modules directory, located at" ;\
		echo " $(DESTDIR)$(MODULES_DIR)" ;\
		echo " contains modules that were not installed by this " ;\
		echo " version of Asterisk. Please ensure that these" ;\
		echo " modules are compatible with this version before" ;\
		echo " attempting to run Asterisk." ;\
		echo "" ;\
		for f in $(OLDMODS); do \
			echo "    $$f" ;\
		done ;\
		echo "" ;\
		echo " WARNING WARNING WARNING" ;\
	fi

badshell:
ifneq ($(findstring ~,$(DESTDIR)),)
	@echo "Your shell doesn't do ~ expansion when expected (specifically, when doing \"make install DESTDIR=~/path\")."
	@echo "Try replacing ~ with \$$HOME, as in \"make install DESTDIR=\$$HOME/path\"."
	@exit 1
endif

install: badshell datafiles bininstall
	@if [ -x /usr/sbin/asterisk-post-install ]; then \
		/usr/sbin/asterisk-post-install $(DESTDIR) . ; \
	fi
	@echo " +---- Asterisk Installation Complete -------+"  
	@echo " +                                           +"
	@echo " +    YOU MUST READ THE SECURITY DOCUMENT    +"
	@echo " +                                           +"
	@echo " + Asterisk has successfully been installed. +"  
	@echo " + If you would like to install the sample   +"  
	@echo " + configuration files (overwriting any      +"
	@echo " + existing config files), run:              +"  
	@echo " +                                           +"
	@echo " +               $(mK) samples               +"
	@echo " +                                           +"
	@echo " +-----------------  or ---------------------+"
	@echo " +                                           +"
	@echo " + You can go ahead and install the asterisk +"
	@echo " + program documentation now or later run:   +"
	@echo " +                                           +"
	@echo " +              $(mK) progdocs               +"
	@echo " +                                           +"
	@echo " + **Note** This requires that you have      +"
	@echo " + doxygen installed on your local system    +"
	@echo " +-------------------------------------------+"
	@$(MAKE) -s oldmodcheck

isntall: install

upgrade: bininstall

# XXX why *.adsi is installed first ?
adsi:
	@echo Installing adsi config files...
	@mkdir -p $(DESTDIR)$(ASTETCDIR)
	@for x in configs/*.adsi; do \
		dst="$(DESTDIR)$(ASTETCDIR)/`$(BASENAME) $$x`" ; \
		if [ -f $${dst} ] ; then \
			echo "Overwriting $$x" ; \
		else \
			echo "Installing $$x" ; \
		fi ; \
		$(INSTALL) -m 644 $$x $(DESTDIR)$(ASTETCDIR)/`$(BASENAME) $$x` ; \
	done

samples: adsi
	@echo Installing other config files...
	@mkdir -p $(DESTDIR)$(ASTETCDIR)
	@for x in configs/*.sample; do \
		dst="$(DESTDIR)$(ASTETCDIR)/`$(BASENAME) $$x .sample`" ;	\
		if [ -f $${dst} ]; then \
			if [ "$(OVERWRITE)" = "y" ]; then \
				if cmp -s $${dst} $$x ; then \
					echo "Config file $$x is unchanged"; \
					continue; \
				fi ; \
				mv -f $${dst} $${dst}.old ; \
			else \
				echo "Skipping config file $$x"; \
				continue; \
			fi ;\
		fi ; \
		echo "Installing file $$x"; \
		$(INSTALL) -m 644 $$x $${dst} ;\
	done
	@if [ "$(OVERWRITE)" = "y" ] || [ ! -f $(DESTDIR)$(ASTCONFPATH) ]; then \
		echo "Creating asterisk.conf"; \
		( \
		echo "[directories](!) ; remove the (!) to enable this" ; \
		echo "astetcdir => $(ASTETCDIR)" ; \
		echo "astmoddir => $(MODULES_DIR)" ; \
		echo "astvarlibdir => $(ASTVARLIBDIR)" ; \
		echo "astdbdir => $(ASTDBDIR)" ; \
		echo "astkeydir => $(ASTKEYDIR)" ; \
		echo "astdatadir => $(ASTDATADIR)" ; \
		echo "astagidir => $(AGI_DIR)" ; \
		echo "astspooldir => $(ASTSPOOLDIR)" ; \
		echo "astrundir => $(ASTVARRUNDIR)" ; \
		echo "astlogdir => $(ASTLOGDIR)" ; \
		echo "" ; \
		echo "[options]" ; \
		echo ";verbose = 3" ; \
		echo ";debug = 3" ; \
		echo ";alwaysfork = yes ; same as -F at startup" ; \
		echo ";nofork = yes ; same as -f at startup" ; \
		echo ";quiet = yes ; same as -q at startup" ; \
		echo ";timestamp = yes ; same as -T at startup" ; \
		echo ";execincludes = yes ; support #exec in config files" ; \
		echo ";console = yes ; Run as console (same as -c at startup)" ; \
		echo ";highpriority = yes ; Run realtime priority (same as -p at startup)" ; \
		echo ";initcrypto = yes ; Initialize crypto keys (same as -i at startup)" ; \
		echo ";nocolor = yes ; Disable console colors" ; \
		echo ";dontwarn = yes ; Disable some warnings" ; \
		echo ";dumpcore = yes ; Dump core on crash (same as -g at startup)" ; \
		echo ";languageprefix = yes ; Use the new sound prefix path syntax" ; \
		echo ";internal_timing = yes" ; \
		echo ";systemname = my_system_name ; prefix uniqueid with a system name for global uniqueness issues" ; \
		echo ";autosystemname = yes ; automatically set systemname to hostname - uses 'localhost' on failure, or systemname if set" ; \
		echo ";maxcalls = 10 ; Maximum amount of calls allowed" ; \
		echo ";maxload = 0.9 ; Asterisk stops accepting new calls if the load average exceed this limit" ; \
		echo ";maxfiles = 1000 ; Maximum amount of openfiles" ; \
		echo ";minmemfree = 1 ; in MBs, Asterisk stops accepting new calls if the amount of free memory falls below this watermark" ; \
		echo ";cache_record_files = yes ; Cache recorded sound files to another directory during recording" ; \
		echo ";record_cache_dir = /tmp ; Specify cache directory (used in conjunction with cache_record_files)" ; \
		echo ";transmit_silence_during_record = yes ; Transmit SLINEAR silence while a channel is being recorded" ; \
		echo ";transmit_silence = yes ; Transmit silence while a channel is in a waiting state, a recording only state, or when DTMF is" ; \
		echo "                        ; being generated.  Note that the silence internally is generated in raw signed linear format." ; \
		echo "                        ; This means that it must be transcoded into the native format of the channel before it can be sent" ; \
		echo "                        ; to the device.  It is for this reason that this is optional, as it may result in requiring a" ; \
		echo "                        ; temporary codec translation path for a channel that may not otherwise require one." ; \
		echo ";transcode_via_sln = yes ; Build transcode paths via SLINEAR, instead of directly" ; \
		echo ";runuser = asterisk ; The user to run as" ; \
		echo ";rungroup = asterisk ; The group to run as" ; \
		echo ";lightbackground = yes ; If your terminal is set for a light-colored background" ; \
		echo "documentation_language = en_US ; Set the Language you want Documentation displayed in. Value is in the same format as locale names" ; \
		echo ";hideconnect = yes ; Hide messages displayed when a remote console connects and disconnects" ; \
		echo "" ; \
		echo "; Changing the following lines may compromise your security." ; \
		echo ";[files]" ; \
		echo ";astctlpermissions = 0660" ; \
		echo ";astctlowner = root" ; \
		echo ";astctlgroup = apache" ; \
		echo ";astctl = asterisk.ctl" ; \
		echo "" ; \
		echo "[compat]" ; \
		echo "pbx_realtime=1.6" ; \
		echo "res_agi=1.6" ; \
		echo "app_set=1.6" ; \
		) > $(DESTDIR)$(ASTCONFPATH) ; \
	else \
		echo "Skipping asterisk.conf creation"; \
	fi
	mkdir -p $(DESTDIR)$(ASTSPOOLDIR)/voicemail/default/1234/INBOX
	build_tools/make_sample_voicemail $(DESTDIR)/$(ASTDATADIR) $(DESTDIR)/$(ASTSPOOLDIR)
	@mkdir -p $(DESTDIR)$(ASTDATADIR)/phoneprov
	@for x in phoneprov/*; do \
		dst="$(DESTDIR)$(ASTDATADIR)/$$x" ;	\
		if [ -f $${dst} ]; then \
			if [ "$(OVERWRITE)" = "y" ]; then \
				if cmp -s $${dst} $$x ; then \
					echo "Config file $$x is unchanged"; \
					continue; \
				fi ; \
				mv -f $${dst} $${dst}.old ; \
			else \
				echo "Skipping config file $$x"; \
				continue; \
			fi ;\
		fi ; \
		echo "Installing file $$x"; \
		$(INSTALL) -m 644 $$x $${dst} ;\
	done

webvmail:
	@[ -d $(DESTDIR)$(HTTP_DOCSDIR)/ ] || ( printf "http docs directory not found.\nUpdate assignment of variable HTTP_DOCSDIR in Makefile!\n" && exit 1 )
	@[ -d $(DESTDIR)$(HTTP_CGIDIR) ] || ( printf "cgi-bin directory not found.\nUpdate assignment of variable HTTP_CGIDIR in Makefile!\n" && exit 1 )
	$(INSTALL) -m 4755 -o root -g root contrib/scripts/vmail.cgi $(DESTDIR)$(HTTP_CGIDIR)/vmail.cgi
	mkdir -p $(DESTDIR)$(HTTP_DOCSDIR)/_asterisk
	for x in images/*.gif; do \
		$(INSTALL) -m 644 $$x $(DESTDIR)$(HTTP_DOCSDIR)/_asterisk/; \
	done
	@echo " +--------- Asterisk Web Voicemail ----------+"  
	@echo " +                                           +"
	@echo " + Asterisk Web Voicemail is installed in    +"
	@echo " + your cgi-bin directory:                   +"
	@echo " + $(DESTDIR)$(HTTP_CGIDIR)"
	@echo " + IT USES A SETUID ROOT PERL SCRIPT, SO     +"
	@echo " + IF YOU DON'T LIKE THAT, UNINSTALL IT!     +"
	@echo " +                                           +"
	@echo " + Other static items have been stored in:   +"
	@echo " + $(DESTDIR)$(HTTP_DOCSDIR)"
	@echo " +                                           +"
	@echo " + If these paths do not match your httpd    +"
	@echo " + installation, correct the definitions     +"
	@echo " + in your Makefile of HTTP_CGIDIR and       +"
	@echo " + HTTP_DOCSDIR                              +"
	@echo " +                                           +"
	@echo " +-------------------------------------------+"  

progdocs:
	(cat contrib/asterisk-ng-doxygen; echo "HAVE_DOT=$(HAVEDOT)"; \
	echo "PROJECT_NUMBER=$(ASTERISKVERSION)") | doxygen - 

install-logrotate:
	if [ ! -d $(ASTETCDIR)/../logrotate.d ]; then \
		mkdir $(ASTETCDIR)/../logrotate.d ; \
	fi
	sed 's#__LOGDIR__#$(ASTLOGDIR)#g' < contrib/scripts/asterisk.logrotate | sed 's#__SBINDIR__#$(ASTSBINDIR)#g' > contrib/scripts/asterisk.logrotate.tmp
	install -m 0644 contrib/scripts/asterisk.logrotate.tmp $(ASTETCDIR)/../logrotate.d/asterisk
	rm -f contrib/scripts/asterisk.logrotate.tmp

config:
	@if [ "${OSARCH}" = "linux-gnu" ]; then \
		if [ -f /etc/redhat-release -o -f /etc/fedora-release ]; then \
			cat contrib/init.d/rc.redhat.asterisk | sed 's|__ASTERISK_ETC_DIR__|$(ASTETCDIR)|;s|__ASTERISK_SBIN_DIR__|$(ASTSBINDIR)|;s|__ASTERISK_VARRUN_DIR__|$(ASTVARRUNDIR)|;' > $(DESTDIR)/etc/rc.d/init.d/asterisk ;\
			chmod 755 $(DESTDIR)/etc/rc.d/init.d/asterisk;\
			if [ -z "$(DESTDIR)" ]; then /sbin/chkconfig --add asterisk; fi; \
		elif [ -f /etc/debian_version ]; then \
			cat contrib/init.d/rc.debian.asterisk | sed 's|__ASTERISK_ETC_DIR__|$(ASTETCDIR)|;s|__ASTERISK_SBIN_DIR__|$(ASTSBINDIR)|;s|__ASTERISK_VARRUN_DIR__|$(ASTVARRUNDIR)|;' > $(DESTDIR)/etc/init.d/asterisk ;\
			chmod 755 $(DESTDIR)/etc/init.d/asterisk;\
			if [ -z "$(DESTDIR)" ]; then /usr/sbin/update-rc.d asterisk defaults 50 91; fi; \
		elif [ -f /etc/gentoo-release ]; then \
			cat contrib/init.d/rc.gentoo.asterisk | sed 's|__ASTERISK_ETC_DIR__|$(ASTETCDIR)|;s|__ASTERISK_SBIN_DIR__|$(ASTSBINDIR)|;s|__ASTERISK_VARRUN_DIR__|$(ASTVARRUNDIR)|;' > $(DESTDIR)/etc/init.d/asterisk ;\
			chmod 755 $(DESTDIR)/etc/init.d/asterisk;\
			if [ -z "$(DESTDIR)" ]; then /sbin/rc-update add asterisk default; fi; \
		elif [ -f /etc/mandrake-release -o -f /etc/mandriva-release ]; then \
			cat contrib/init.d/rc.mandriva.asterisk | sed 's|__ASTERISK_ETC_DIR__|$(ASTETCDIR)|;s|__ASTERISK_SBIN_DIR__|$(ASTSBINDIR)|;s|__ASTERISK_VARRUN_DIR__|$(ASTVARRUNDIR)|;' > $(DESTDIR)/etc/rc.d/init.d/asterisk ;\
			chmod 755 $(DESTDIR)/etc/rc.d/init.d/asterisk;\
			if [ -z "$(DESTDIR)" ]; then /sbin/chkconfig --add asterisk; fi; \
		elif [ -f /etc/SuSE-release -o -f /etc/novell-release ]; then \
			cat contrib/init.d/rc.suse.asterisk | sed 's|__ASTERISK_ETC_DIR__|$(ASTETCDIR)|;s|__ASTERISK_SBIN_DIR__|$(ASTSBINDIR)|;s|__ASTERISK_VARRUN_DIR__|$(ASTVARRUNDIR)|;' > $(DESTDIR)/etc/init.d/asterisk ;\
			chmod 755 $(DESTDIR)/etc/init.d/asterisk;\
			if [ -z "$(DESTDIR)" ]; then /sbin/chkconfig --add asterisk; fi; \
		elif [ -f /etc/arch-release -o -f /etc/arch-release ]; then \
			cat contrib/init.d/rc.archlinux.asterisk | sed 's|__ASTERISK_ETC_DIR__|$(ASTETCDIR)|;s|__ASTERISK_SBIN_DIR__|$(ASTSBINDIR)|;s|__ASTERISK_VARRUN_DIR__|$(ASTVARRUNDIR)|;' > $(DESTDIR)/etc/rc.d/asterisk ;\
			chmod 755 $(DESTDIR)/etc/rc.d/asterisk;\
		elif [ -f /etc/slackware-version ]; then \
			echo "Slackware is not currently supported, although an init script does exist for it."; \
		else \
			echo "We could not install init scripts for your distribution."; \
		fi \
	else \
		echo "We could not install init scripts for your operating system."; \
	fi

sounds:
	$(MAKE) -C sounds all

# If the cleancount has been changed, force a make clean.
# .cleancount is the global clean count, and .lastclean is the 
# last clean count we had

cleantest:
	@cmp -s .cleancount .lastclean || $(MAKE) clean

$(SUBDIRS_UNINSTALL):
	+@$(SUBMAKE) -C $(@:-uninstall=) uninstall

_uninstall: $(SUBDIRS_UNINSTALL)
	rm -f $(DESTDIR)$(MODULES_DIR)/*
	rm -f $(DESTDIR)$(ASTSBINDIR)/*asterisk*
	rm -f $(DESTDIR)$(ASTSBINDIR)/astgenkey
	rm -f $(DESTDIR)$(ASTSBINDIR)/autosupport
	rm -rf $(DESTDIR)$(ASTHEADERDIR)
	rm -rf $(DESTDIR)$(ASTDATADIR)/firmware
	rm -f $(DESTDIR)$(ASTMANDIR)/man8/asterisk.8
	rm -f $(DESTDIR)$(ASTMANDIR)/man8/astgenkey.8
	rm -f $(DESTDIR)$(ASTMANDIR)/man8/autosupport.8
	rm -f $(DESTDIR)$(ASTMANDIR)/man8/safe_asterisk.8
	$(MAKE) -C sounds uninstall

uninstall: _uninstall
	@echo " +--------- Asterisk Uninstall Complete -----+"  
	@echo " + Asterisk binaries, sounds, man pages,     +"  
	@echo " + headers, modules, and firmware builds,    +"  
	@echo " + have all been uninstalled.                +"  
	@echo " +                                           +"
	@echo " + To remove ALL traces of Asterisk,         +"
	@echo " + including configuration, spool            +"
	@echo " + directories, and logs, run the following  +"
	@echo " + command:                                  +"
	@echo " +                                           +"
	@echo " +            $(mK) uninstall-all            +"  
	@echo " +-------------------------------------------+"  

uninstall-all: _uninstall
	rm -rf $(DESTDIR)$(ASTLIBDIR)
	rm -rf $(DESTDIR)$(ASTVARLIBDIR)
	rm -rf $(DESTDIR)$(ASTDATADIR)
	rm -rf $(DESTDIR)$(ASTSPOOLDIR)
	rm -rf $(DESTDIR)$(ASTETCDIR)
	rm -rf $(DESTDIR)$(ASTLOGDIR)

menuconfig: menuselect

cmenuconfig: cmenuselect

gmenuconfig: gmenuselect

nmenuconfig: nmenuselect

menuselect: menuselect/cmenuselect menuselect/nmenuselect menuselect/gmenuselect
	@if [ -x menuselect/nmenuselect ]; then \
		$(MAKE) nmenuselect; \
	elif [ -x menuselect/cmenuselect ]; then \
		$(MAKE) cmenuselect; \
	elif [ -x menuselect/gmenuselect ]; then \
		$(MAKE) gmenuselect; \
	else \
		echo "No menuselect user interface found. Install ncurses,"; \
		echo "newt or GTK libraries to build one and re-rerun"; \
		echo "'make menuselect'."; \
	fi

cmenuselect: menuselect/cmenuselect menuselect-tree menuselect.makeopts
	-@menuselect/cmenuselect menuselect.makeopts && (echo "menuselect changes saved!"; rm -f channels/h323/Makefile.ast main/asterisk) || echo "menuselect changes NOT saved!"

gmenuselect: menuselect/gmenuselect menuselect-tree menuselect.makeopts
	-@menuselect/gmenuselect menuselect.makeopts && (echo "menuselect changes saved!"; rm -f channels/h323/Makefile.ast main/asterisk) || echo "menuselect changes NOT saved!"

nmenuselect: menuselect/nmenuselect menuselect-tree menuselect.makeopts
	-@menuselect/nmenuselect menuselect.makeopts && (echo "menuselect changes saved!"; rm -f channels/h323/Makefile.ast main/asterisk) || echo "menuselect changes NOT saved!"

# options for make in menuselect/
MAKE_MENUSELECT=CC="$(HOST_CC)" CXX="$(CXX)" LD="" AR="" RANLIB="" CFLAGS="" $(MAKE) -C menuselect CONFIGURE_SILENT="--silent"

menuselect/menuselect: menuselect/makeopts
	+$(MAKE_MENUSELECT) menuselect

menuselect/cmenuselect: menuselect/makeopts
	+$(MAKE_MENUSELECT) cmenuselect

menuselect/gmenuselect: menuselect/makeopts
	+$(MAKE_MENUSELECT) gmenuselect

menuselect/nmenuselect: menuselect/makeopts
	+$(MAKE_MENUSELECT) nmenuselect

menuselect/makeopts: makeopts
	+$(MAKE_MENUSELECT) makeopts

menuselect-tree: $(foreach dir,$(filter-out main,$(MOD_SUBDIRS)),$(wildcard $(dir)/*.c) $(wildcard $(dir)/*.cc)) build_tools/cflags.xml build_tools/cflags-devmode.xml sounds/sounds.xml build_tools/embed_modules.xml configure
	@echo "Generating input for menuselect ..."
	@echo "<?xml version=\"1.0\"?>" > $@
	@echo >> $@
	@echo "<menu name=\"Asterisk Module and Build Option Selection\">" >> $@
	+@for dir in $(sort $(filter-out main,$(MOD_SUBDIRS))); do $(SILENTMAKE) -C $${dir} SUBDIR=$${dir} moduleinfo >> $@; done
	@cat build_tools/cflags.xml >> $@
	+@for dir in $(sort $(filter-out main,$(MOD_SUBDIRS))); do $(SILENTMAKE) -C $${dir} SUBDIR=$${dir} makeopts >> $@; done
	@if [ "${AST_DEVMODE}" = "yes" ]; then \
		cat build_tools/cflags-devmode.xml >> $@; \
	fi
	@cat build_tools/embed_modules.xml >> $@
	@cat sounds/sounds.xml >> $@
	@echo "</menu>" >> $@

pdf: asterisk.pdf
asterisk.pdf:
	$(MAKE) -C doc/tex asterisk.pdf

.PHONY: menuselect
.PHONY: main
.PHONY: sounds
.PHONY: clean
.PHONY: dist-clean
.PHONY: distclean
.PHONY: all
.PHONY: prereqs
.PHONY: cleantest
.PHONY: uninstall
.PHONY: _uninstall
.PHONY: uninstall-all
.PHONY: pdf
.PHONY: dont-optimize
.PHONY: badshell
.PHONY: installdirs
.PHONY: validate-docs
.PHONY: _clean
.PHONY: $(SUBDIRS_INSTALL)
.PHONY: $(SUBDIRS_DIST_CLEAN)
.PHONY: $(SUBDIRS_CLEAN)
.PHONY: $(SUBDIRS_UNINSTALL)
.PHONY: $(SUBDIRS)
.PHONY: $(MOD_SUBDIRS_EMBED_LDSCRIPT)
.PHONY: $(MOD_SUBDIRS_EMBED_LDFLAGS)
.PHONY: $(MOD_SUBDIRS_EMBED_LIBS)

FORCE:

