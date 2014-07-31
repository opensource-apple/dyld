/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2006-2009 Apple Inc. All rights reserved.
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
#ifndef __DYLD_CACHE_FORMAT__
#define __DYLD_CACHE_FORMAT__

#include <sys/types.h>
#include <stdint.h>
#include <mach/shared_region.h>


struct dyld_cache_header
{
	char		magic[16];				// e.g. "dyld_v0    i386"
	uint32_t	mappingOffset;			// file offset to first dyld_cache_mapping_info
	uint32_t	mappingCount;			// number of dyld_cache_mapping_info entries
	uint32_t	imagesOffset;			// file offset to first dyld_cache_image_info
	uint32_t	imagesCount;			// number of dyld_cache_image_info entries
	uint64_t	dyldBaseAddress;		// base address of dyld when cache was built
	uint64_t	codeSignatureOffset;	// file offset of code signature blob
	uint64_t	codeSignatureSize;		// size of code signature blob (zero means to end of file)
	uint64_t	slideInfoOffset;		// file offset of kernel slid info
	uint64_t	slideInfoSize;			// size of kernel slid info
	uint64_t	localSymbolsOffset;		// file offset of where local symbols are stored
	uint64_t	localSymbolsSize;		// size of local symbols information
	uint8_t		uuid[16];				// unique value for each shared cache file
};

struct dyld_cache_mapping_info {
	uint64_t	address;
	uint64_t	size;
	uint64_t	fileOffset;
	uint32_t	maxProt;
	uint32_t	initProt;
};

struct dyld_cache_image_info
{
	uint64_t	address;
	uint64_t	modTime;
	uint64_t	inode;
	uint32_t	pathFileOffset;
	uint32_t	pad;
};

struct dyld_cache_slide_info
{
	uint32_t	version;		// currently 1
	uint32_t	toc_offset;
	uint32_t	toc_count;
	uint32_t	entries_offset;
	uint32_t	entries_count;
	uint32_t	entries_size;  // currently 128 
	// uint16_t toc[toc_count];
	// entrybitmap entries[entries_count];
};


struct dyld_cache_local_symbols_info
{
	uint32_t	nlistOffset;		// offset into this chunk of nlist entries
	uint32_t	nlistCount;			// count of nlist entries
	uint32_t	stringsOffset;		// offset into this chunk of string pool
	uint32_t	stringsSize;		// byte count of string pool
	uint32_t	entriesOffset;		// offset into this chunk of array of dyld_cache_local_symbols_entry 
	uint32_t	entriesCount;		// number of elements in dyld_cache_local_symbols_entry array
};

struct dyld_cache_local_symbols_entry
{
	uint32_t	dylibOffset;		// offset in cache file of start of dylib
	uint32_t	nlistStartIndex;	// start index of locals for this dylib
	uint32_t	nlistCount;			// number of local symbols for this dylib
};



#define MACOSX_DYLD_SHARED_CACHE_DIR	"/var/db/dyld/"
#define IPHONE_DYLD_SHARED_CACHE_DIR	"/System/Library/Caches/com.apple.dyld/"
#define DYLD_SHARED_CACHE_BASE_NAME		"dyld_shared_cache_"



#endif // __DYLD_CACHE_FORMAT__


