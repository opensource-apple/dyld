/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2009 Apple Inc. All rights reserved.
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


#ifndef __DYLDLIBSYSTEMHELPERS__
#define __DYLDLIBSYSTEMHELPERS__

#include <stdint.h>

#if __cplusplus
namespace dyld {
#endif
	//
	// This file contains the synchronization utilities used by dyld to be thread safe.
	// This struct is implemented in in libSystem (where pthreads is available)
	// and passed to dyld to use.
	//
	struct LibSystemHelpers
	{
		uintptr_t	version;
		void		(*acquireGlobalDyldLock)();
		void		(*releaseGlobalDyldLock)();
		char*		(*getThreadBufferFor_dlerror)(uint32_t sizeRequired);
		// addded in version 2
		void*		(*malloc)(size_t);
		void		(*free)(void*);
		int			(*cxa_atexit)(void (*)(void*), void*, void*);
		// addded in version 3
		void		(*dyld_shared_cache_missing)();
		void		(*dyld_shared_cache_out_of_date)();
		// addded in version 4
		void		(*acquireDyldInitializerLock)();
		void		(*releaseDyldInitializerLock)();
		// added in version 5
		int			(*pthread_key_create)(pthread_key_t*, void (*destructor)(void*));
		int			(*pthread_setspecific)(pthread_key_t, const void*);
		// added in version 6
		size_t		(*malloc_size)(const void *ptr);
		// added in version 7
		void*		(*pthread_getspecific)(pthread_key_t);
		// added in version 8
		void		(*cxa_finalize)(const void*);
		// added in version 9
		void*		startGlueToCallExit;
	};
#if __cplusplus
};
#endif




#endif // __DYLDLIBSYSTEMHELPERS__

