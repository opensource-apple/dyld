##
# Copyright (c) 2005-2007 Apple Inc. All rights reserved.
#
# @APPLE_LICENSE_HEADER_START@
# 
# This file contains Original Code and/or Modifications of Original Code
# as defined in and that are subject to the Apple Public Source License
# Version 2.0 (the 'License'). You may not use this file except in
# compliance with the License. Please obtain a copy of the License at
# http://www.opensource.apple.com/apsl/ and read it before using this
# file.
# 
# The Original Code and all software distributed under the License are
# distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
# EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
# INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
# Please see the License for the specific language governing rights and
# limitations under the License.
# 
# @APPLE_LICENSE_HEADER_END@
##
TESTROOT = ../..
include ${TESTROOT}/include/common.makefile

STACK_BASE = 0x8fe01000

ifeq "armv6" "$(ARCH)"
	STACK_BASE = 0x2fe01000
endif
ifeq "ppc64" "$(ARCH)"
	STACK_BASE = 0x7fff5fc00000
endif
ifeq "x86_64" "$(ARCH)"
	STACK_BASE = 0x7fff5fc00000
endif



all-check: all check

check:
	${TESTROOT}/bin/exit-zero-pass.pl "dyld did slide" "dyld did not slide" ./main

all:
	${CC} ${CCFLAGS} -Wno-deprecated-declarations -I${TESTROOT}/include -o main main.c -Wl,-stack_addr,${STACK_BASE} -Wl,-stack_size,0x00100000

clean:
	${RM} ${RMFLAGS} main
