/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#include <stdio.h>  // fprintf(), NULL
#include <stdlib.h> // exit(), EXIT_SUCCESS
#include <string.h> 
#include <dlfcn.h> 
#include <mach-o/dyld.h> 

#include "test.h" // PASS(), FAIL(), XPASS(), XFAIL()



int bar()
{
	return 2;
}

static int foo()
{
	return 3;
}

int bar2()
{
	return 4;
}

// checks global symbol
static void verifybar()
{
	Dl_info info;
	if ( dladdr(&bar, &info) == 0 ) {
		FAIL("dladdr(&bar, xx) failed");
		exit(0);
	}
	if ( strcmp(info.dli_sname, "bar") != 0 ) {
		if ( strcmp(info.dli_sname, "_bar") == 0 ) {
			XFAIL("dladdr()->dli_sname is \"%s\" instead of \"bar\"", info.dli_sname);
		} 
		else {
			FAIL("dladdr()->dli_sname is \"%s\" instead of \"bar\"", info.dli_sname);
			exit(0);
		}
	}
	if ( info.dli_saddr != &bar) {
		FAIL("dladdr()->dli_saddr is not &bar");
		exit(0);
	}
	if ( info.dli_fbase != _dyld_get_image_header_containing_address(&bar) ) {
		FAIL("dladdr()->dli_fbase is not image that contains &bar");
		exit(0);
	}
}

// checks local symbol (should resolve to previoius global symbol bar)
static void verifyfoo()
{
	Dl_info info;
	if ( dladdr(&foo, &info) == 0 ) {
		FAIL("dladdr(&foo, xx) failed");
		exit(0);
	}
	if ( strcmp(info.dli_sname, "bar") != 0 ) {
		if ( strcmp(info.dli_sname, "_bar") == 0 ) {
			XFAIL("dladdr()->dli_sname is \"%s\" instead of \"bar\"", info.dli_sname);
		} 
		else {
			FAIL("dladdr()->dli_sname is \"%s\" instead of \"bar\"", info.dli_sname);
			exit(0);
		}
	}
	if ( info.dli_saddr != &bar) {
		FAIL("dladdr()->dli_saddr is not &bar");
		exit(0);
	}
	if ( info.dli_fbase != _dyld_get_image_header_containing_address(&bar) ) {
		FAIL("dladdr()->dli_fbase is not image that contains &bar");
		exit(0);
	}
}


int main()
{
	verifybar();
	verifyfoo();

  
	PASS("dladdr");
	return EXIT_SUCCESS;
}
