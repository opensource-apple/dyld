/*
 * Copyright (c) 2005-2007 Apple Inc. All rights reserved.
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
#include <stdlib.h> // EXIT_SUCCESS
#include <mach-o/dyld.h>

#include "test.h"

//
// This builds an executable that is just big enough to force dyld to slide a bit
//
#if __arm__
	#define ARRAY_SIZE  66582528
#else
	#define ARRAY_SIZE 335400000
#endif

//int bigarray1[ARRAY_SIZE];

int
main()
{
	// call a dyld function that will internally throw an exception to test dyld slide properly
	NSAddImage("/foo/bar", NSADDIMAGE_OPTION_RETURN_ON_ERROR);
	
	return EXIT_SUCCESS;
}


