# DVDNAV

LIBDVDNAV_VERSION := 4.2.0
LIBDVDNAV_URL := http://dvdnav.mplayerhq.hu/releases/libdvdnav-$(LIBDVDNAV_VERSION).tar.bz2

ifdef BUILD_DISCS
PKGS += dvdnav
endif
ifeq ($(call need_pkg,"dvdnav"),)
PKGS_FOUND += dvdnav
endif

$(TARBALLS)/libdvdnav-$(LIBDVDNAV_VERSION).tar.bz2:
	$(call download,$(LIBDVDNAV_URL))

.sum-dvdnav: libdvdnav-$(LIBDVDNAV_VERSION).tar.bz2

dvdnav: libdvdnav-$(LIBDVDNAV_VERSION).tar.bz2 .sum-dvdnav
	$(UNPACK)
	$(APPLY) $(SRC)/dvdnav/dvdnav.patch
	$(APPLY) $(SRC)/dvdnav/libdvdnav-searching.c-check-cellnr-before-indexing.patch
	$(MOVE)

DEPS_dvdnav = dvdcss dvdread

.dvdnav: dvdnav .dvdcss .dvdread
	cd $< && sh autogen.sh noconfig
	cd $< && $(HOSTVARS) ./configure $(HOSTCONF)
	cd $< && $(MAKE) install
	touch $@
