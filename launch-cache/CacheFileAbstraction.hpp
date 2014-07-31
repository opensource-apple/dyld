/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2006 Apple Computer, Inc. All rights reserved.
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
#ifndef __DYLD_CACHE_ABSTRACTION__
#define __DYLD_CACHE_ABSTRACTION__

#include "dyld_cache_format.h"

#include "FileAbstraction.hpp"
#include "Architectures.hpp"

template <typename E>
class dyldCacheHeader {
public:
	const char*		magic() const							INLINE { return fields.magic; }
	void			set_magic(const char* value)			INLINE { memcpy(fields.magic, value, 16); }

	uint32_t		mappingOffset() const					INLINE { return E::get32(fields.mappingOffset); }
	void			set_mappingOffset(uint32_t value)		INLINE { E::set32(fields.mappingOffset, value); }

	uint32_t		mappingCount() const					INLINE { return E::get32(fields.mappingCount); }
	void			set_mappingCount(uint32_t value)		INLINE { E::set32(fields.mappingCount, value); }

	uint32_t		imagesOffset() const					INLINE { return E::get32(fields.imagesOffset); }
	void			set_imagesOffset(uint32_t value)		INLINE { E::set32(fields.imagesOffset, value); }

	uint32_t		imagesCount() const						INLINE { return E::get32(fields.imagesCount); }
	void			set_imagesCount(uint32_t value)			INLINE { E::set32(fields.imagesCount, value); }

	uint64_t		dyldBaseAddress() const					INLINE { return E::get64(fields.dyldBaseAddress); }
	void			set_dyldBaseAddress(uint64_t value)		INLINE { E::set64(fields.dyldBaseAddress, value); }

	uint64_t		codeSignatureOffset() const				INLINE { return E::get64(fields.codeSignatureOffset); }
	void			set_codeSignatureOffset(uint64_t value)	INLINE { E::set64(fields.codeSignatureOffset, value); }

	uint64_t		codeSignatureSize() const				INLINE { return E::get64(fields.codeSignatureSize); }
	void			set_codeSignatureSize(uint64_t value)	INLINE { E::set64(fields.codeSignatureSize, value); }

	uint64_t		slideInfoOffset() const					INLINE { return E::get64(fields.slideInfoOffset); }
	void			set_slideInfoOffset(uint64_t value)		INLINE { E::set64(fields.slideInfoOffset, value); }

	uint64_t		slideInfoSize() const					INLINE { return E::get64(fields.slideInfoSize); }
	void			set_slideInfoSize(uint64_t value)		INLINE { E::set64(fields.slideInfoSize, value); }

private:
	dyld_cache_header			fields;
};


template <typename E>
class dyldCacheFileMapping {
public:		
	uint64_t		address() const								INLINE { return E::get64(fields.address); }
	void			set_address(uint64_t value)					INLINE { E::set64(fields.address, value); }

	uint64_t		size() const								INLINE { return E::get64(fields.size); }
	void			set_size(uint64_t value)					INLINE { E::set64(fields.size, value); }

	uint64_t		file_offset() const							INLINE { return E::get64(fields.fileOffset); }
	void			set_file_offset(uint64_t value)				INLINE { E::set64(fields.fileOffset, value); }

	uint32_t		max_prot() const							INLINE { return E::get32(fields.maxProt); }
	void			set_max_prot(uint32_t value)				INLINE { E::set32((uint32_t&)fields.maxProt, value); }

	uint32_t		init_prot() const							INLINE { return E::get32(fields.initProt); }
	void			set_init_prot(uint32_t value)				INLINE { E::set32((uint32_t&)fields.initProt, value); }

private:
	dyld_cache_mapping_info			fields;
};


template <typename E>
class dyldCacheImageInfo {
public:		
	uint64_t		address() const								INLINE { return E::get64(fields.address); }
	void			set_address(uint64_t value)					INLINE { E::set64(fields.address, value); }

	uint64_t		modTime() const								INLINE { return E::get64(fields.modTime); }
	void			set_modTime(uint64_t value)					INLINE { E::set64(fields.modTime, value); }

	uint64_t		inode() const								INLINE { return E::get64(fields.inode); }
	void			set_inode(uint64_t value)					INLINE { E::set64(fields.inode, value); }

	uint32_t		pathFileOffset() const						INLINE { return E::get32(fields.pathFileOffset); }
	void			set_pathFileOffset(uint32_t value)			INLINE { E::set32(fields.pathFileOffset, value); fields.pad=0; }

private:
	dyld_cache_image_info			fields;
};

template <typename E>
class dyldCacheSlideInfo {
public:		
	uint32_t		version() const								INLINE { return E::get32(fields.version); }
	void			set_version(uint32_t value)					INLINE { E::set32(fields.version, value); }

	uint32_t		toc_offset() const							INLINE { return E::get32(fields.toc_offset); }
	void			set_toc_offset(uint32_t value)				INLINE { E::set32(fields.toc_offset, value); }

	uint32_t		toc_count() const							INLINE { return E::get32(fields.toc_count); }
	void			set_toc_count(uint32_t value)				INLINE { E::set32(fields.toc_count, value); }

	uint32_t		entries_offset() const						INLINE { return E::get32(fields.entries_offset); }
	void			set_entries_offset(uint32_t value)			INLINE { E::set32(fields.entries_offset, value); }
	
	uint32_t		entries_count() const						INLINE { return E::get32(fields.entries_count); }
	void			set_entries_count(uint32_t value)			INLINE { E::set32(fields.entries_count, value); }

	uint32_t		entries_size() const						INLINE { return E::get32(fields.entries_size); }
	void			set_entries_size(uint32_t value)			INLINE { E::set32(fields.entries_size, value); }

	uint16_t		toc(unsigned index) const					INLINE { return E::get16(((uint16_t*)(((uint8_t*)this)+E::get16(fields.toc_offset)))[index]); }
	void			set_toc(unsigned index, uint16_t value) 	INLINE { return E::set16(((uint16_t*)(((uint8_t*)this)+E::get16(fields.toc_offset)))[index], value); }

private:
	dyld_cache_slide_info			fields;
};


struct dyldCacheSlideInfoEntry {
	uint8_t  bits[4096/(8*4)]; // 128-byte bitmap
};



#endif // __DYLD_CACHE_ABSTRACTION__


