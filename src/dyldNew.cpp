/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2005 Apple Computer, Inc. All rights reserved.
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

#include <new>
#include <malloc/malloc.h>
//#include <stdio.h>


//
// dyld does not use malloc anywhere, instead C++ new is used.
// All dyld allocations go in dyld-only zone so as to be not co-mingled with target proccess's allocations
//
//
//

static malloc_zone_t* sZone = NULL;  // could be initialized to malloc_create_zone, but that would require careful ordering of initializers


void* operator new(std::size_t len) throw (std::bad_alloc)
{
	if ( sZone == NULL ) {
		sZone = malloc_create_zone(40960, 0);
		malloc_set_zone_name(sZone, "dyld heap");
	}
	//fprintf(stderr, "new(%d)\n", len);
	return malloc_zone_malloc(sZone, len);
}

void* operator new[](std::size_t len) throw (std::bad_alloc)
{
	if ( sZone == NULL ) {
		sZone = malloc_create_zone(40960, 0);
		malloc_set_zone_name(sZone, "dyld heap");
	}
	//fprintf(stderr, "new[](%d)\n", len);
	return malloc_zone_malloc(sZone, len);
}


void operator delete(void* obj) throw()
{
	//fprintf(stderr, "delete(%p)\n", obj);
	malloc_zone_free(sZone, obj);
}


void operator delete[](void* obj) throw()
{
	//fprintf(stderr, "delete[](%p)\n", obj);
	malloc_zone_free(sZone, obj);
}


