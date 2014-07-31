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

#include <stdlib.h>
#include <stdio.h>
#include <Availability.h>


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
	void walkSegments(const uint8_t* cache, const char* dylibPath, const uint8_t* machHeader, uint64_t slide, dyld_shared_cache_iterator_slide_t callback) 
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
				uint64_t fileOffset = segCmd->fileoff();
				// work around until <rdar://problem/7022345> is fixed
				if ( fileOffset == 0 ) {
					fileOffset = (machHeader - cache);
				}
				uint64_t sizem = segCmd->vmsize();
				if ( strcmp(segCmd->segname(), "__LINKEDIT") == 0 ) {
					// clip LINKEDIT size if bigger than cache file
					const dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)cache;
					const dyldCacheFileMapping<E>* mappings = (dyldCacheFileMapping<E>*)&cache[header->mappingOffset()];
					if ( mappings[2].file_offset() <= fileOffset ) {
						if ( sizem > mappings[2].size() )
							sizem = mappings[2].file_offset() + mappings[2].size() - fileOffset;
					}
				}
				callback(dylibPath, segCmd->segname(), fileOffset, sizem, segCmd->vmaddr()+slide, slide);
			}
			cmd = (const macho_load_command<P>*)(((uint8_t*)cmd)+cmd->cmdsize());
		}
	}
			
							  
	// call walkSegments on each image in the cache							  	  
	template <typename A>
	int walkImages(const uint8_t* cache, dyld_shared_cache_iterator_slide_t callback) 
	{
		typedef typename A::P::E			E;	
		typedef typename A::P				P;	
		typedef typename A::P::uint_t		pint_t;
		const dyldCacheHeader<E>* header = (dyldCacheHeader<E>*)cache;
		uint64_t slide = 0;
		if ( header->mappingOffset() >= 0x48 ) {
			const dyldCacheFileMapping<E>* mappings = (dyldCacheFileMapping<E>*)&cache[header->mappingOffset()];
			uint64_t storedPointerToHeader = P::getP(*((pint_t*)&cache[mappings[1].file_offset()]));
			slide = storedPointerToHeader - mappings[0].address();
		}	
		const dyldCacheImageInfo<E>* dylibs = (dyldCacheImageInfo<E>*)&cache[header->imagesOffset()];
		for (uint32_t i=0; i < header->imagesCount(); ++i) {
			const char* dylibPath  = (char*)cache + dylibs[i].pathFileOffset();
			const uint8_t* machHeader = mappedAddress<E>(cache, dylibs[i].address());
			walkSegments<A>(cache, dylibPath, machHeader, slide, callback);
		}
		return 0;
	}

}


// Given a pointer to an in-memory copy of a dyld shared cache file,
// this routine will call the callback block once for each segment
// in each dylib in the shared cache file.  
// Returns -1 if there was an error, otherwise 0.
int dyld_shared_cache_iterate_segments_with_slide(const void* shared_cache_file, dyld_shared_cache_iterator_slide_t callback)
{
	const uint8_t* cache = (uint8_t*)shared_cache_file;
		 if ( strcmp((char*)cache, "dyld_v1    i386") == 0 ) 
			return dyld::walkImages<x86>(cache, callback);
	else if ( strcmp((char*)cache, "dyld_v1  x86_64") == 0 ) 
			return dyld::walkImages<x86_64>(cache, callback);
	else if ( strcmp((char*)cache, "dyld_v1   armv5") == 0 ) 
			return dyld::walkImages<arm>(cache, callback);
	else if ( strcmp((char*)cache, "dyld_v1   armv6") == 0 ) 
			return dyld::walkImages<arm>(cache, callback);
	else if ( strcmp((char*)cache, "dyld_v1   armv7") == 0 ) 
			return dyld::walkImages<arm>(cache, callback);
	else if ( strncmp((char*)cache, "dyld_v1  armv7", 14) == 0 ) 
			return dyld::walkImages<arm>(cache, callback);
	else
		return -1;
}

// implement non-block version by calling block version
int dyld_shared_cache_iterate_segments_with_slide_nb(const void* shared_cache_file, dyld_shared_cache_iterator_slide_nb_t func, void* userData)
{
	return dyld_shared_cache_iterate_segments_with_slide(shared_cache_file, ^(const char* dylibName, const char* segName, 
													uint64_t offset, uint64_t size, uint64_t mappedddress, uint64_t slide) {
														(*func)(dylibName, segName, offset, size, mappedddress, slide, userData);
	});
}


// implement non-slide version by wrapping slide version in block
int dyld_shared_cache_iterate_segments(const void* shared_cache_file, dyld_shared_cache_iterator_t callback)
{
	dyld_shared_cache_iterator_slide_t wrapper_cb = ^(const char* dylibName, const char* segName, uint64_t offset, 
														uint64_t size, uint64_t mappedddress, uint64_t slide) {
														callback(dylibName, segName, offset, size, mappedddress);
													};
	return dyld_shared_cache_iterate_segments_with_slide(shared_cache_file, wrapper_cb);
}

// implement non-slide,non-block version by wrapping slide version in block
int dyld_shared_cache_iterate_segments_nb(const void* shared_cache_file, dyld_shared_cache_iterator_nb_t func, void* userData)
{
	return dyld_shared_cache_iterate_segments_with_slide(shared_cache_file, ^(const char* dylibName, const char* segName, 
																			  uint64_t offset, uint64_t size, uint64_t mappedddress, uint64_t slide) {
		(*func)(dylibName, segName, offset, size, mappedddress, userData);
	});
}

