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

PWD = `pwd`


#
# Test that the 10.4 and 10.5 ways to re-export a dylib work
#

all-check: all check

check:
	./main4
	./main5

all: main4 main5

libbar4.dylib : bar.c
	${CC} bar.c -dynamiclib -o $(PWD)/libbar4.dylib -mmacosx-version-min=10.4

libfoo4.dylib : foo.c libbar4.dylib
	${CC} foo.c -dynamiclib libbar4.dylib -sub_library libbar4 -install_name $(PWD)/libfoo4.dylib -o libfoo4.dylib -mmacosx-version-min=10.4

main4 : main.c libfoo4.dylib
	${CC} main.c -I${TESTROOT}/include -o main4 libfoo4.dylib -mmacosx-version-min=10.4



libbar5.dylib : bar.c
	${CC} bar.c -dynamiclib -o $(PWD)/libbar5.dylib -mmacosx-version-min=10.5

libfoo5.dylib : foo.c libbar5.dylib
	${CC} foo.c -dynamiclib libbar5.dylib -sub_library libbar5 -install_name $(PWD)/libfoo5.dylib -o libfoo5.dylib -mmacosx-version-min=10.5

main5 : main.c libfoo5.dylib
	${CC} main.c -I${TESTROOT}/include -o main5 libfoo5.dylib -mmacosx-version-min=10.5

	
clean:
	rm -rf main4 main5 libfoo4.dylib libfoo5.dylib libbar4.dylib libbar5.dylib
	