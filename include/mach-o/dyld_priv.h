/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2003-2006 Apple Computer, Inc. All rights reserved.
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
#ifndef _MACH_O_DYLD_PRIV_H_
#define _MACH_O_DYLD_PRIV_H_


#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>

#if __cplusplus
extern "C" {
#endif /* __cplusplus */


/*
 * Given an imageOffset into an ObjectFileImage, returns 
 * the segment/section name and offset into that section of
 * that imageOffset.  Returns FALSE if the imageOffset is not 
 * in any section.  You can used the resulting sectionOffset to
 * index into the data returned by NSGetSectionDataInObjectFileImage.
 * 
 * First appeared in Mac OS X 10.3 
 *
 * SPI: currently only used by ZeroLink to detect +load methods
 */
bool 
NSFindSectionAndOffsetInObjectFileImage(
    NSObjectFileImage objectFileImage, 
    unsigned long imageOffset,
    const char** segmentName, 	/* can be NULL */
    const char** sectionName, 	/* can be NULL */
    unsigned long* sectionOffset);	/* can be NULL */


//
// Possible state changes for which you can register to be notified
//
enum dyld_image_states
{
	dyld_image_state_mapped					= 10,		// No batch notification for this
	dyld_image_state_dependents_mapped		= 20,		// Only batch notification for this
	dyld_image_state_rebased				= 30, 
	dyld_image_state_bound					= 40,
	dyld_image_state_dependents_initialized	= 45,		// Only single notification for this
	dyld_image_state_initialized			= 50,
	dyld_image_state_terminated				= 60		// FIX ME - only called if image has termination routine
};

// 
// Callback that provides a bottom-up array of images
// For dyld_image_state_[dependents_]mapped state only, returning non-NULL will cause dyld to abort loading all those images
// and append the returned string to its load failure error message. dyld does not free the string, so
// it should be a literal string or a static buffer
//
typedef const char* (*dyld_image_state_change_handler)(enum dyld_image_states state, uint32_t infoCount, const struct dyld_image_info info[]);

//
// Register a handler to be called when any image changes to the requested state.
// If 'batch' is true, the callback is called with an array of all images that are in the requested state sorted by dependency.
// If 'batch' is false, the callback is called with one image at a time as each image transitions to the the requested state.
// During the call to this function, the handler may be called back with existing images and the handler should
// not return a string, since there is no load to abort.  In batch mode, existing images at or past the request
// state supplied in the callback.  In non-batch mode, the callback is called for each image exactly in the
// requested state.    
//
extern void
dyld_register_image_state_change_handler(enum dyld_image_states state, bool batch, dyld_image_state_change_handler handler);


//
//
//
extern void
_dyld_library_locator(const char* (*handler)(const char*));

extern void* dlord(void* handle, uint32_t ordinal); /* Mac OS X 10.5 and later */


#define	RTLD_MAIN_ONLY		((void *) -5)	/* Search main executable only (Mac OS X 10.5 and later) */




#if __cplusplus
}
#endif /* __cplusplus */

#endif /* _MACH_O_DYLD_PRIV_H_ */
