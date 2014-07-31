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
# Test that the 10.4 and 10.5 ways to re-export a sub framework work
#

all-check: all check

check:
	./main4
	./main5

all: main4 main5

Bar4.framework/Bar4 : bar.c
	mkdir -p Bar4.framework
	${CC} bar.c -dynamiclib -install_name $(PWD)/Bar4.framework/Bar4 -o Bar4.framework/Bar4 -umbrella Foo4 -mmacosx-version-min=10.4

Foo4.framework/Foo4 : foo.c Bar4.framework/Bar4
	mkdir -p Foo4.framework
	${CC} foo.c -dynamiclib Bar4.framework/Bar4 -install_name $(PWD)/Foo4.framework/Foo4 -o Foo4.framework/Foo4 -mmacosx-version-min=10.4

main4 : main.c Foo4.framework/Foo4
	${CC} main.c -I${TESTROOT}/include -o main4 -framework Foo4 -F. -mmacosx-version-min=10.4


Bar5.framework/Bar5 : bar.c
	mkdir -p Bar5.framework
	${CC} bar.c -dynamiclib -install_name $(PWD)/Bar5.framework/Bar5  -o Bar5.framework/Bar5 -umbrella Foo5 -mmacosx-version-min=10.5

Foo5.framework/Foo5 : foo.c Bar5.framework/Bar5
	mkdir -p Foo5.framework
	${CC} foo.c -dynamiclib Bar5.framework/Bar5 -install_name $(PWD)/Foo5.framework/Foo5 -o Foo5.framework/Foo5 -mmacosx-version-min=10.5

main5 : main.c Foo5.framework/Foo5
	${CC} main.c -I${TESTROOT}/include -o main5 -framework Foo5 -F. -mmacosx-version-min=10.5


	
clean:
	rm -rf main4 Foo4.framework Bar4.framework main5 Foo5.framework Bar5.framework 
	