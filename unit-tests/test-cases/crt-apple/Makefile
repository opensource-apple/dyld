##
# Copyright (c) 2007 Apple Inc. All rights reserved.
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

#
# verify that apple[0] parameter is correct by comparing to argv[1]
#

all-check: all check

check:
	./main-10.4 ./main-10.4
	./main-10.5 ./main-10.5
	./main-10.4.stripped ./main-10.4.stripped
	./main-10.5.stripped ./main-10.5.stripped
	`pwd`/main-10.4 `pwd`/main-10.4
	`pwd`/main-10.5 `pwd`/main-10.5
	`pwd`/main-10.4.stripped `pwd`/main-10.4.stripped
	`pwd`/main-10.5.stripped `pwd`/main-10.5.stripped

all: main-10.4 main-10.5 main-10.4.stripped main-10.5.stripped

main-10.4: main.c
	${CC} ${CCFLAGS} -I${TESTROOT}/include -o main-10.4 main.c -mmacosx-version-min=10.4 -w

main-10.4.stripped: main-10.4
	strip main-10.4 -o main-10.4.stripped
	
main-10.5: main.c
	${CC} ${CCFLAGS} -I${TESTROOT}/include -o main-10.5 main.c -mmacosx-version-min=10.5 -w

main-10.5.stripped: main-10.5
	strip main-10.5 -o main-10.5.stripped

clean:
	${RM} ${RMFLAGS} *~ main-10.4 main-10.5 main-10.4.stripped main-10.5.stripped
 

