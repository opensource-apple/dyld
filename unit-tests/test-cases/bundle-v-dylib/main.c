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
#include <stdbool.h>
#include <stdlib.h>
#include <mach-o/dyld.h>

#include "test.h" // PASS(), FAIL()


void loadAsBundle(const char* path)
{
	NSObjectFileImage ofi;
	if ( NSCreateObjectFileImageFromFile(path, &ofi) == NSObjectFileImageSuccess ) {
		FAIL("NSCreateObjectFileImageFromFile() incorrectly allowed %s to be loaded", path);
		exit(1);
	}
}

void loadAsDylib(const char* path)
{
	if ( NSAddImage(path, NSADDIMAGE_OPTION_RETURN_ON_ERROR) != NULL ) {
		FAIL("NSAddImage() incorrectly allowed %s to be loaded", path);
		exit(1);
	}
}


extern void bar();

int main()
{
	// verify that NSAddImage fails to load MH_BUNDLE
	loadAsDylib("foo.bundle");

	// verify that NSCreateObjectFileImageFromFile fails to load MH_DYLIB
	loadAsBundle("foo.dylib");

	// verify that NSCreateObjectFileImageFromFile fails to load MH_DYLIB already linked against main
	loadAsBundle("bar.dylib");
	// verify that bar.dylib was not unloaded when above failed
	bar();
	
	PASS("bundle-v-dylib");
	return 0;
}