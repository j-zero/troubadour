#
# Build kernel
#

include Config.mk

OBJS		:=	src/config.o \
			src/control/control.o \
			src/control/mister.o \
			src/control/rotaryencoder.o \
			src/control/simplebuttons.o \
			src/control/simpleencoder.o \
			src/kernel.o \
			src/laststate.o \
			src/lcd/canvas.o \
			src/lcd/drivers/gc9a01.o \
			src/lcd/fonts.o \
			src/lcd/logo.o \
			src/lcd/ui.o \
			src/main.o \
			src/midimonitor.o \
			src/midiparser.o \
			src/mt32pi.o \
			src/net/applemidi.o \
			src/net/ftpdaemon.o \
			src/net/ftpworker.o \
			src/net/udpmidi.o \
			src/pisound.o \
			src/power.o \
			src/soundfontmanager.o \
			src/synth/soundfontsynth.o \
			src/zoneallocator.o

EXTRACLEAN	+=	src/*.d src/*.o \
			src/control/*.d src/control/*.o \
			src/lcd/*.d src/lcd/*.o \
			src/lcd/drivers/*.d src/lcd/drivers/*.o \
			src/net/*.d src/net/*.o \
			src/synth/*.d src/synth/*.o

#
# inih
#
OBJS		+=	$(INIHHOME)/ini.o
INCLUDE		+=	-I $(INIHHOME)
EXTRACLEAN	+=	$(INIHHOME)/ini.d \
			$(INIHHOME)/ini.o

include $(CIRCLEHOME)/Rules.mk

CFLAGS		+=	-Werror -Wextra -Wno-unused-parameter -Wno-array-bounds

CFLAGS		+=	-I "$(NEWLIBDIR)/include" \
			-I $(STDDEF_INCPATH) \
			-I $(CIRCLESTDLIBHOME)/include \
			-I include \
			-I .

LIBS 		:=	$(CIRCLE_STDLIB_LIBS) \
			$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
			$(CIRCLEHOME)/addon/SDCard/libsdcard.a \
			$(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
			$(CIRCLEHOME)/addon/wlan/libwlan.a \
			$(CIRCLEHOME)/lib/fs/libfs.a \
			$(CIRCLEHOME)/lib/input/libinput.a \
			$(CIRCLEHOME)/lib/libcircle.a \
			$(CIRCLEHOME)/lib/net/libnet.a \
			$(CIRCLEHOME)/lib/sched/libsched.a \
			$(CIRCLEHOME)/lib/sound/libsound.a \
			$(CIRCLEHOME)/lib/usb/libusb.a

ifeq ($(HDMI_CONSOLE), 1)
DEFINE		+=	-D HDMI_CONSOLE
endif

-include $(DEPS)

INCLUDE		+=	-I $(FLUIDSYNTHBUILDDIR)/include \
			-I $(FLUIDSYNTHHOME)/include
EXTRALIBS	+=	$(FLUIDSYNTHLIB)

#
# Generate version string from git tag
#
VERSION=$(shell git describe --tags --dirty --always 2>/dev/null)
ifneq ($(VERSION),)
DEFINE		+=	-D MT32_PI_VERSION=\"$(VERSION)\"
endif
