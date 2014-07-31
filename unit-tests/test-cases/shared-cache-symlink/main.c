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
#include <stdint.h>
#include <stdio.h>  // fprintf(), NULL
#include <stdlib.h> // exit(), EXIT_SUCCESS
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

#include "test.h" // PASS(), FAIL(), XPASS(), XFAIL()

// libz.1.dylib should be in the shared cache
// libz.dylib is a symlink to libz.dylib
// we want to verify that dlopening the symlink name will use the one in the shared cache


int main()
{
	void* libzHandle = dlopen("/usr/lib/libz.dylib", RTLD_LAZY);
	if ( libzHandle == NULL ) {
		FAIL("shared-cache-symlink: dlopen(/usr/lib/libz.dylib, RTLD_LAZY) failed: %s", dlerror());
		return EXIT_SUCCESS;
	}
	
	// get address of strcmp()
	void* sym = dlsym(libzHandle, "inflate");
	if ( sym == NULL ) {
		FAIL("shared-cache-symlink: dlsym(handle, \"inflate\") failed");
		return EXIT_SUCCESS;
	}
	
	Dl_info info;
	if ( dladdr(sym, &info) == 0 ) {
		FAIL("shared-cache-symlink: dladdr(sym, xx) failed");
		return EXIT_SUCCESS;
	}

	// walk images to get slide
	uint32_t count = _dyld_image_count();
	for(uint32_t i=0; i < count; ++i) {
		if ( _dyld_get_image_header(i) == info.dli_fbase ) {
			if ( _dyld_get_image_vmaddr_slide(i) == 0 ) {
				// images in shared cache have a slide of zero
				PASS("shared-cache-symlink");
				return EXIT_SUCCESS;
			}
			else {
				FAIL("shared-cache-symlink: libz.dylib not loaded from shared cache");
				return EXIT_SUCCESS;
			}
		}
	 }

	FAIL("shared-cache-symlink libz.dylib not found");
	return EXIT_SUCCESS;
}
