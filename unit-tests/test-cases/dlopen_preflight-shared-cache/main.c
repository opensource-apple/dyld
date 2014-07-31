/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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
#include <stdbool.h>  // fprintf(), NULL
#include <stdio.h>  // fprintf(), NULL
#include <stdlib.h> // exit(), EXIT_SUCCESS
#include <stdbool.h>
#include <string.h>
#include <mach-o/dyld.h>  
#include <mach-o/dyld_priv.h>  
#include <dlfcn.h>  

#include "test.h" // PASS(), FAIL(), XPASS(), XFAIL()



int main(int argc, const char* argv[])
{
	// test that libbar which links with libz.dylib preflights
	if ( ! dlopen_preflight("./libbar.dylib") ) {
		FAIL("dlopen_preflight-basic libbar.dylib should be loadable, but got: %s", dlerror());
		exit(0);
	}

	// test that libz.dylib itself preflights
	if ( ! dlopen_preflight("/usr/lib/libz.dylib") ) {
		FAIL("dlopen_preflight-basic /usr/lib/libz.dylib should be loadable, but got: %s", dlerror());
		exit(0);
	}

	// verify libbar and libz are no longer loaded
	uint32_t count = _dyld_image_count();
	for (uint32_t i=0; i < count; ++i) {
		const char* path = _dyld_get_image_name(i);
		if ( strstr(path, "libz.") != NULL ) {
			FAIL("dlopen_preflight-shared-cache: %s is loaded", path);
			exit(0);
		}
	}

	
	PASS("dlopen_preflight-shared-cache");
		
	return EXIT_SUCCESS;
}
