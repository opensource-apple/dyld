/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
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

#include <stdlib.h>

#include "dsc_iterator.h"
#include "dyld_cache_format.h"
#define NO_ULEB
#include "Architectures.hpp"
#include "MachOFileAbstraction.hpp"
#include "CacheFileAbstraction.hpp"


namespace dyld {

	// convert an address in the shared region where the cache would normally be mapped, into an address where the cache is currently mapped
	template <typename E>
	const uint8_t* mappedAddress(const uint8_t* cache, uint64_t addr)
	{
		const dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)cache;
		const dyldCacheFileMapping<E>* mappings = (dyldCacheFileMapping<E>*)&cache[header->mappingOffset()];
		for (uint32_t i=0; i < header->mappingCount(); ++i) {
			if ( (mappings[i].address() <= addr) &&  (addr < (mappings[i].address() + mappings[i].size())) ) {
				return &cache[mappings[i].file_offset() + addr - mappings[i].address()];
			}
		}
		return NULL;
	}

	// call the callback block on each segment in this image						  	  
	template <typename A>
	void walkSegments(const uint8_t* cache, const char* dylibPath, const uint8_t* machHeader, dyld_shared_cache_iterator_t callback) 
	{
		typedef typename A::P		P;	
		typedef typename A::P::E	E;	
		const macho_header<P>* mh = (const macho_header<P>*)machHeader;
		const macho_load_command<P>* const cmds = (macho_load_command<P>*)(machHeader + sizeof(macho_header<P>));
		const uint32_t cmd_count = mh->ncmds();
		const macho_load_command<P>* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd() == macho_segment_command<P>::CMD ) {
				macho_segment_command<P>* segCmd = (macho_segment_command<P>*)cmd;
				const uint8_t* segStartInCache = mappedAddress<E>(cache, segCmd->vmaddr());
				uint64_t fileOffset = segStartInCache - cache;
				callback(dylibPath, segCmd->segname(), fileOffset, segCmd->vmsize());
			}
			cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
		}
	}
						
							  
	// call walkSegments on each image in the cache							  	  
	template <typename A>
	int walkImages(const uint8_t* cache, dyld_shared_cache_iterator_t callback) 
	{
		typedef typename A::P::E   E;	
		const dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)cache;
		const dyldCacheImageInfo<E>* dylibs = (dyldCacheImageInfo<E>*)&cache[header->imagesOffset()];
		for (uint32_t i=0; i < header->imagesCount(); ++i) {
			const char* dylibPath  = (char*)cache + dylibs[i].pathFileOffset();
			const uint8_t* machHeader = mappedAddress<E>(cache, dylibs[i].address());
			walkSegments<A>(cache, dylibPath, machHeader, callback);
		}
		return 0;
	}

}


// Given a pointer to an in-memory copy of a dyld shared cache file,
// this routine will call the callback block once for each segment
// in each dylib in the shared cache file.  
// Returns -1 if there was an error, otherwise 0.
int dyld_shared_cache_iterate_segments(const void* shared_cache_file, dyld_shared_cache_iterator_t callback)
{
	const uint8_t* cache = (uint8_t*)shared_cache_file;
		 if ( strcmp((char*)cache, "dyld_v1    i386") == 0 ) 
			return dyld::walkImages<x86>(cache, callback);
	else if ( strcmp((char*)cache, "dyld_v1  x86_64") == 0 ) 
			return dyld::walkImages<x86_64>(cache, callback);
	else if ( strcmp((char*)cache, "dyld_v1     ppc") == 0 ) 
			return dyld::walkImages<ppc>(cache, callback);
	else
		return -1;
}

