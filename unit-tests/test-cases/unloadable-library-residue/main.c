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
#include <stdio.h>
#include <string.h>
#include <mach-o/dyld.h>

#include "test.h" // PASS(), FAIL()


int main()
{
	// load libfoo which depends on libbar
	const struct mach_header* mh = NSAddImage("libfoo.dylib", NSADDIMAGE_OPTION_RETURN_ON_ERROR);
	if ( mh != NULL ) {
		FAIL("library-cant-be-bound: NSAddImage should have failed");
		return 1;
	}
	
	 uint32_t count = _dyld_image_count();
	 for(uint32_t i=0; i < count; ++i) {
		const char*  name = _dyld_get_image_name(i);
		if (strcmp(name, "libfoo.dylib") == 0 ) {
			FAIL("library-cant-be-bound: libfoo.dylib shows up in list of images");
			return 0;
		}
	 }

	
	PASS("library-cant-be-bound");
	return 0;
}