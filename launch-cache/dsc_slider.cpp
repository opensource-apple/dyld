/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2010 Apple Inc. All rights reserved.
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
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <Availability.h>

#define NO_ULEB 1
#include "dyld_cache_format.h"
#include "Architectures.hpp"
#include "MachOFileAbstraction.hpp"
#include "CacheFileAbstraction.hpp"

#include "dsc_slider.h"

int update_dyld_shared_cache_load_address(const char* path, void (*logProc)(const char* format, ...) )
{
	int fd = open(path, O_RDONLY, 0);
	if ( fd == -1 ) {
		(*logProc)("open(%s) failed, errno=%d\n", path, errno);
		return -1;
	}
	(void)fcntl(fd, F_NOCACHE, 1); // tell kernel to not cache file content

	uint8_t buffer[4096];
	if ( read(fd, buffer, 4096) != 4096 ) {
		(*logProc)("read(%s) failed, errno=%d\n", path, errno);
		close(fd);
		return -1;
	}
	bool has64BitPointers = false;
	bool isBigEndian = false;
	uint64_t startReadOnlySharedRegion = 0;
	uint64_t endReadOnlySharedRegion = 0;
	uint64_t startReadWriteSharedRegion = 0;
	uint64_t endReadWriteSharedRegion = 0;
	if ( strcmp((char*)buffer, "dyld_v1    i386") == 0 ) {
		has64BitPointers = false;
		isBigEndian = false;
	}
	else if ( strcmp((char*)buffer, "dyld_v1  x86_64") == 0 ) {
		has64BitPointers = true;
		isBigEndian = false;
		startReadOnlySharedRegion  = 0x7FFF80000000LL;
		endReadOnlySharedRegion    = 0x7FFFC0000000LL;
		startReadWriteSharedRegion = 0x7FFF70000000LL;
		endReadWriteSharedRegion   = 0x7FFF80000000LL;
	}
	else if ( strcmp((char*)buffer, "dyld_v1     ppc") == 0 ) {
		has64BitPointers = false;
		isBigEndian = true;
	}
	else if ( strcmp((char*)buffer, "dyld_v1   armv5") == 0 ) {
		has64BitPointers = false;
		isBigEndian = false;
		startReadOnlySharedRegion  = 0x30000000LL;
		endReadOnlySharedRegion    = 0x3E000000LL;
		startReadWriteSharedRegion = 0x3E000000LL;
		endReadWriteSharedRegion   = 0x40000000LL;
	}
	else if ( strcmp((char*)buffer, "dyld_v1   armv6") == 0 ) {
		has64BitPointers = false;
		isBigEndian = false;
		startReadOnlySharedRegion  = 0x30000000LL;
		endReadOnlySharedRegion    = 0x3E000000LL;
		startReadWriteSharedRegion = 0x3E000000LL;
		endReadWriteSharedRegion   = 0x40000000LL;
	}
	else if ( strcmp((char*)buffer, "dyld_v1   armv7") == 0 ) {
		has64BitPointers = false;
		isBigEndian = false;
		startReadOnlySharedRegion  = 0x30000000LL;
		endReadOnlySharedRegion    = 0x3E000000LL;
		startReadWriteSharedRegion = 0x3E000000LL;
		endReadWriteSharedRegion   = 0x40000000LL;
	}
	else {
		(*logProc)("file %s is not a known dyld shared cache file\n", path);
		close(fd);
		return -1;
	}
#if __BIG_ENDIAN__ 
	bool swap = !isBigEndian;
#else
	bool swap = isBigEndian;
#endif
	
	const dyld_cache_header* header = (dyld_cache_header*)buffer;
	uint32_t mappingOffset = swap ? OSSwapInt32(header->mappingOffset) : header->mappingOffset;
	if ( mappingOffset < 0x48 ) {
		(*logProc)("dyld shared cache file %s is old format\n", path);
		close(fd);
		return -1;
	}

	uint32_t mappingCount = swap ? OSSwapInt32(header->mappingCount) : header->mappingCount;
	if ( mappingCount != 3 ) {
		(*logProc)("dyld shared cache file %s has wrong mapping count\n", path);
		close(fd);
		return -1;
	}

	uint64_t slidePointersOffset = swap ? OSSwapInt64(header->slidePointersOffset) : header->slidePointersOffset;
	uint64_t slidePointersSize = swap ? OSSwapInt64(header->slidePointersSize) : header->slidePointersSize;
	if ( (slidePointersOffset == 0) || (slidePointersSize == 0) ) {
		(*logProc)("dyld shared cache file %s is missing slide information\n", path);
		close(fd);
		return -1;
	}
	
	// read slide info
	void* slideInfo = malloc(slidePointersSize);
	if ( slideInfo == NULL ) {
		(*logProc)("malloc(%llu) failed\n", slidePointersSize);
		close(fd);
		return -1;
	}
	int64_t amountRead = pread(fd, slideInfo, slidePointersSize, slidePointersOffset);
	if ( amountRead != (int64_t)slidePointersSize ) {
		(*logProc)("slide info pread(fd, buf, %llu, %llu) failed\n", slidePointersSize, slidePointersOffset);
		close(fd);
		return -1;
	}

	// read all DATA 
	const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)&buffer[mappingOffset];
	uint64_t dataFileOffset = swap ? OSSwapInt64(mappings[1].fileOffset) : mappings[1].fileOffset;
	uint64_t dataSize = swap ? OSSwapInt64(mappings[1].size) : mappings[1].size;
	uint8_t* data = (uint8_t*)malloc(dataSize);
	amountRead = pread(fd, data, dataSize, dataFileOffset);
	if ( amountRead != (int64_t)dataSize ) {
		(*logProc)("data pread(fd, buf, %llu, %llu) failed\n", data, dataSize);
		close(fd);
		return -1;
	}
	
	// close read-only file
	close(fd);
	
	// extract current slide
	uint64_t headerDataUses;
	if ( has64BitPointers ) {
		headerDataUses = *((uint64_t*)data);
		if ( swap ) 
			headerDataUses = OSSwapInt64(headerDataUses);
	}
	else {
		uint32_t temp = *((uint32_t*)data);
		if ( swap ) 
			temp = OSSwapInt32(temp);
		headerDataUses = temp;
	}
	uint64_t textAddress = swap ? OSSwapInt64(mappings[0].address) : mappings[0].address;
	uint32_t currentSlide = headerDataUses - textAddress;
	(*logProc)("currentSlide=0x%08X\n", currentSlide);
	
	// find read-only space
	uint64_t linkeditAddress = swap ? OSSwapInt64(mappings[2].address) : mappings[2].address;
	uint64_t linkeditSize = swap ? OSSwapInt64(mappings[2].size) : mappings[2].size;
	uint64_t linkeditEnd = linkeditAddress + linkeditSize;
	uint64_t roSpace = endReadOnlySharedRegion - linkeditEnd;
	(*logProc)("ro space=0x%08llX\n", roSpace);
	
	// find read-write space
	uint64_t dataAddress = swap ? OSSwapInt64(mappings[1].address) : mappings[1].address;
	uint64_t dataEnd = dataAddress + dataSize;
	uint64_t rwSpace = endReadWriteSharedRegion - dataEnd;
	(*logProc)("rw space=0x%08llX\n", rwSpace);
	
	// choose new random slide
	uint32_t slideSpace = rwSpace;
	if ( roSpace < rwSpace )
		slideSpace = roSpace;
	uint32_t newSlide = (arc4random() % slideSpace) & (-4096);
	(*logProc)("newSlide=0x%08X\n", newSlide);
	int32_t slideAdjustment = newSlide - currentSlide;
	
	// update DATA with slide info
	uint64_t offsetInCacheFile = 0;
	const uint8_t* infoStart = (uint8_t*)slideInfo;
	const uint8_t* infoEnd = infoStart + slidePointersSize;
	for(const uint8_t* p = infoStart; (*p != 0) && (p < infoEnd); ) {
		uint64_t delta = 0;
		uint32_t shift = 0;
		bool more = true;
		do {
			uint8_t byte = *p++;
			delta |= ((byte & 0x7F) << shift);
			shift += 7;
			if ( byte < 0x80 ) {
				offsetInCacheFile += delta;
				// verify in DATA range
				if ( (offsetInCacheFile < dataFileOffset) || (offsetInCacheFile > (dataFileOffset+dataSize)) ) {
					(*logProc)("pointer offset 0x%llX outside DATA range\n", offsetInCacheFile);
					return -1;
				}
				uint32_t offsetInData = offsetInCacheFile - dataFileOffset;
				if ( has64BitPointers ) {
					uint64_t value64 = *((uint64_t*)(&data[offsetInData]));
					if ( swap ) 
						value64 = OSSwapInt64(value64);
					value64 += slideAdjustment;
					if ( swap ) 
						value64 = OSSwapInt64(value64);
					*((uint64_t*)(&data[offsetInData])) = value64;
				}
				else {
					uint64_t value32 = *((uint32_t*)(&data[offsetInData]));
					if ( swap ) 
						value32 = OSSwapInt32(value32);
					value32 += slideAdjustment;
					if ( swap ) 
						value32 = OSSwapInt32(value32);
					*((uint32_t*)(&data[offsetInData])) = value32;
				}
				//(*logProc)("update pointer at offset 0x%08X", offsetInData);
				more = false;
			}
		} while (more);
	}
	free(slideInfo);
	slideInfo = NULL;
	
	// re-open cache file and overwrite DATA range
	fd = open(path, O_RDWR, 0);
	if ( fd == -1 ) {
		(*logProc)("open(%s) failed, errno=%d\n", path, errno);
		return -1;
	}
	(void)fcntl(fd, F_NOCACHE, 1); // tell kernel to not cache file content
	uint64_t amountWrote = pwrite(fd, data, dataSize, dataFileOffset);
	if ( amountWrote != dataSize ) {
		(*logProc)("data pwrite(fd, buf, %llu, %llu) failed\n", data, dataSize);
		close(fd);
		return -1;
	}
	close(fd);
	
	// re-open and re-read to verify write
	fd = open(path, O_RDONLY, 0);
	if ( fd == -1 ) {
		(*logProc)("verify open(%s) failed, errno=%d\n", path, errno);
		return -1;
	}
	(void)fcntl(fd, F_NOCACHE, 1); // tell kernel to not cache file content
	uint8_t* dataVerify = (uint8_t*)malloc(dataSize);
	amountRead = pread(fd, dataVerify, dataSize, dataFileOffset);
	if ( amountRead != (int64_t)dataSize ) {
		(*logProc)("verify data pread(fd, buf, %llu, %llu) failed\n", data, dataSize);
		close(fd);
		return -1;
	}
	close(fd);
	if ( memcmp(data, dataVerify, dataSize) != 0 ) {
		(*logProc)("data update verification failed\n");
		return -1;
	}
	free(data);
	free(dataVerify);

	
	// success
	return 0;
}


#if 0
static void logger(const char* format, ...)
{
	va_list	list;
	va_start(list, format);
	fprintf(stderr, "error: ");
	vfprintf(stderr, format, list);
}


int main(int argc, const char* argv[])
{
	return update_dyld_shared_cache_load_address(argv[1], logger);
}
#endif


