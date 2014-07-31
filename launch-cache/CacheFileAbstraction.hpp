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

	//uint32_t		architecture() const					INLINE { return E::get32(fields.architecture); }
	//void			set_architecture(uint32_t value)		INLINE { E::set32(fields.architecture, value); }

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

	//uint32_t		dependenciesOffset() const				INLINE { return E::get32(fields.dependenciesOffset); }
	//void			set_dependenciesOffset(uint32_t value)	INLINE { E::set32(fields.dependenciesOffset, value); }

	//uint32_t		dependenciesCount() const				INLINE { return E::get32(fields.dependenciesCount); }
	//void			set_dependenciesCount(uint32_t value)	INLINE { E::set32(fields.dependenciesCount, value); }

private:
	dyld_cache_header			fields;
};


template <typename E>
class dyldCacheFileMapping {
public:		
	uint64_t		address() const								INLINE { return E::get64(fields.sfm_address); }
	void			set_address(uint64_t value)					INLINE { E::set64(fields.sfm_address, value); }

	uint64_t		size() const								INLINE { return E::get64(fields.sfm_size); }
	void			set_size(uint64_t value)					INLINE { E::set64(fields.sfm_size, value); }

	uint64_t		file_offset() const							INLINE { return E::get64(fields.sfm_file_offset); }
	void			set_file_offset(uint64_t value)				INLINE { E::set64(fields.sfm_file_offset, value); }

	uint32_t		max_prot() const							INLINE { return E::get32(fields.sfm_max_prot); }
	void			set_max_prot(uint32_t value)				INLINE { E::set32((uint32_t&)fields.sfm_max_prot, value); }

	uint32_t		init_prot() const							INLINE { return E::get32(fields.sfm_init_prot); }
	void			set_init_prot(uint32_t value)				INLINE { E::set32((uint32_t&)fields.sfm_init_prot, value); }

private:
	shared_file_mapping_np			fields;
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

	//uint32_t		dependenciesStartOffset() const				INLINE { return E::get32(fields.dependenciesStartOffset); }
	//void			set_dependenciesStartOffset(uint32_t value)	INLINE { E::set32(fields.dependenciesStartOffset, value); }

private:
	dyld_cache_image_info			fields;
};



#endif // __DYLD_CACHE_ABSTRACTION__


