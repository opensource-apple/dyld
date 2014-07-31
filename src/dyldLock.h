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


#ifndef __DYLDLOCK__
#define __DYLDLOCK__

//
// This file contains the syncronization utilities used by dyld to be thread safe.
// Access to all dyld data structures is protected by a reader-writer lock.
// This lock allows multiple "reader" threads to access dyld data structures at
// the same time.  If there is a "writer" thread, it is the only thread allowed
// access to dyld data structures.  
//
// The implementation of all dyld API's must acquire the global lock around accesses
// to dyld's data structures.  This is done using the macros DYLD_*_LOCK_THIS_BLOCK.
// Example:
//
//  void dyld_api_modifying_dyld() {
//		DYLD_WRITER_LOCK_THIS_BLOCK;
//		// free to do stuff here 
//		// that modifies dyld data structures
//	}
//
//  void dyld_api_examinging_dyld() {
//		DYLD_READER_LOCK_THIS_BLOCK
//		// can only do stuff here 
//		// that examines but does not modify dyld data structures
//	}
//
//  void dyld_api_state_free() {
//		DYLD_NO_LOCK_THIS_BLOCK
//		// can only do stuff here 
//		// that does not require locking
//	}
//


#define DYLD_READER_LOCK_THIS_BLOCK		LockReaderHelper _dyld_lock;
#define DYLD_WRITER_LOCK_THIS_BLOCK		LockWriterHelper _dyld_lock;
#define DYLD_NO_LOCK_THIS_BLOCK

// used by dyld wrapper functions in libSystem
class LockReaderHelper
{
public:
	LockReaderHelper() __attribute__((visibility("hidden")));
	~LockReaderHelper() __attribute__((visibility("hidden")));
};

// used by dyld wrapper functions in libSystem
class LockWriterHelper
{
public:
	LockWriterHelper() __attribute__((visibility("hidden")));
	~LockWriterHelper() __attribute__((visibility("hidden")));
};

// used by lazy binding
extern void lockForLazyBinding() __attribute__((visibility("hidden")));
extern void unlockForLazyBinding() __attribute__((visibility("hidden")));



#endif // __DYLDLOCK__

