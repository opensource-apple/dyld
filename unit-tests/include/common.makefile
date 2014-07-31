# stuff to include in every test Makefile

SHELL = /bin/sh

# set default to be host
ARCH ?= $(shell arch)

# set default to be all
VALID_ARCHS ?= "ppc ppc64 i386 x86_64"

CC		 = gcc-4.0 -arch ${ARCH}
CCFLAGS = -Wall -std=c99

CXX		  = g++-4.0 -arch ${ARCH}
CXXFLAGS = -Wall 

RM      = rm
RMFLAGS = -rf

SAFE_RUN	= ${TESTROOT}/bin/fail-if-non-zero.pl
