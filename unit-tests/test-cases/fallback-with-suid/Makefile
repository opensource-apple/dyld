##
# Copyright (c) 2006 Apple Computer, Inc. All rights reserved.
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

all-check: all check

check:
	${TESTROOT}/bin/exit-non-zero-pass.pl "fallback-with-suid" "fallback-with-suid" ./main-suid
	
all: main-suid

main: main.c libfoo.dylib
	${CC} ${CCFLAGS} -I${TESTROOT}/include -o main main.c libfoo.dylib
	
main-suid: main
	cp main main-suid
	sudo chown root main-suid
	sudo chmod 4755 main-suid

libfoo.dylib : foo.c
	${CC} ${CCFLAGS} foo.c -dynamiclib -o libfoo.dylib -install_name /bogus/libz.dylib

clean:
	${RM} ${RMFLAGS} *~ main main-suid libfoo.dylib

