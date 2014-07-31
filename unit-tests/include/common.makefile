# stuff to include in every test Makefile

SHELL = /bin/sh

# set default OS to be current Mac OS X 
OS_NAME ?= MacOSX
ifeq "$(OS_NAME)" "iPhoneOS"
	OS_VERSION ?= 3.1
	ifeq "$(OS_VERSION)" "4.3"
		OS_BAROLO_FEATURES = 1
	endif
	ARCH ?= armv6
	VALID_ARCHS ?= armv6
else
	OS_VERSION ?= 10.7
	ifeq "$(OS_VERSION)" "10.7"
		OS_BAROLO_FEATURES = 1
	endif
	ARCH ?= $(shell arch)
	VALID_ARCHS ?= "i386 x86_64"
endif

ifeq "$(OS_NAME)" "iPhoneOS"
	CC			= /Developer/Platforms/iPhoneOS.platform/Developer/usr/bin/gcc-4.2 -arch ${ARCH} -miphoneos-version-min=$(OS_VERSION) -isysroot /Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS4.3.Internal.sdk
	CXX			= /Developer/Platforms/iPhoneOS.platform/Developer/usr/bin/g++-4.2 -arch ${ARCH} -miphoneos-version-min=$(OS_VERSION) -isysroot /Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS4.3.Internal.sdk
#	CC			= gcc-4.2 -arch ${ARCH} -miphoneos-version-min=$(OS_VERSION)
#	CXX			= g++-4.2 -arch ${ARCH} -miphoneos-version-min=$(OS_VERSION)
else
	CC			= gcc-4.2 -arch ${ARCH} -mmacosx-version-min=$(OS_VERSION)
	CXX			= g++-4.2 -arch ${ARCH} -mmacosx-version-min=$(OS_VERSION)
endif

CCFLAGS		= -Wall -std=c99
CXXFLAGS	= -Wall 

RM      = rm
RMFLAGS = -rf

SAFE_RUN	= ${TESTROOT}/bin/fail-if-non-zero.pl
PASS_IFF	= ${TESTROOT}/bin/pass-iff-exit-zero.pl
PASS_IFF_FAILURE = $(TESTROOT)/bin/exit-non-zero-pass.pl

ifeq ($(ARCH),armv7)
  CCFLAGS += -mno-thumb
  CXXFLAGS += -mno-thumb
  override FILEARCH = arm
else
  FILEARCH = $(ARCH)
endif

ifeq ($(ARCH),thumb)
  CCFLAGS += -mthumb
  CXXFLAGS += -mthumb
  override ARCH = armv6
  override FILEARCH = arm
else
  FILEARCH = $(ARCH)
endif

ifeq ($(ARCH),thumb2)
  CCFLAGS += -mthumb
  CXXFLAGS += -mthumb
  override ARCH = armv7
  override FILEARCH = arm
else
  FILEARCH = $(ARCH)
endif
