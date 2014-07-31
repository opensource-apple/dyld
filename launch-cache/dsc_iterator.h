/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2009-2010 Apple Inc. All rights reserved.
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

#ifdef __cplusplus
extern "C" {
#endif 


typedef void (^dyld_shared_cache_iterator_t)(const char* dylibName, const char* segName, uint64_t offset, uint64_t size, uint64_t mappedddress);
typedef void (^dyld_shared_cache_iterator_slide_t)(const char* dylibName, const char* segName, uint64_t offset, uint64_t size, uint64_t mappedddress, uint64_t slide);
typedef void (*dyld_shared_cache_iterator_nb_t)(const char* dylibName, const char* segName, uint64_t offset, uint64_t sizem, uint64_t mappedddress, void* userData);
typedef void (*dyld_shared_cache_iterator_slide_nb_t)(const char* dylibName, const char* segName, uint64_t offset, uint64_t sizem, uint64_t mappedddress, uint64_t slide, void* userData);

// Given a pointer to an in-memory copy of a dyld shared cache file,
// this routine will call the callback block once for each segment
// in each dylib in the shared cache file.  
// Returns -1 if there was an error, otherwise 0.
extern int dyld_shared_cache_iterate_segments(const void* shared_cache_file, dyld_shared_cache_iterator_t callback);
extern int dyld_shared_cache_iterate_segments_with_slide(const void* shared_cache_file, dyld_shared_cache_iterator_slide_t callback);
extern int dyld_shared_cache_iterate_segments_nb(const void* shared_cache_file, dyld_shared_cache_iterator_nb_t callback, void* userData);
extern int dyld_shared_cache_iterate_segments_with_slide_nb(const void* shared_cache_file, dyld_shared_cache_iterator_slide_nb_t callback, void* userData);


#ifdef __cplusplus
}
#endif 
