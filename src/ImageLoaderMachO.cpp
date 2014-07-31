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

#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h> 
#include <sys/mman.h>
#include <mach/shared_memory_server.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <mach-o/loader.h> 
#include <mach-o/reloc.h> 
#include <mach-o/nlist.h> 
#include <sys/sysctl.h>
#if __ppc__ || __ppc64__
	#include <mach-o/ppc/reloc.h>
#endif

#include "ImageLoaderMachO.h"
#include "mach-o/dyld_gdb.h"

// no header for this yet, rdar://problem/3850825
extern "C" void sys_icache_invalidate(void *, size_t);

// optimize strcmp for ppc
#if __ppc__
	#include <ppc_intrinsics.h>
#else
	#define astrcmp(a,b) strcmp(a,b)
#endif

// relocation_info.r_length field has value 3 for 64-bit executables and value 2 for 32-bit executables
#if __LP64__
	#define RELOC_SIZE 3
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define LC_ROUTINES_COMMAND		LC_ROUTINES_64
	struct macho_header				: public mach_header_64  {};
	struct macho_segment_command	: public segment_command_64  {};
	struct macho_section			: public section_64  {};	
	struct macho_nlist				: public nlist_64  {};	
	struct macho_routines_command	: public routines_command_64  {};	
#else
	#define RELOC_SIZE 2
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define LC_ROUTINES_COMMAND		LC_ROUTINES
	struct macho_header				: public mach_header  {};
	struct macho_segment_command	: public segment_command {};
	struct macho_section			: public section  {};	
	struct macho_nlist				: public nlist  {};	
	struct macho_routines_command	: public routines_command  {};	
#endif


uint32_t ImageLoaderMachO::fgHintedBinaryTreeSearchs = 0;
uint32_t ImageLoaderMachO::fgUnhintedBinaryTreeSearchs = 0;


//#define LINKEDIT_USAGE_DEBUG 1

#if LINKEDIT_USAGE_DEBUG
	#include <set>
	static std::set<uintptr_t> sLinkEditPageBuckets;

	namespace dyld {
		extern ImageLoader*	findImageContainingAddress(const void* addr);
	};

	static void noteAccessedLinkEditAddress(const void* addr)
	{
		uintptr_t page = ((uintptr_t)addr) & (-4096);
		sLinkEditPageBuckets.insert(page);
		fprintf(stderr, "dyld: accessing page 0x%08lX in __LINKEDIT of %s\n", page, dyld::findImageContainingAddress(addr)->getPath());
	}
#endif

// only way to share initialization in C++
void ImageLoaderMachO::init()
{
	fMachOData		= NULL;
	fLinkEditBase	= NULL;
	fSymbolTable	= NULL;
	fStrings		= NULL;
	fDynamicInfo	= NULL;
	fSlide			= 0;
	fIsSplitSeg		= false;
	fHasSubLibraries= false;
	fHasSubUmbrella = false;
	fDashInit		= NULL;
	fModInitSection	= NULL;
	fModTermSection	= NULL;
	fDATAdyld		= NULL;
	fImageNotifySection	= NULL;
	fTwoLevelHints	= NULL;
	fDylibID		= NULL;
	fReExportThruFramework	= NULL;
	fTextSegmentWithFixups = NULL;
}

// create image by copying an in-memory mach-o file
ImageLoaderMachO::ImageLoaderMachO(const char* moduleName, const struct mach_header* mh, uint64_t len, const LinkContext& context)
 : ImageLoader(moduleName)
{
	// clean slate
	this->init();

	// temporary use this buffer until TEXT is mapped in
	fMachOData = (const uint8_t*)mh;

	// create segments
	this->instantiateSegments((const uint8_t*)mh);
	
	// map segments
	if ( mh->filetype != MH_EXECUTE )
		ImageLoader::mapSegments((const void*)mh, len, context);
	
	// get pointers to interesting things 
	this->parseLoadCmds();
}


// create image by mapping in a mach-o file
ImageLoaderMachO::ImageLoaderMachO(const char* path, int fd, const uint8_t firstPage[4096], uint64_t offsetInFat, 
									uint64_t lenInFat, const struct stat& info, const LinkContext& context)
 : ImageLoader(path, offsetInFat, info)
{	
	// clean slate
	this->init();

	// read load commands
	const unsigned int dataSize = sizeof(macho_header) + ((macho_header*)firstPage)->sizeofcmds;
	uint8_t buffer[dataSize];
	const uint8_t* fileData = firstPage;
	if ( dataSize > 4096 ) {
		// only read more if cmds take up more space than first page
		fileData = buffer;
		memcpy(buffer, firstPage, 4096);
		pread(fd, &buffer[4096], dataSize-4096, offsetInFat+4096);
	}
	
	// temporary use this buffer until TEXT is mapped in
	fMachOData = fileData;
	
	// the meaning of many fields changes in split seg mach-o files
	fIsSplitSeg = ((((macho_header*)fileData)->flags & MH_SPLIT_SEGS) != 0) && (((macho_header*)fileData)->filetype == MH_DYLIB);	
	
	// create segments
	this->instantiateSegments(fileData);
	
	// map segments, except for main executable which is already mapped in by kernel
	if ( ((macho_header*)fileData)->filetype != MH_EXECUTE )
		this->mapSegments(fd, offsetInFat, lenInFat, info.st_size, context);
			
	// get pointers to interesting things 
	this->parseLoadCmds();
}




void ImageLoaderMachO::instantiateSegments(const uint8_t* fileData)
{
	const uint32_t cmd_count = ((macho_header*)fileData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fileData[sizeof(macho_header)];

	// construct Segment object for each LC_SEGMENT cmd and add to list
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			fSegments.push_back(new SegmentMachO((struct macho_segment_command*)cmd, this, fileData));
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}



bool ImageLoaderMachO::segmentsMustSlideTogether() const 
{
	return true;
}

bool ImageLoaderMachO::segmentsCanSlide() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->filetype == MH_DYLIB) || (mh->filetype == MH_BUNDLE) );
}

bool ImageLoaderMachO::isBundle() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( mh->filetype == MH_BUNDLE );
}

bool ImageLoaderMachO::isDylib() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( mh->filetype == MH_DYLIB );
}

bool ImageLoaderMachO::forceFlat() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_FORCE_FLAT) != 0 );
}

bool ImageLoaderMachO::usesTwoLevelNameSpace() const
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_TWOLEVEL) != 0 );
}

bool ImageLoaderMachO::isPrebindable() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_PREBOUND) != 0 );
}

bool ImageLoaderMachO::hasCoalescedExports() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_WEAK_DEFINES) != 0 );
}

bool ImageLoaderMachO::needsCoalescing() const 
{
	const macho_header* mh = (macho_header*)fMachOData;
	return ( (mh->flags & MH_BINDS_TO_WEAK) != 0 );
}

#if !__LP64__   // split segs not supported for 64-bits

#if 1 // hack until kernel headers and glue are in system
struct _shared_region_mapping_np {
    mach_vm_address_t   address;
    mach_vm_size_t      size;
    mach_vm_offset_t    file_offset;
	vm_prot_t		max_prot;   /* read/write/execute/COW/ZF */
	vm_prot_t		init_prot;  /* read/write/execute/COW/ZF */
};
struct _shared_region_range_np {
    mach_vm_address_t   address;
    mach_vm_size_t      size;
};
		
// Called by dyld.  
// Requests the kernel to map a number of regions from the fd into the
// shared sections address range (0x90000000-0xAFFFFFFF).
// If shared_region_make_private_np() has not been called by this process, 
// the file mapped in is seen in the address space of all processes that
// participate in using the shared region. 
// If shared_region_make_private_np() _has_ been called by this process, 
// the file mapped in is only seen by this process.
// If the slide parameter is not NULL and then regions cannot be mapped
// as requested, the kernel will try to map the file in at a different
// address in the shared region and return the distance slid. 
// If the mapping requesting cannot be fulfilled, returns non-zero.
static int 
_shared_region_map_file_np(
	int fd,							// file descriptor to map into shared region
	unsigned int regionCount,		// number of entres in array of regions
	const _shared_region_mapping_np regions[],	// the array of regions to map
	uint64_t* slide)					// the amount all regions were slid,  NULL means don't attempt to slide
{
	//fprintf(stderr, "%s(%i, %u, %8p, %8p)\n", __func__, fd, regionCount, regions, slide);
	//for ( unsigned int i=0; i < regionCount; ++i) {
	//	fprintf(stderr, "\taddress=0x%08llX, size=0x%08llX\n", regions[i].address, regions[i].size);
	//}
	int r = syscall(299, fd, regionCount, regions, slide);
// 	if(0 != r)
// 		fprintf(stderr, "%s(%i, %u, %8p, %8p) errno=%i (%s)\n", __func__, fd, regionCount, regions, slide, errno, strerror(errno));
    return r;
}
// Called by dyld if shared_region_map_file() fails.
// Requests the kernel to take this process out of using the shared region.
// The specified ranges are created as private copies from the shared region for this process.
static int 
_shared_region_make_private_np(
	unsigned int rangeCount,				// number of entres in array of msrp_range
	const _shared_region_range_np ranges[])	// the array of shared regions to make private
{
 	//fprintf(stderr, "%s(%u, %8p)\n", __func__, rangeCount, ranges);
	int r = syscall(300, rangeCount, ranges);
// 	if(0 != r)
// 		fprintf(stderr, "%s(%u, %8p) errno=%i (%s)\n", __func__, rangeCount, ranges, errno, strerror(errno));
    return r;
}
#define KERN_SHREG_PRIVATIZABLE	54
#endif // hack until kernel headers and glue are in system

static uintptr_t sNextAltLoadAddress 
#if __ppc_
	= 0xC0000000;
#else
	= 0;
#endif

static int 
_shared_region_map_file_with_mmap(
	int fd,							// file descriptor to map into shared region
	unsigned int regionCount,		// number of entres in array of regions
	const _shared_region_mapping_np regions[])	// the array of regions to map
{
	// map in each region
	for(unsigned int i=0; i < regionCount; ++i) {
		void* mmapAddress = (void*)(uintptr_t)(regions[i].address);
		size_t size = regions[i].size;
		if ( (regions[i].init_prot & VM_PROT_ZF) != 0 ) {
			// do nothing already vm_allocate() which zero fills
		}
		else {
			int protection = 0;
			if ( regions[i].init_prot & VM_PROT_EXECUTE )
				protection   |= PROT_EXEC;
			if ( regions[i].init_prot & VM_PROT_READ )
				protection   |= PROT_READ;
			if ( regions[i].init_prot & VM_PROT_WRITE )
				protection   |= PROT_WRITE;
			off_t offset = regions[i].file_offset;
			//fprintf(stderr, "mmap(%p, 0x%08lX, block=0x%08X, %s\n", mmapAddress, size, biggestDiff, fPath);
			mmapAddress = mmap(mmapAddress, size, protection, MAP_FILE | MAP_FIXED | MAP_PRIVATE, fd, offset);
			if ( mmapAddress == ((void*)(-1)) )
				throw "mmap error";
		}
	}
	
	return 0;
}


static
bool
hasSharedRegionMapFile(void)
{
	int	mib[CTL_MAXNAME];
	int	value = 0;
	size_t	size;

	mib[0] = CTL_KERN;
	mib[1] = KERN_SHREG_PRIVATIZABLE;
	size = sizeof (int);
	if (sysctl(mib, 2, &value, &size, NULL, 0) != 0) {
		value = 0;
	}

	return 0 != value;
}

int
ImageLoaderMachO::sharedRegionMapFilePrivateOutside(int fd,
													uint64_t offsetInFat,
													uint64_t lenInFat,
													uint64_t fileLen,
													const LinkContext& context)
{
	const unsigned int segmentCount = fSegments.size();
	const unsigned int extraZeroFillEntries = getExtraZeroFillEntriesCount();
	const unsigned int regionCount = segmentCount+extraZeroFillEntries;
	_shared_region_mapping_np regions[regionCount];
	initMappingTable(offsetInFat, regions);
	int r = -1;
		// find space somewhere to allocate split seg
		bool foundRoom = false;
		vm_size_t biggestDiff = 0;
		while ( ! foundRoom ) {
			foundRoom = true;
			for(unsigned int i=0; i < regionCount; ++i) {
				vm_address_t addr = sNextAltLoadAddress + regions[i].address - regions[0].address;
				vm_size_t size = regions[i].size ;
				r = vm_allocate(mach_task_self(), &addr, size, false /*only this range*/);
				if ( 0 != r ) {
					// no room here, deallocate what has succeeded so far
					for(unsigned int j=0; j < i; ++j) {
						vm_address_t addr = sNextAltLoadAddress + regions[j].address - regions[0].address;
						vm_size_t size = regions[j].size ;
						(void)vm_deallocate(mach_task_self(), addr, size);
					}
					sNextAltLoadAddress += 0x00100000;  // skip ahead 1MB and try again
					if ( (sNextAltLoadAddress & 0xF0000000) == 0x90000000 )
						throw "can't map split seg anywhere";
					foundRoom = false;
					break;
				}
				vm_size_t high = (regions[i].address + size - regions[0].address) & 0x0FFFFFFF;
				if ( high > biggestDiff )
					biggestDiff = high;
			}
		}
		
		// map in each region
		uintptr_t slide = sNextAltLoadAddress - regions[0].address;
		this->setSlide(slide);
		for(unsigned int i=0; i < regionCount; ++i) {
			if ( (regions[i].init_prot & VM_PROT_ZF) != 0 ) {
				// do nothing vm_allocate() zero-fills by default
			}
			else {
				void* mmapAddress = (void*)(uintptr_t)(regions[i].address + slide);
				size_t size = regions[i].size;
				int protection = 0;
				if ( regions[i].init_prot & VM_PROT_EXECUTE )
					protection   |= PROT_EXEC;
				if ( regions[i].init_prot & VM_PROT_READ )
					protection   |= PROT_READ;
				if ( regions[i].init_prot & VM_PROT_WRITE )
					protection   |= PROT_WRITE;
				off_t offset = regions[i].file_offset;
				//fprintf(stderr, "mmap(%p, 0x%08lX, block=0x%08X, %s\n", mmapAddress, size, biggestDiff, fPath);
				mmapAddress = mmap(mmapAddress, size, protection, MAP_FILE | MAP_FIXED | MAP_PRIVATE, fd, offset);
				if ( mmapAddress == ((void*)(-1)) )
					throw "mmap error";
			}
		}
		// set so next maps right after this one
		sNextAltLoadAddress += biggestDiff; 
		sNextAltLoadAddress = (sNextAltLoadAddress + 4095) & (-4096);
		
		// logging
		if ( context.verboseMapping ) {
			fprintf(stderr, "dyld: Mapping split-seg outside shared region, slid by 0x%08lX %s\n", this->fSlide, this->getPath());
			for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
				Segment* seg = fSegments[segIndex];
				const _shared_region_mapping_np* entry = &regions[entryIndex];
				if ( (entry->init_prot & VM_PROT_ZF) == 0 ) 
					fprintf(stderr, "%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), seg->getActualLoadAddress(), seg->getActualLoadAddress()+seg->getFileSize()-1);
				if ( entryIndex < (regionCount-1) ) {
					const _shared_region_mapping_np* nextEntry = &regions[entryIndex+1];
					if ( (nextEntry->init_prot & VM_PROT_ZF) != 0 ) {
						uint64_t segOffset = nextEntry->address - entry->address;
						fprintf(stderr, "%18s at 0x%08lX->0x%08lX (zerofill)\n",
								seg->getName(), (uintptr_t)(seg->getActualLoadAddress() + segOffset), (uintptr_t)(seg->getActualLoadAddress() + segOffset + nextEntry->size - 1));
						++entryIndex;
					}
				}
			}
		}
		
		return r;
}


void ImageLoaderMachO::mapSegments(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context)
{
	enum SharedRegionState
	{
		kSharedRegionStartState = 0,
		kSharedRegionLoadFileState,
		kSharedRegionMapFileState,
		kSharedRegionMapFilePrivateState,
		kSharedRegionMapFilePrivateMMapState,
		kSharedRegionMapFilePrivateOutsideState,
	};
	static SharedRegionState sSharedRegionState = kSharedRegionStartState;

	// non-split segment libraries handled by super class
	if ( !fIsSplitSeg )
		return ImageLoader::mapSegments(fd, offsetInFat, lenInFat, fileLen, context);
	
	if ( kSharedRegionStartState == sSharedRegionState ) {
		if ( hasSharedRegionMapFile() ) {
			if ( context.slideAndPackDylibs ) { 
				sharedRegionMakePrivate(context);
				// remove underlying submap and block out 0x90000000 to 0xAFFFFFFF
				vm_address_t addr = (vm_address_t)0x90000000;
				vm_deallocate(mach_task_self(), addr, 0x20000000);
				vm_allocate(mach_task_self(), &addr, 0x20000000, false);
				sSharedRegionState = kSharedRegionMapFilePrivateMMapState;
			}
			else if ( context.sharedRegionMode == kUsePrivateSharedRegion ) { 
				sharedRegionMakePrivate(context);
				sSharedRegionState = kSharedRegionMapFilePrivateState;
			}
			else if ( context.sharedRegionMode == kDontUseSharedRegion ) {
				sSharedRegionState = kSharedRegionMapFilePrivateOutsideState;
			}
			else {
				sSharedRegionState = kSharedRegionMapFileState;
			}
		}
		else {
			sSharedRegionState = kSharedRegionLoadFileState;
		}
	}
	
	if ( kSharedRegionLoadFileState == sSharedRegionState ) {
		if ( 0 != sharedRegionLoadFile(fd, offsetInFat, lenInFat, fileLen, context) ) {
			sSharedRegionState = kSharedRegionMapFilePrivateOutsideState;
		}
	}
	else
	if ( kSharedRegionMapFileState == sSharedRegionState ) {
		if ( 0 != sharedRegionMapFile(fd, offsetInFat, lenInFat, fileLen, context) ) {
			sharedRegionMakePrivate(context);
			sSharedRegionState = kSharedRegionMapFilePrivateState;
		}
	}
	
	if ( (kSharedRegionMapFilePrivateState == sSharedRegionState) || (kSharedRegionMapFilePrivateMMapState == sSharedRegionState) ) {
		if ( 0 != sharedRegionMapFilePrivate(fd, offsetInFat, lenInFat, fileLen, context, (kSharedRegionMapFilePrivateMMapState == sSharedRegionState)) ) {
			sSharedRegionState = kSharedRegionMapFilePrivateOutsideState;
		}
	}
	
	if ( kSharedRegionMapFilePrivateOutsideState == sSharedRegionState ) {
		if ( 0 != sharedRegionMapFilePrivateOutside(fd, offsetInFat, lenInFat, fileLen, context) ) {
			throw "mapping error";
		}
	}
}

unsigned int
ImageLoaderMachO::getExtraZeroFillEntriesCount()
{
	// calculate mapping entries
	const unsigned int segmentCount = fSegments.size();
	unsigned int extraZeroFillEntries = 0;
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		if ( seg->hasTrailingZeroFill() )
			++extraZeroFillEntries;
	}
	
	return extraZeroFillEntries;
}

void
ImageLoaderMachO::initMappingTable(uint64_t offsetInFat,
								   _shared_region_mapping_np *mappingTable)
{
	unsigned int segmentCount = fSegments.size();
	for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
		Segment* seg = fSegments[segIndex];
		_shared_region_mapping_np* entry = &mappingTable[entryIndex];
		entry->address			= seg->getActualLoadAddress();
		entry->size				= seg->getFileSize();
		entry->file_offset		= seg->getFileOffset() + offsetInFat;
		entry->init_prot		= VM_PROT_NONE;
		if ( !seg->unaccessible() ) {
			if ( seg->executable() )
				entry->init_prot   |= VM_PROT_EXECUTE;
			if ( seg->readable() )
				entry->init_prot   |= VM_PROT_READ;
			if ( seg->writeable() )
				entry->init_prot   |= VM_PROT_WRITE | VM_PROT_COW;
		}
		entry->max_prot			= entry->init_prot;
		if ( seg->hasTrailingZeroFill() ) {
			_shared_region_mapping_np* zfentry = &mappingTable[++entryIndex];
			zfentry->address		= entry->address + seg->getFileSize();
			zfentry->size			= seg->getSize() - seg->getFileSize();
			zfentry->file_offset	= 0;
			zfentry->init_prot		= entry->init_prot | VM_PROT_COW | VM_PROT_ZF;
			zfentry->max_prot		= zfentry->init_prot;
		}
	}
}
	
int
ImageLoaderMachO::sharedRegionMakePrivate(const LinkContext& context)
{
	if ( context.verboseMapping )
		fprintf(stderr, "dyld: making shared regions private\n");

	// shared mapping failed, so make private copy of shared region and try mapping private
	RegionsVector allRegions;
	context.getAllMappedRegions(allRegions);
	std::vector<_shared_region_range_np> splitSegRegions;
	const unsigned int allRegiontCount = allRegions.size();
	for(unsigned int i=0; i < allRegiontCount; ++i){
		MappedRegion region = allRegions[i];
		uint8_t highByte = region.address >> 28;
		if ( (highByte == 9) || (highByte == 0xA) ) {
			_shared_region_range_np splitRegion;
			splitRegion.address = region.address;
			splitRegion.size = region.size;
			splitSegRegions.push_back(splitRegion);
		}
	}
	int result = _shared_region_make_private_np(splitSegRegions.size(), &splitSegRegions[0]);
	// notify gdb or other lurkers that this process is no longer using the shared region
	dyld_all_image_infos.processDetachedFromSharedRegion = true;
	return result;
}

int
ImageLoaderMachO::sharedRegionMapFile(int fd,
                                         uint64_t offsetInFat,
                                         uint64_t lenInFat,
                                         uint64_t fileLen,
                                         const LinkContext& context)
{
	// build table of segments to map
	const unsigned int segmentCount = fSegments.size();
	const unsigned int extraZeroFillEntries = getExtraZeroFillEntriesCount();
	const unsigned int mappingTableCount = segmentCount+extraZeroFillEntries;
	_shared_region_mapping_np mappingTable[mappingTableCount];
	initMappingTable(offsetInFat, mappingTable);
// 	uint64_t slide;
	uint64_t *slidep = NULL;

	// try to map it in shared
	int r = _shared_region_map_file_np(fd, mappingTableCount, mappingTable, slidep);
	if ( 0 == r ) {
		if(NULL != slidep && 0 != *slidep) {
			// update with actual load addresses
		}
		if ( context.verboseMapping ) {
			fprintf(stderr, "dyld: Mapping split-seg shared %s\n", this->getPath());
			for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
				Segment* seg = fSegments[segIndex];
				const _shared_region_mapping_np* entry = &mappingTable[entryIndex];
				if ( (entry->init_prot & VM_PROT_ZF) == 0 ) 
					fprintf(stderr, "%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), seg->getActualLoadAddress(), seg->getActualLoadAddress()+seg->getFileSize()-1);
				if ( entryIndex < (mappingTableCount-1) ) {
					const _shared_region_mapping_np* nextEntry = &mappingTable[entryIndex+1];
					if ( (nextEntry->init_prot & VM_PROT_ZF) != 0 ) {
						uint64_t segOffset = nextEntry->address - entry->address;
						fprintf(stderr, "%18s at 0x%08lX->0x%08lX\n",
								seg->getName(), (uintptr_t)(seg->getActualLoadAddress() + segOffset), (uintptr_t)(seg->getActualLoadAddress() + segOffset + nextEntry->size - 1));
						++entryIndex;
					}
				}
			}
		}
	}
	return r;
}


int
ImageLoaderMachO::sharedRegionMapFilePrivate(int fd,
											 uint64_t offsetInFat,
											 uint64_t lenInFat,
											 uint64_t fileLen,
											 const LinkContext& context,
											 bool usemmap)
{
	const unsigned int segmentCount = fSegments.size();

	// adjust base address of segments to pack next to last dylib
	if ( context.slideAndPackDylibs ) {
		uintptr_t lowestReadOnly = (uintptr_t)(-1);
		uintptr_t lowestWritable = (uintptr_t)(-1);
		for(unsigned int segIndex=0; segIndex < segmentCount; ++segIndex){
			Segment* seg = fSegments[segIndex];
			uintptr_t segEnd = seg->getActualLoadAddress();
			if ( seg->writeable() ) {
				if ( segEnd < lowestWritable )
					lowestWritable = segEnd;
			}
			else {
				if ( segEnd < lowestReadOnly )
					lowestReadOnly = segEnd;
			}
		}
		uintptr_t baseAddress;
		if ( lowestWritable - 256*1024*1024 < lowestReadOnly )
			baseAddress = lowestWritable - 256*1024*1024;
		else
			baseAddress = lowestReadOnly;
		// record that we want dylb slid to fgNextSplitSegAddress
		this->setSlide(fgNextSplitSegAddress - baseAddress);
	}
	
	// build table of segments to map
	const unsigned int extraZeroFillEntries = getExtraZeroFillEntriesCount();
	const unsigned int mappingTableCount = segmentCount+extraZeroFillEntries;
	_shared_region_mapping_np mappingTable[mappingTableCount];
	initMappingTable(offsetInFat, mappingTable);
	uint64_t slide = 0;

	// try map it in privately (don't allow sliding if we pre-calculated the load address to pack dylibs)
	int r;
	if ( usemmap )
		r = _shared_region_map_file_with_mmap(fd, mappingTableCount, mappingTable);
	else
		r = _shared_region_map_file_np(fd, mappingTableCount, mappingTable, context.slideAndPackDylibs ? NULL : &slide);
	if ( 0 == r ) {
		if ( 0 != slide ) {
			slide = (slide) & (-4096); // round down to page boundary
			this->setSlide(slide);
		}
		if ( context.verboseMapping ) {
			if ( slide == 0 )
				fprintf(stderr, "dyld: Mapping split-seg un-shared %s\n", this->getPath());
			else
				fprintf(stderr, "dyld: Mapping split-seg un-shared slid by 0x%08llX %s\n", slide, this->getPath());
			for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
				Segment* seg = fSegments[segIndex];
				const _shared_region_mapping_np* entry = &mappingTable[entryIndex];
				if ( (entry->init_prot & VM_PROT_ZF) == 0 ) 
					fprintf(stderr, "%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), seg->getActualLoadAddress(), seg->getActualLoadAddress()+seg->getFileSize()-1);
				if ( entryIndex < (mappingTableCount-1) ) {
					const _shared_region_mapping_np* nextEntry = &mappingTable[entryIndex+1];
					if ( (nextEntry->init_prot & VM_PROT_ZF) != 0 ) {
						uint64_t segOffset = nextEntry->address - entry->address;
						fprintf(stderr, "%18s at 0x%08lX->0x%08lX (zerofill)\n",
								seg->getName(), (uintptr_t)(seg->getActualLoadAddress() + segOffset), (uintptr_t)(seg->getActualLoadAddress() + segOffset + nextEntry->size - 1));
						++entryIndex;
					}
				}
			}
		}
		if ( context.slideAndPackDylibs ) {
			// calculate where next split-seg dylib can load
			uintptr_t largestReadOnly = 0;
			uintptr_t largestWritable = 0;
			for (unsigned int segIndex=0; segIndex < segmentCount; ++segIndex) {
				Segment* seg = fSegments[segIndex];
				uintptr_t segEnd = seg->getActualLoadAddress()+seg->getSize();
				segEnd = (segEnd+4095) & (-4096); // page align
				if ( seg->writeable() ) {
					if ( segEnd > largestWritable )
						largestWritable = segEnd;
				}
				else {
					if ( segEnd > largestReadOnly )
						largestReadOnly = segEnd;
				}
			}
			if ( largestWritable - 256*1024*1024 > largestReadOnly )
				fgNextSplitSegAddress = largestWritable - 256*1024*1024;
			else
				fgNextSplitSegAddress = largestReadOnly;
		}
	}
	if ( context.slideAndPackDylibs && (r != 0) )
		throwf("can't rebase split-seg dylib %s because shared_region_map_file_np() returned %d", this->getPath(), r);
	
	return r;
}


int
ImageLoaderMachO::sharedRegionLoadFile(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context)
{
	
	// map in split segment file at random address, then tell kernel to share it
	void* loadAddress = 0;
	loadAddress = mmap(NULL, fileLen, PROT_READ, MAP_FILE, fd, 0);
	if ( loadAddress == ((void*)(-1)) )
		throw "mmap error";

	// calculate mapping entries
	const unsigned int segmentCount = fSegments.size();
	unsigned int extraZeroFillEntries = getExtraZeroFillEntriesCount();
	
	// build table of segments to map
	const unsigned int mappingTableCount = segmentCount+extraZeroFillEntries;
	const uintptr_t baseAddress = fSegments[0]->getPreferredLoadAddress();
	sf_mapping mappingTable[mappingTableCount];
	initMappingTable(offsetInFat, mappingTable, baseAddress);
	
	
	// use load_shared_file() to map all segments at once
	int flags = 0; // might need to set NEW_LOCAL_SHARED_REGIONS on first use
	static bool firstTime = true;
	if ( firstTime ) {
		// when NEW_LOCAL_SHARED_REGIONS bit is set, this process will get is own shared region
		// this is used by Xcode to prevent development libraries from polluting the global shared segment
		if ( context.sharedRegionMode == kUsePrivateSharedRegion )
			flags |= NEW_LOCAL_SHARED_REGIONS;
		firstTime = false;
	}
	
	caddr_t base_address = (caddr_t)baseAddress;
	kern_return_t r;
	r = load_shared_file(   (char*)fPath,		// path of file to map shared
							(char*)loadAddress, // beginning of local copy of sharable pages in file
							fileLen,			// end of shareable pages in file
							&base_address,		// beginning of address range to map
							mappingTableCount,  // number of entres in array of sf_mapping
							mappingTable,		// the array of sf_mapping
							&flags);			// in/out flags
	if ( 0 != r ) {
		// try again but tell kernel it is ok to slide
		flags |= ALTERNATE_LOAD_SITE;
		r = load_shared_file((char*)fPath,(char*)loadAddress, fileLen, &base_address,	
							mappingTableCount, mappingTable, &flags);
	}
	
	// unmap file from random address now that they are (hopefully) mapped into the shared region
	munmap(loadAddress, fileLen);

	if ( 0 == r ) {
		if ( base_address != (caddr_t)baseAddress )
			this->setSlide((uintptr_t)base_address - baseAddress);
		if ( context.verboseMapping ) {
			if ( base_address != (caddr_t)baseAddress )
				fprintf(stderr, "dyld: Mapping split-seg load_shared_alt_region %s\n", this->getPath());
			else
				fprintf(stderr, "dyld: Mapping split-seg load_shared %s\n", this->getPath());
			for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
				Segment* seg = fSegments[segIndex];
				const sf_mapping* entry = &mappingTable[entryIndex];
				if ( (entry->protection & VM_PROT_ZF) == 0 )
					fprintf(stderr, "%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), seg->getActualLoadAddress(), seg->getActualLoadAddress()+seg->getFileSize()-1);
				if ( entryIndex < (mappingTableCount-1) ) {
					const sf_mapping* nextEntry = &mappingTable[entryIndex+1];
					if ( (nextEntry->protection & VM_PROT_ZF) != 0 ) {
						fprintf(stderr, "%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), (uintptr_t)(nextEntry->mapping_offset + base_address), (uintptr_t)(nextEntry->mapping_offset + base_address + nextEntry->size - 1));
						++entryIndex;
					}
				}
			}
		}
	}
	return r;
}
void
ImageLoaderMachO::initMappingTable(uint64_t offsetInFat,
								   sf_mapping *mappingTable,
								   uintptr_t baseAddress)
{
	unsigned int segmentCount = fSegments.size();
	for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
		Segment* seg = fSegments[segIndex];
		sf_mapping* entry = &mappingTable[entryIndex];
		entry->mapping_offset   = seg->getPreferredLoadAddress() - baseAddress;
		entry->size				= seg->getFileSize();
		entry->file_offset		= seg->getFileOffset() + offsetInFat;
		entry->protection		= VM_PROT_NONE;
		if ( !seg->unaccessible() ) {
			if ( seg->executable() )
				entry->protection   |= VM_PROT_EXECUTE;
			if ( seg->readable() )
				entry->protection   |= VM_PROT_READ;
			if ( seg->writeable() )
				entry->protection   |= VM_PROT_WRITE | VM_PROT_COW;
		}
		
		entry->cksum			= 0;
		if ( seg->hasTrailingZeroFill() ) {
			sf_mapping* zfentry = &mappingTable[++entryIndex];
			zfentry->mapping_offset = entry->mapping_offset + seg->getFileSize();
			zfentry->size			= seg->getSize() - seg->getFileSize();
			zfentry->file_offset	= 0;
			zfentry->protection		= entry->protection | VM_PROT_COW | VM_PROT_ZF;
			zfentry->cksum			= 0;
		}
	}
}

#endif //  !__LP64__  split segs not supported for 64-bits


void ImageLoaderMachO::setSlide(intptr_t slide)
{
	fSlide = slide;
}

void ImageLoaderMachO::parseLoadCmds()
{
	// now that segments are mapped in, get real fMachOData, fLinkEditBase, and fSlide
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		// set up pointer to __LINKEDIT segment
		if ( strcmp(seg->getName(),"__LINKEDIT") == 0 ) 
			fLinkEditBase = (uint8_t*)(seg->getActualLoadAddress() - seg->getFileOffset());
		// __TEXT segment always starts at beginning of file and contains mach_header and load commands
		if ( strcmp(seg->getName(),"__TEXT") == 0 ) {
			if ( seg->hasFixUps() )
				fTextSegmentWithFixups = (SegmentMachO*)seg;
		}
		// some segment always starts at beginning of file and contains mach_header and load commands
		if ( (seg->getFileOffset() == 0) && (seg->getFileSize() != 0) ) {
			fMachOData = (uint8_t*)(seg->getActualLoadAddress());
		}
	}

	// walk load commands (mapped in at start of __TEXT segment)
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SYMTAB:
				{
					const struct symtab_command* symtab = (struct symtab_command*)cmd;
					fStrings = (const char*)&fLinkEditBase[symtab->stroff];
					fSymbolTable = (struct macho_nlist*)(&fLinkEditBase[symtab->symoff]);
				}
				break;
			case LC_DYSYMTAB:
				fDynamicInfo = (struct dysymtab_command*)cmd;
				break;
			case LC_SUB_UMBRELLA:
				fHasSubUmbrella = true;
				break;
			case LC_SUB_FRAMEWORK:
				{
					const struct sub_framework_command* subf = (struct sub_framework_command*)cmd;
					fReExportThruFramework = (char*)cmd + subf->umbrella.offset;
				}
				break;
			case LC_SUB_LIBRARY:
				fHasSubLibraries = true;
				break;
			case LC_ROUTINES_COMMAND:
				fDashInit = (struct macho_routines_command*)cmd;
				break;
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const bool isDataSeg = (strcmp(seg->segname, "__DATA") == 0);
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( type == S_MOD_INIT_FUNC_POINTERS )
							fModInitSection = sect;
						else if ( type == S_MOD_TERM_FUNC_POINTERS )
							fModTermSection = sect;
						else if ( isDataSeg && (strcmp(sect->sectname, "__dyld") == 0) ) {
								fDATAdyld = sect;
						}
						else if ( isDataSeg && (strcmp(sect->sectname, "__image_notify") == 0) )
							fImageNotifySection = sect;
					}
				}
				break;
			case LC_TWOLEVEL_HINTS:
				fTwoLevelHints = (struct twolevel_hints_command*)cmd;
				break;
			case LC_ID_DYLIB:
				{
					fDylibID = (struct dylib_command*)cmd;
				}
				break;
			case LC_LOAD_WEAK_DYLIB:
				// do nothing, just prevent LC_REQ_DYLD exception from occuring
				break;
			default:
				if ( (cmd->cmd & LC_REQ_DYLD) != 0 )
					throwf("unknown required load command 0x%08X", cmd->cmd);
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}




const char* ImageLoaderMachO::getInstallPath() const
{
	if ( fDylibID != NULL ) {
		return (char*)fDylibID + fDylibID->dylib.name.offset;
	}
	return NULL;
}

// test if this image is re-exported through parent (the image that loaded this one)
bool ImageLoaderMachO::isSubframeworkOf(const LinkContext& context, const ImageLoader* parent) const
{
	if ( fReExportThruFramework != NULL ) {
		// need to match LC_SUB_FRAMEWORK string against the leaf name of the install location of parent...
		const char* parentInstallPath = parent->getInstallPath();
		if ( parentInstallPath != NULL ) {
			const char* lastSlash = strrchr(parentInstallPath, '/');
			if ( lastSlash != NULL ) {
				if ( strcmp(&lastSlash[1], fReExportThruFramework) == 0 )
					return true;
				if ( context.imageSuffix != NULL ) {
					// when DYLD_IMAGE_SUFFIX is used, lastSlash string needs imageSuffix removed from end
					char reexportAndSuffix[strlen(context.imageSuffix)+strlen(fReExportThruFramework)+1];
					strcpy(reexportAndSuffix, fReExportThruFramework);
					strcat(reexportAndSuffix, context.imageSuffix);
					if ( strcmp(&lastSlash[1], reexportAndSuffix) == 0 )
						return true;
				}
			}
		}
	}
	return false;
}

// test if child is re-exported 
bool ImageLoaderMachO::hasSubLibrary(const LinkContext& context, const ImageLoader* child) const
{
	if ( fHasSubLibraries ) {
		// need to match LC_SUB_LIBRARY string against the leaf name (without extension) of the install location of child...
		const char* childInstallPath = child->getInstallPath();
		if ( childInstallPath != NULL ) {
			const char* lastSlash = strrchr(childInstallPath, '/');
			if ( lastSlash != NULL ) {
				const char* firstDot = strchr(lastSlash, '.');
				int len;
				if ( firstDot == NULL )
					len = strlen(lastSlash);
				else
					len = firstDot-lastSlash-1;
				char childLeafName[len+1];
				strncpy(childLeafName, &lastSlash[1], len);
				childLeafName[len] = '\0';
				const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
				const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
				const struct load_command* cmd = cmds;
				for (uint32_t i = 0; i < cmd_count; ++i) {
					switch (cmd->cmd) {
						case LC_SUB_LIBRARY:
							{
								const struct sub_library_command* lib = (struct sub_library_command*)cmd;
								const char* aSubLibName = (char*)cmd + lib->sub_library.offset;
								if ( strcmp(aSubLibName, childLeafName) == 0 )
									return true;
								if ( context.imageSuffix != NULL ) {
									// when DYLD_IMAGE_SUFFIX is used, childLeafName string needs imageSuffix removed from end
									char aSubLibNameAndSuffix[strlen(context.imageSuffix)+strlen(aSubLibName)+1];
									strcpy(aSubLibNameAndSuffix, aSubLibName);
									strcat(aSubLibNameAndSuffix, context.imageSuffix);
									if ( strcmp(aSubLibNameAndSuffix, childLeafName) == 0 )
										return true;
								}
							}
							break;
					}
					cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
				}
			}
		}
	}
	if ( fHasSubUmbrella ) {
		// need to match LC_SUB_UMBRELLA string against the leaf name of install location of child...
		const char* childInstallPath = child->getInstallPath();
		if ( childInstallPath != NULL ) {
			const char* lastSlash = strrchr(childInstallPath, '/');
			if ( lastSlash != NULL ) {
				const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
				const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
				const struct load_command* cmd = cmds;
				for (uint32_t i = 0; i < cmd_count; ++i) {
					switch (cmd->cmd) {
						case LC_SUB_UMBRELLA:
							{
								const struct sub_umbrella_command* um = (struct sub_umbrella_command*)cmd;
								const char* aSubUmbrellaName = (char*)cmd + um->sub_umbrella.offset;
								if ( strcmp(aSubUmbrellaName, &lastSlash[1]) == 0 )
									return true;
								if ( context.imageSuffix != NULL ) {
									// when DYLD_IMAGE_SUFFIX is used, lastSlash string needs imageSuffix removed from end
									char umbrellaAndSuffix[strlen(context.imageSuffix)+strlen(aSubUmbrellaName)+1];
									strcpy(umbrellaAndSuffix, aSubUmbrellaName);
									strcat(umbrellaAndSuffix, context.imageSuffix);
									if ( strcmp(umbrellaAndSuffix, &lastSlash[1]) == 0 )
										return true;
								}
							}
							break;
					}
					cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
				}
			}
		}
	}
	return false;
}

	
void* ImageLoaderMachO::getMain() const
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_UNIXTHREAD:
			{
			#if __ppc__
				const ppc_thread_state_t* registers = (ppc_thread_state_t*)(((char*)cmd) + 16);
				return (void*)registers->srr0;
			#elif __ppc64__
				const ppc_thread_state64_t* registers = (ppc_thread_state64_t*)(((char*)cmd) + 16);
				return (void*)registers->srr0;
			#elif __i386__
				const i386_thread_state_t* registers = (i386_thread_state_t*)(((char*)cmd) + 16);
				return (void*)registers->eip;
			#else
				#warning need processor specific code
			#endif
			}
			break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return NULL;
}


uint32_t ImageLoaderMachO::doGetDependentLibraryCount()
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	uint32_t count = 0;
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
				++count;
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return count;
}

void ImageLoaderMachO::doGetDependentLibraries(DependentLibrary libs[])
{
	uint32_t index = 0;
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			{
				const struct dylib_command* dylib = (struct dylib_command*)cmd;
				DependentLibrary* lib = &libs[index++];
				lib->name = (char*)cmd + dylib->dylib.name.offset;
				//lib->name = strdup((char*)cmd + dylib->dylib.name.offset);
				lib->image = NULL;
				lib->info.checksum = dylib->dylib.timestamp;
				lib->info.minVersion = dylib->dylib.compatibility_version;
				lib->info.maxVersion = dylib->dylib.current_version;
				lib->required = (cmd->cmd == LC_LOAD_DYLIB);
				lib->checksumMatches = false;
				lib->isReExported = false;
			}
			break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

ImageLoader::LibraryInfo ImageLoaderMachO::doGetLibraryInfo()
{
	LibraryInfo info;
	if ( fDylibID != NULL ) {
		info.minVersion = fDylibID->dylib.compatibility_version;
		info.maxVersion = fDylibID->dylib.current_version;
		info.checksum = fDylibID->dylib.timestamp;
	}
	else {
		info.minVersion = 0;
		info.maxVersion = 0;		
		info.checksum = 0;
	}
	return info;
}


uintptr_t ImageLoaderMachO::getRelocBase()
{
	if ( fIsSplitSeg ) {
		// in split segment libraries r_address is offset from first writable segment
		const unsigned int segmentCount = fSegments.size();
		for(unsigned int i=0; i < segmentCount; ++i){
			Segment* seg = fSegments[i];
			if ( seg->writeable() ) {
				return seg->getActualLoadAddress();
			}
		}
	}
	
	// in non-split segment libraries r_address is offset from first segment
	return fSegments[0]->getActualLoadAddress();
}

#if __ppc__
static inline void otherRelocsPPC(uintptr_t* locationToFix, uint8_t relocationType, uint16_t otherHalf, uintptr_t slide)
{
	// low 16 bits of 32-bit ppc instructions need fixing
	struct ppcInstruction { uint16_t opcode; int16_t immediateValue; };
	ppcInstruction* instruction = (ppcInstruction*)locationToFix;
	//uint32_t before = *((uint32_t*)locationToFix);
	switch ( relocationType )
	{
		case PPC_RELOC_LO16: 
			instruction->immediateValue = ((otherHalf << 16) | instruction->immediateValue) + slide;
			break;
		case PPC_RELOC_HI16: 
			instruction->immediateValue = ((((instruction->immediateValue << 16) | otherHalf) + slide) >> 16);
			break;
		case PPC_RELOC_HA16: 
			int16_t signedOtherHalf = (int16_t)(otherHalf & 0xffff);
			uint32_t temp = (instruction->immediateValue << 16) + signedOtherHalf + slide;
			if ( (temp & 0x00008000) != 0 )
				temp += 0x00008000;
			instruction->immediateValue = temp >> 16;
	}
	//uint32_t after = *((uint32_t*)locationToFix);
	//fprintf(stderr, "dyld: ppc fixup %0p type %d from 0x%08X to 0x%08X\n", locationToFix, relocationType, before, after);
}
#endif

void ImageLoaderMachO::doRebase(const LinkContext& context)
{
	// if prebound and loaded at prebound address, then no need to rebase
	// Note: you might think that the check for allDependentLibrariesAsWhenPreBound() is not needed
	// but it is.  If a dependent library changed, this image's lazy pointers into that library
	// need to be updated (reset back to lazy binding handler).  That work is done most easily
	// here because there is a PPC_RELOC_PB_LA_PTR reloc record for each lazy pointer.
	if ( this->usablePrebinding(context) && this->usesTwoLevelNameSpace() ) {
		// skip rebasing cause prebound and prebinding not disabled
		++fgImagesWithUsedPrebinding; // bump totals for statistics
		return;
	}
		
	// print why prebinding was not used
	if ( context.verbosePrebinding ) {
		if ( !this->isPrebindable() ) {
			fprintf(stderr, "dyld: image not prebound, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( fSlide != 0 ) {
			fprintf(stderr, "dyld: image slid, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( !this->allDependentLibrariesAsWhenPreBound() ) {
			fprintf(stderr, "dyld: dependent libraries changed, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( !this->usesTwoLevelNameSpace() ){
			fprintf(stderr, "dyld: image uses flat-namespace so, parts of prebinding ignored %s\n", this->getPath());
		}
		else {
			fprintf(stderr, "dyld: environment variable disabled use of prebinding in %s\n", this->getPath());
		}
	}

	// if there are __TEXT fixups, temporarily make __TEXT writable
	if ( fTextSegmentWithFixups != NULL ) 
		fTextSegmentWithFixups->tempWritable();

	// cache this value that is used in the following loop
	register const uintptr_t slide = this->fSlide;

	// loop through all local (internal) relocation records
	const uintptr_t relocBase = this->getRelocBase();
	const relocation_info* const relocsStart = (struct relocation_info*)(&fLinkEditBase[fDynamicInfo->locreloff]);
	const relocation_info* const relocsEnd = &relocsStart[fDynamicInfo->nlocrel];
	for (const relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		if ( (reloc->r_address & R_SCATTERED) == 0 ) {
			if ( reloc->r_symbolnum == R_ABS ) {
				// ignore absolute relocations
			}
			else if (reloc->r_length == RELOC_SIZE) {
				switch(reloc->r_type) {
					case GENERIC_RELOC_VANILLA:
						*((uintptr_t*)(reloc->r_address + relocBase)) += slide;
						break;
		#if __ppc__
					case PPC_RELOC_HI16: 
					case PPC_RELOC_LO16: 
					case PPC_RELOC_HA16: 
						// some tools leave object file relocations in linked images
						otherRelocsPPC((uintptr_t*)(reloc->r_address + relocBase), reloc->r_type, reloc[1].r_address, slide);
						++reloc; // these relocations come in pairs, skip next
						break;
		#endif
					default:
						throw "unknown local relocation type";
				}
			}
			else {
				throw "bad local relocation length";
			}
		}
		else {
			const struct scattered_relocation_info* sreloc = (struct scattered_relocation_info*)reloc;
			if (sreloc->r_length == RELOC_SIZE) {
				uintptr_t* locationToFix = (uintptr_t*)(sreloc->r_address + relocBase);
				switch(sreloc->r_type) {
					case GENERIC_RELOC_VANILLA:
						*locationToFix += slide;
						break;
		#if __ppc__ || __ppc64__
					case PPC_RELOC_PB_LA_PTR:
						// should only see these in prebound images, and we got here so prebinding is being ignored
						*locationToFix = sreloc->r_value + slide;
						break;
		#endif
		#if __ppc__
					case PPC_RELOC_HI16: 
					case PPC_RELOC_LO16: 
					case PPC_RELOC_HA16: 
						// Metrowerks compiler sometimes leaves object file relocations in linked images???
						++reloc; // these relocations come in pairs, get next one
						otherRelocsPPC(locationToFix, sreloc->r_type, reloc->r_address, slide);
						break;
		#endif
		#if __i386__
					case GENERIC_RELOC_PB_LA_PTR:
						// should only see these in prebound images, and we got here so prebinding is being ignored
						*locationToFix = sreloc->r_value + slide;
						break;
		#endif
					default:
						throw "unknown local scattered relocation type";
				}
			}
			else {
				throw "bad local scattered relocation length";
			}
		}
	}
	
	// if there were __TEXT fixups, restore write protection
	if ( fTextSegmentWithFixups != NULL ) {
		fTextSegmentWithFixups->setPermissions();
		sys_icache_invalidate((void*)fTextSegmentWithFixups->getActualLoadAddress(), fTextSegmentWithFixups->getSize());
	}
	
	// update stats
	fgTotalRebaseFixups += fDynamicInfo->nlocrel;
}


const struct macho_nlist* ImageLoaderMachO::binarySearchWithToc(const char* key, const char stringPool[], const struct macho_nlist symbols[], 
												const struct dylib_table_of_contents toc[], uint32_t symbolCount, uint32_t hintIndex)
{
	int32_t high = symbolCount-1;
	int32_t mid = hintIndex;
	
	// handle out of range hint
	if ( mid >= (int32_t)symbolCount ) {
		mid = symbolCount/2;
		++ImageLoaderMachO::fgUnhintedBinaryTreeSearchs;
	}
	else {
		++ImageLoaderMachO::fgHintedBinaryTreeSearchs;
	}

	for (int32_t low = 0; low <= high; mid = (low+high)/2) {
		const uint32_t index = toc[mid].symbol_index;
		const struct macho_nlist* pivot = &symbols[index];
		const char* pivotStr = &stringPool[pivot->n_un.n_strx];
#if LINKEDIT_USAGE_DEBUG
		noteAccessedLinkEditAddress(&toc[mid]);
		noteAccessedLinkEditAddress(pivot);
		noteAccessedLinkEditAddress(pivotStr);
#endif
		int cmp = astrcmp(key, pivotStr);
		if ( cmp == 0 )
			return pivot;
		if ( cmp > 0 ) {
			// key > pivot 
			low = mid + 1;
		}
		else {
			// key < pivot 
			high = mid - 1;
		}
	}
	return NULL;
}

const struct macho_nlist* ImageLoaderMachO::binarySearch(const char* key, const char stringPool[], const struct macho_nlist symbols[], uint32_t symbolCount)
{
	++ImageLoaderMachO::fgUnhintedBinaryTreeSearchs;
	const struct macho_nlist* base = symbols;
	for (uint32_t n = symbolCount; n > 0; n /= 2) {
		const struct macho_nlist* pivot = &base[n/2];
		const char* pivotStr = &stringPool[pivot->n_un.n_strx];
#if LINKEDIT_USAGE_DEBUG
		noteAccessedLinkEditAddress(pivot);
		noteAccessedLinkEditAddress(pivotStr);
#endif
		int cmp = astrcmp(key, pivotStr);
		if ( cmp == 0 )
			return pivot;
		if ( cmp > 0 ) {
			// key > pivot 
			// move base to symbol after pivot
			base = &pivot[1];
			--n; 
		}
		else {
			// key < pivot 
			// keep same base
		}
	}
	return NULL;
}

const ImageLoader::Symbol* ImageLoaderMachO::findExportedSymbol(const char* name, const void* hint, bool searchReExports, ImageLoader** foundIn) const
{
	const struct macho_nlist* sym = NULL;
	const struct twolevel_hint* theHint = (struct twolevel_hint*)hint;
	if ( fDynamicInfo->tocoff == 0 )
		sym = binarySearch(name, fStrings, &fSymbolTable[fDynamicInfo->iextdefsym], fDynamicInfo->nextdefsym);
	else {
		uint32_t start = fDynamicInfo->nextdefsym;
		if ( theHint != NULL )
			 start = theHint->itoc;
		if ( (theHint == NULL) || (theHint->isub_image == 0) ) {
			sym = binarySearchWithToc(name, fStrings, fSymbolTable, (dylib_table_of_contents*)&fLinkEditBase[fDynamicInfo->tocoff], 
										fDynamicInfo->ntoc, start);
		}
	}
	if ( sym != NULL ) {
		if ( foundIn != NULL )
			*foundIn = (ImageLoader*)this;		
			
		return (const Symbol*)sym;
	}
	
	if ( searchReExports ) {
		// hint might tell us to try a particular subimage
		if ( (theHint != NULL) && (theHint->isub_image > 0) && (theHint->isub_image <= fLibrariesCount) ) {
			// isub_image is an index into a list that is sorted non-rexported images first
			uint32_t index = 0;
			ImageLoader* target = NULL;
			// pass one, only look at sub-frameworks
			for (uint32_t i=0; i < fLibrariesCount; ++i) {
				DependentLibrary& libInfo =  fLibraries[i];
				if ( libInfo.isSubFramework && (libInfo.image != NULL)) {
					if ( ++index == theHint->isub_image ) {
						target = libInfo.image;
						break;
					}
				}
			}
			if (target != NULL) {
				// pass two, only look at non-sub-framework-reexports
				for (uint32_t i=0; i < fLibrariesCount; ++i) {
					DependentLibrary& libInfo =  fLibraries[i];
					if ( libInfo.isReExported && !libInfo.isSubFramework && (libInfo.image != NULL) ) {
						if ( ++index == theHint->isub_image ) {
							target = libInfo.image;
							break;
						}
					}
				}
			}
			if (target != NULL) {
				const Symbol* result = target->findExportedSymbol(name, NULL, searchReExports, foundIn);
				if ( result != NULL )
					return result;
			}
		}
		
		// hint failed, try all sub images
		// pass one, only look at sub-frameworks
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& libInfo =  fLibraries[i];
			if ( (libInfo.image != NULL) && libInfo.isSubFramework ) {
				const Symbol* result = libInfo.image->findExportedSymbol(name, NULL, searchReExports, foundIn);
				if ( result != NULL )
					return result;
			}
		}
		// pass two, only look at non-sub-framework-reexports
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& libInfo =  fLibraries[i];
			if ( (libInfo.image != NULL) && libInfo.isReExported && !libInfo.isSubFramework ) {
				const Symbol* result = libInfo.image->findExportedSymbol(name, NULL, searchReExports, foundIn);
				if ( result != NULL )
					return result;
			}
		}
	}
	
	// last change: the hint is wrong (non-zero but actually in this image)
	if ( (theHint != NULL) && (theHint->isub_image != 0) ) {
		sym = binarySearchWithToc(name, fStrings, fSymbolTable, (dylib_table_of_contents*)&fLinkEditBase[fDynamicInfo->tocoff], 
										fDynamicInfo->ntoc, fDynamicInfo->nextdefsym);
		if ( sym != NULL ) {
			if ( foundIn != NULL ) 
				*foundIn = (ImageLoader*)this;
			return (const Symbol*)sym;
		}
	}

	return NULL;
}


uintptr_t ImageLoaderMachO::getExportedSymbolAddress(const Symbol* sym) const
{
	const struct macho_nlist* nlistSym = (const struct macho_nlist*)sym;
	return nlistSym->n_value + fSlide;
}

ImageLoader::DefinitionFlags ImageLoaderMachO::getExportedSymbolInfo(const Symbol* sym) const
{
	const struct macho_nlist* nlistSym = (const struct macho_nlist*)sym;
	if ( (nlistSym->n_desc & N_WEAK_DEF) != 0 )
		return kWeakDefinition;
	return kNoDefinitionOptions;
}

const char* ImageLoaderMachO::getExportedSymbolName(const Symbol* sym) const
{
	const struct macho_nlist* nlistSym = (const struct macho_nlist*)sym;
	return &fStrings[nlistSym->n_un.n_strx];
}

uint32_t ImageLoaderMachO::getExportedSymbolCount() const
{
	return fDynamicInfo->nextdefsym;
}


const ImageLoader::Symbol* ImageLoaderMachO::getIndexedExportedSymbol(uint32_t index) const
{
	if ( index < fDynamicInfo->nextdefsym ) {
		const struct macho_nlist* sym = &fSymbolTable[fDynamicInfo->iextdefsym + index];
		return (const ImageLoader::Symbol*)sym;
	}
	return NULL;
}


uint32_t ImageLoaderMachO::getImportedSymbolCount() const
{
	return fDynamicInfo->nundefsym;
}


const ImageLoader::Symbol* ImageLoaderMachO::getIndexedImportedSymbol(uint32_t index) const
{
	if ( index < fDynamicInfo->nundefsym ) {
		const struct macho_nlist* sym = &fSymbolTable[fDynamicInfo->iundefsym + index];
		return (const ImageLoader::Symbol*)sym;
	}
	return NULL;
}


ImageLoader::ReferenceFlags ImageLoaderMachO::geImportedSymbolInfo(const ImageLoader::Symbol* sym) const
{
	const struct macho_nlist* nlistSym = (const struct macho_nlist*)sym;
	ImageLoader::ReferenceFlags flags = kNoReferenceOptions;
	if ( ((nlistSym->n_type & N_TYPE) == N_UNDF) && (nlistSym->n_value != 0) )
		flags |= ImageLoader::kTentativeDefinition;
	if ( (nlistSym->n_desc & N_WEAK_REF) != 0 )
		flags |= ImageLoader::kWeakReference;
	return flags;
}


const char* ImageLoaderMachO::getImportedSymbolName(const ImageLoader::Symbol* sym) const
{
	const struct macho_nlist* nlistSym = (const struct macho_nlist*)sym;
	return &fStrings[nlistSym->n_un.n_strx];
}


bool ImageLoaderMachO::getSectionContent(const char* segmentName, const char* sectionName, void** start, size_t* length)
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if ( (strcmp(sect->segname, segmentName) == 0) && (strcmp(sect->sectname, sectionName) == 0) ) {
							*start = (uintptr_t*)(sect->addr + fSlide);
							*length = sect->size;
							return true;
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return false;
}


bool ImageLoaderMachO::findSection(const void* imageInterior, const char** segmentName, const char** sectionName, size_t* sectionOffset)
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	const uintptr_t unslidInteriorAddress = (uintptr_t)imageInterior - this->getSlide();
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					if ( (unslidInteriorAddress >= seg->vmaddr) && (unslidInteriorAddress < (seg->vmaddr+seg->vmsize)) ) {
						const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
						const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
						for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
							if ((sect->addr <= unslidInteriorAddress) && (unslidInteriorAddress < (sect->addr+sect->size))) {
								if ( segmentName != NULL )
									*segmentName = sect->segname;
								if ( sectionName != NULL )
									*sectionName = sect->sectname;
								if ( sectionOffset != NULL )
									*sectionOffset = unslidInteriorAddress - sect->addr;
								return true;
							}
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return false;
}


bool ImageLoaderMachO::symbolRequiresCoalescing(const struct macho_nlist* symbol)
{
	// if a define and weak ==> coalesced 
	if ( ((symbol->n_type & N_TYPE) == N_SECT) && ((symbol->n_desc & N_WEAK_DEF) != 0) ) 
		return true;
	// if an undefine and not referencing a weak symbol ==> coalesced
	if ( ((symbol->n_type & N_TYPE) != N_SECT) && ((symbol->n_desc & N_REF_TO_WEAK) != 0) )
		return true;
	
	// regular symbol
	return false;
}


static void __attribute__((noreturn)) throwSymbolNotFound(const char* symbol, const char* referencedFrom, const char* expectedIn)
{
	const char* formatString = "Symbol not found: %s\n  Referenced from: %s\n  Expected in: %s\n";
	char buf[strlen(symbol)+strlen(referencedFrom)+strlen(expectedIn)+strlen(formatString)];
	sprintf(buf, formatString, symbol, referencedFrom, expectedIn);
	throw strdup(buf);  // this is a leak if exception doesn't halt program
}

uintptr_t ImageLoaderMachO::resolveUndefined(const LinkContext& context, const struct macho_nlist* undefinedSymbol, bool twoLevel, ImageLoader** foundIn)
{
	const char* symbolName = &fStrings[undefinedSymbol->n_un.n_strx];

	if ( context.bindFlat || !twoLevel ) {
		// flat lookup
		const Symbol* sym;
		if ( context.flatExportFinder(symbolName, &sym, foundIn) )
			return (*foundIn)->getExportedSymbolAddress(sym);
		// if a bundle is loaded privately the above will not find its exports
		if ( this->isBundle() && this->hasHiddenExports() ) {
			// look in self for needed symbol
			sym = this->findExportedSymbol(symbolName, NULL, false, foundIn);
			if ( sym != NULL )
				return (*foundIn)->getExportedSymbolAddress(sym);
		}
		if ( ((undefinedSymbol->n_type & N_PEXT) != 0) || ((undefinedSymbol->n_type & N_TYPE) == N_SECT) ) {
			// could be a multi-module private_extern internal reference
			// the static linker squirrels away the target address in n_value
			uintptr_t addr = undefinedSymbol->n_value + this->fSlide;
			*foundIn = this;
			return addr;
		}
		if ( (undefinedSymbol->n_desc & N_WEAK_REF) != 0 ) {
			// definition can't be found anywhere
			// if reference is weak_import, then it is ok, just return 0
			return 0;
		}
		throwSymbolNotFound(symbolName, this->getPath(), "flat namespace");
	}
	else {
		// symbol requires searching images with coalesced symbols
		if ( this->needsCoalescing() && symbolRequiresCoalescing(undefinedSymbol) ) {
			const Symbol* sym;
			if ( context.coalescedExportFinder(symbolName, &sym, foundIn) )
				return (*foundIn)->getExportedSymbolAddress(sym);
			//throwSymbolNotFound(symbolName, this->getPath(), "coalesced namespace");
			//fprintf(stderr, "dyld: coalesced symbol %s not found in any coalesced image, falling back to two-level lookup", symbolName);
		}
		
		// two level lookup
		void* hint = NULL;
		ImageLoader* target = NULL;
		uint8_t ord = GET_LIBRARY_ORDINAL(undefinedSymbol->n_desc);
		if ( ord == EXECUTABLE_ORDINAL ) {
			target = context.mainExecutable;
		}
		else if ( ord == SELF_LIBRARY_ORDINAL ) {
			target = this;
		}
		else if ( ord == DYNAMIC_LOOKUP_ORDINAL ) {
			// rnielsen: HACKHACK
			// flat lookup
			const Symbol* sym;
			if ( context.flatExportFinder(symbolName, &sym, foundIn) )
				return (*foundIn)->getExportedSymbolAddress(sym);
			// no image has exports this symbol
			// either report error or hope ZeroLink can just-in-time load an image
			context.undefinedHandler(symbolName);
			// try looking again
			if ( context.flatExportFinder(symbolName, &sym, foundIn) )
				return (*foundIn)->getExportedSymbolAddress(sym);
			
			throwSymbolNotFound(symbolName, this->getPath(), "dynamic lookup");
		}
		else if ( ord <= fLibrariesCount ) {
			DependentLibrary& libInfo = fLibraries[ord-1];
			target = libInfo.image;
			if ( (target == NULL) && (((undefinedSymbol->n_desc & N_WEAK_REF) != 0) || !libInfo.required) ) {
				// if target library not loaded and reference is weak or library is weak return 0
				return 0;
			}
		}
		else {
			throw "corrupt binary, library ordinal too big";
		}
		
		if ( target == NULL ) {
			fprintf(stderr, "resolveUndefined(%s) in %s\n", symbolName, this->getPath());
			throw "symbol not found";
		}
		
		// interpret hint
		if ( fTwoLevelHints != NULL ) {
			uint32_t symIndex = undefinedSymbol - fSymbolTable;
			int32_t undefinedIndex = symIndex - fDynamicInfo->iundefsym;
			if ( (undefinedIndex >= 0) && ((uint32_t)undefinedIndex < fDynamicInfo->nundefsym) ) {
				const struct twolevel_hint* hints = (struct twolevel_hint*)(&fLinkEditBase[fTwoLevelHints->offset]);
				const struct twolevel_hint* theHint = &hints[undefinedIndex];
				hint = (void*)theHint;
			}
		}
		
		const Symbol* sym = target->findExportedSymbol(symbolName, hint, true, foundIn);
		if ( sym!= NULL ) {
			return (*foundIn)->getExportedSymbolAddress(sym);
		}
		else if ( (undefinedSymbol->n_type & N_PEXT) != 0 ) {
			// don't know why the static linker did not eliminate the internal reference to a private extern definition
			*foundIn = this;
			return undefinedSymbol->n_value + fSlide;
		}
		else if ( (undefinedSymbol->n_desc & N_WEAK_REF) != 0 ) {
			// if definition not found and reference is weak return 0
			return 0;
		}
		
		// nowhere to be found
		throwSymbolNotFound(symbolName, this->getPath(), target->getPath());
	}
}

void ImageLoaderMachO::doBindExternalRelocations(const LinkContext& context, bool onlyCoalescedSymbols)
{
	const uintptr_t relocBase = this->getRelocBase();
	const bool twoLevel = this->usesTwoLevelNameSpace();
	const bool prebound = this->isPrebindable();
	
	// if there are __TEXT fixups, temporarily make __TEXT writable
	if ( fTextSegmentWithFixups != NULL ) 
		fTextSegmentWithFixups->tempWritable();

	// cache last lookup
	const struct macho_nlist*	lastUndefinedSymbol = 0;
	uintptr_t					symbolAddr = 0;
	ImageLoader*				image = NULL;
	
	// loop through all external relocation records and bind each
	const relocation_info* const relocsStart = (struct relocation_info*)(&fLinkEditBase[fDynamicInfo->extreloff]);
	const relocation_info* const relocsEnd = &relocsStart[fDynamicInfo->nextrel];
	for (const relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		if (reloc->r_length == RELOC_SIZE) {
			switch(reloc->r_type) {
				case GENERIC_RELOC_VANILLA:
					{
						const struct macho_nlist* undefinedSymbol = &fSymbolTable[reloc->r_symbolnum];
						// if only processing coalesced symbols and this one does not require coalesceing, skip to next
						if ( onlyCoalescedSymbols && !symbolRequiresCoalescing(undefinedSymbol) )
							continue;
						uintptr_t* location = ((uintptr_t*)(reloc->r_address + relocBase));
						uintptr_t value = *location;
					#if __i386__
						if ( reloc->r_pcrel ) {
							value += (uintptr_t)location + 4 - fSlide;
						}
					#endif
						if ( prebound ) {
							// we are doing relocations, so prebinding was not usable
							// in a prebound executable, the n_value field is set to the address where the symbol was found when prebound
							// so, subtracting that gives the initial displacement which we need to add to the newly found symbol address
							// if mach-o relocation structs had an "addend" field this would not be necessary.
							value -= undefinedSymbol->n_value;
						}
						// if undefinedSymbol is same as last time, then symbolAddr and image will resolve to the same too
						if ( undefinedSymbol != lastUndefinedSymbol ) {
							symbolAddr = this->resolveUndefined(context, undefinedSymbol, twoLevel, &image);
							lastUndefinedSymbol = undefinedSymbol;
						}
						if ( context.verboseBind ) {
							const char *path = NULL;
							if(NULL != image) {
								path = image->getShortName();
							}
							if(0 == value) {
								fprintf(stderr, "dyld: bind: %s:0x%08lx = %s:%s, *0x%08lx = 0x%08lx\n",
										this->getShortName(), (uintptr_t)location,
										path, &fStrings[undefinedSymbol->n_un.n_strx], (uintptr_t)location, symbolAddr);
							}
							else {
								fprintf(stderr, "dyld: bind: %s:0x%08lx = %s:%s, *0x%08lx = 0x%08lx + %ld\n",
										this->getShortName(), (uintptr_t)location,
										path, &fStrings[undefinedSymbol->n_un.n_strx], (uintptr_t)location, symbolAddr, value);
							}
						}
						value += symbolAddr;
					#if __i386__
						if ( reloc->r_pcrel ) {
							*location = value - ((uintptr_t)location + 4);
						}
						else {
							*location = value; 
						}
					#else
                         *location = value; 
					#endif
					}
					break;
				default:
					throw "unknown external relocation type";
			}
		}
		else {
			throw "bad external relocation length";
		}
	}
	
	// if there were __TEXT fixups, restore write protection
	if ( fTextSegmentWithFixups != NULL ) {
		fTextSegmentWithFixups->setPermissions();
		sys_icache_invalidate((void*)fTextSegmentWithFixups->getActualLoadAddress(), fTextSegmentWithFixups->getSize());
	}
	
	// update stats
	fgTotalBindFixups += fDynamicInfo->nextrel;
}

const mach_header* ImageLoaderMachO::machHeader() const
{
	return (mach_header*)fMachOData;
}

uintptr_t ImageLoaderMachO::getSlide() const
{
	return fSlide;
}

// hmm. maybe this should be up in ImageLoader??
const void* ImageLoaderMachO::getBaseAddress() const
{
	Segment* seg = fSegments[0];
	return (const void*)seg->getActualLoadAddress();
}


uintptr_t ImageLoaderMachO::doBindLazySymbol(uintptr_t* lazyPointer, const LinkContext& context)
{
	// scan for all non-lazy-pointer sections
	const bool twoLevel = this->usesTwoLevelNameSpace();
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	const uint32_t* const indirectTable = (uint32_t*)&fLinkEditBase[fDynamicInfo->indirectsymoff];
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( type == S_LAZY_SYMBOL_POINTERS ) {
							const uint32_t pointerCount = sect->size / sizeof(uintptr_t);
							uintptr_t* const symbolPointers = (uintptr_t*)(sect->addr + fSlide);
							if ( (lazyPointer >= symbolPointers) && (lazyPointer < &symbolPointers[pointerCount]) ) {
								const uint32_t indirectTableOffset = sect->reserved1;
								const uint32_t lazyIndex = lazyPointer - symbolPointers;
								uint32_t symbolIndex = indirectTable[indirectTableOffset + lazyIndex];
								if ( symbolIndex != INDIRECT_SYMBOL_ABS && symbolIndex != INDIRECT_SYMBOL_LOCAL ) {
									ImageLoader *image = NULL;
									const char *path = NULL;
									uintptr_t symbolAddr = this->resolveUndefined(context,  &fSymbolTable[symbolIndex], twoLevel, &image);
									if ( context.verboseBind ) {
										if(NULL == path && NULL != image) {
											path = image->getShortName();
										}
										fprintf(stderr, "dyld: bind: %s:%s$%s = %s:%s, *0x%08lx = 0x%08lx\n",
												this->getShortName(), &fStrings[fSymbolTable[symbolIndex].n_un.n_strx], "lazy_ptr",
												path, &fStrings[fSymbolTable[symbolIndex].n_un.n_strx], (uintptr_t)&symbolPointers[lazyIndex], symbolAddr);
									}
									if ( NULL != context.bindingHandler ) {
										if(NULL == path && NULL != image) {
											path = image->getPath();
										}
										symbolAddr = (uintptr_t)context.bindingHandler(path, &fStrings[fSymbolTable[symbolIndex].n_un.n_strx], (void *)symbolAddr);
									}
									symbolPointers[lazyIndex] = symbolAddr;
									// update stats
									fgTotalLazyBindFixups++;
									return symbolPointers[lazyIndex];
								}
							}
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	throw "lazy pointer not found";
}



void ImageLoaderMachO::doBindIndirectSymbolPointers(const LinkContext& context, BindingLaziness bindness, bool onlyCoalescedSymbols)
{
	// scan for all non-lazy-pointer sections 
	const bool twoLevel = this->usesTwoLevelNameSpace();
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	const uint32_t* const indirectTable = (uint32_t*)&fLinkEditBase[fDynamicInfo->indirectsymoff];
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						const uint32_t pointerCount = sect->size / sizeof(uintptr_t);
						if ( type == S_NON_LAZY_SYMBOL_POINTERS ) {
							if ( (bindness == kLazyOnly) || (bindness == kLazyOnlyNoDependents) )
								continue;
						}
						else if ( type == S_LAZY_SYMBOL_POINTERS ) {
							// process each symbol pointer in this section
							fgTotalPossibleLazyBindFixups += pointerCount;
							if ( bindness == kNonLazyOnly )
								continue;
						}
						else {
							continue;
						}
						const uint32_t indirectTableOffset = sect->reserved1;
						uintptr_t* const symbolPointers = (uintptr_t*)(sect->addr + fSlide);
						for (uint32_t j=0; j < pointerCount; ++j) {
							uint32_t symbolIndex = indirectTable[indirectTableOffset + j];
							if ( symbolIndex == INDIRECT_SYMBOL_LOCAL) {
								symbolPointers[j] += this->fSlide;
							}
							else if ( symbolIndex == INDIRECT_SYMBOL_ABS) {
								// do nothing since already has absolute address
							}
							else {
								const struct macho_nlist* sym = &fSymbolTable[symbolIndex];
								if ( symbolIndex == 0 ) {
									// This could be rdar://problem/3534709 
									if ( ((const macho_header*)fMachOData)->filetype == MH_EXECUTE ) {
										static bool alreadyWarned = false;
										if ( (sym->n_type & N_TYPE) != N_UNDF ) {
											// The indirect table parallels the (non)lazy pointer sections.  For
											// instance, to find info about the fifth lazy pointer you look at the
											// fifth entry in the indirect table.  (try otool -Iv on a file).
											// The entry in the indirect table contains an index into the symbol table.

											// The bug in ld caused the entry in the indirect table to be zero
											// (instead of a magic value that means a local symbol).  So, if the
											// symbolIndex == 0, we may be encountering the bug, or 0 may be a valid
											// symbol table index. The check I put in place is to see if the zero'th
											// symbol table entry is an import entry (usually it is a local symbol
											// definition).
											if ( context.verboseWarnings && !alreadyWarned ) {
												fprintf(stderr, "dyld: malformed executable '%s', skipping indirect symbol to %s\n",
														this->getPath(), &fStrings[sym->n_un.n_strx]);
												alreadyWarned = true;
											}
											continue;
										}
									}
								}
								ImageLoader *image = NULL;
								// if only processing coalesced symbols and this one does not require coalesceing, skip to next
								if ( onlyCoalescedSymbols && !symbolRequiresCoalescing(sym) )
									continue;
								uintptr_t symbolAddr;
									symbolAddr = resolveUndefined(context, sym, twoLevel, &image);
								if ( context.verboseBind ) {
									const char *path = NULL;
									if(NULL != image) {
										path = image->getShortName();
									}
									const char *typeName;
									if ( type == S_LAZY_SYMBOL_POINTERS ) {
										typeName = "lazy_ptr";
									}
									else {
										typeName = "non_lazy_ptr";
									}
									fprintf(stderr, "dyld: bind: %s:%s$%s = %s:%s, *0x%08lx = 0x%08lx\n",
											this->getShortName(), &fStrings[sym->n_un.n_strx], typeName,
											path, &fStrings[sym->n_un.n_strx], (uintptr_t)&symbolPointers[j], symbolAddr);
								}
								symbolPointers[j] = symbolAddr;
							}
						}
						// update stats
						fgTotalBindFixups += pointerCount;
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

/*
 * The address of these symbols are written in to the (__DATA,__dyld) section
 * at the following offsets:
 *	at offset 0	stub_binding_helper_interface
 *	at offset 4	_dyld_func_lookup
 *	at offset 8	start_debug_thread
 * The 'C' types (if any) for these symbols are ignored here and all are
 * declared as longs so the assignment of their address in to the section will
 * not require a cast.  stub_binding_helper_interface is really a label in the
 * assembly code interface for the stub binding.  It does not have a meaningful 
 * 'C' type.  _dyld_func_lookup is the routine in dyld_libfuncs.c.
 * start_debug_thread is the routine in debug.c.
 *
 * For ppc the image's stub_binding_binding_helper is read from:
 *	at offset 20	the image's stub_binding_binding_helper address
 * and saved into to the image structure.
 */
struct DATAdyld {
	void*   dyldLazyBinder;		// filled in at launch by dyld to point into dyld to &stub_binding_helper_interface
	void*   dyldFuncLookup;		// filled in at launch by dyld to point into dyld to &_dyld_func_lookup
	void*   startDebugThread;   // debugger interface ???
	void*   debugPort;			// debugger interface ???
	void*   debugThread;		// debugger interface ???
	void*   stubBindHelper;		// filled in at static link time to point to stub helper in image
	void*   coreDebug;			// ???
};

// These are defined in dyldStartup.s
extern "C" void stub_binding_helper();
extern "C" bool dyld_func_lookup(const char* name, uintptr_t* address);


void ImageLoaderMachO::setupLazyPointerHandler()
{
	if ( fDATAdyld != NULL ) {
		struct DATAdyld* dd = (struct DATAdyld*)(fDATAdyld->addr + fSlide);
		if ( fDATAdyld->size > offsetof(DATAdyld, dyldLazyBinder) ) {
			if ( dd->dyldLazyBinder != (void*)&stub_binding_helper )
				dd->dyldLazyBinder = (void*)&stub_binding_helper;
		}
		if ( fDATAdyld->size > offsetof(DATAdyld, dyldFuncLookup) ) {
			if ( dd->dyldFuncLookup != (void*)&dyld_func_lookup )
				dd->dyldFuncLookup = (void*)&dyld_func_lookup;
		}
		//if ( fDATAdyld->size > offsetof(DATAdyld, startDebugThread) ) 
		//	dd->startDebugThread = &start_debug_thread;
#ifdef __ppc__
		//if ( fDATAdyld->size > offsetof(DATAdyld, stubBindHelper) )
		//	save = dd->stubBindHelper;	
#endif
	}
}

bool ImageLoaderMachO::usablePrebinding(const LinkContext& context) const
{
	// if prebound and loaded at prebound address, and all libraries are same as when this was prebound, then no need to bind
	if ( this->isPrebindable() && this->allDependentLibrariesAsWhenPreBound() && (this->getSlide() == 0) ) {
		// allow environment variables to disable prebinding
		if ( context.bindFlat )
			return false;
		switch ( context.prebindUsage ) {
			case kUseAllPrebinding:
				return true;
			case kUseSplitSegPrebinding:
				return this->fIsSplitSeg;
			case kUseAllButAppPredbinding:
				return (this != context.mainExecutable);
			case kUseNoPrebinding:
				return false;
		}
	}
	return false;
}

void ImageLoaderMachO::doBind(const LinkContext& context, BindingLaziness bindness)
{
	// set dyld entry points in image
	this->setupLazyPointerHandler();

	// if prebound and loaded at prebound address, and all libraries are same as when this was prebound, then no need to bind
	// note: flat-namespace binaries need to be imports rebound (even if correctly prebound)
	if ( this->usablePrebinding(context) && this->usesTwoLevelNameSpace() ) {
		// if image has coalesced symbols, then these need to be rebound
		if ( this->needsCoalescing() ) {
			this->doBindExternalRelocations(context, true);
			this->doBindIndirectSymbolPointers(context, kLazyAndNonLazy, true);
		}
		// skip binding because prebound and prebinding not disabled
		return;
	}
	
 	// values bound by name are stored two different ways in mach-o
	switch (bindness) {
		case kNonLazyOnly:
		case kLazyAndNonLazy:
			// external relocations are used for data initialized to external symbols
			this->doBindExternalRelocations(context, false);
			break;
		case kLazyOnly:
		case kLazyOnlyNoDependents:
			break;
	}
	// "indirect symbols" are used for code references to external symbols
	this->doBindIndirectSymbolPointers(context, bindness, false);
}



void ImageLoaderMachO::doImageInit(const LinkContext& context)
{
	if ( fDashInit != NULL ) {
		Initializer func = (Initializer)(fDashInit->init_address + fSlide);
		if ( context.verboseInit )
			fprintf(stderr, "dyld: calling -init function 0x%p in %s\n", func, this->getPath());
		func(context.argc, context.argv, context.envp, context.apple);
	}
}

void ImageLoaderMachO::doModInitFunctions(const LinkContext& context)
{
	if ( fModInitSection != NULL ) {
		Initializer* inits = (Initializer*)(fModInitSection->addr + fSlide);
		const uint32_t count = fModInitSection->size / sizeof(uintptr_t);
		for (uint32_t i=0; i < count; ++i) {
			Initializer func = inits[i];
			if ( context.verboseInit )
				fprintf(stderr, "dyld: calling initializer function %p in %s\n", func, this->getPath());
			func(context.argc, context.argv, context.envp, context.apple);
		}
	}
}


void ImageLoaderMachO::doInitialization(const LinkContext& context)
{
	// mach-o has -init and static initializers
	doImageInit(context);
	doModInitFunctions(context);
}

bool ImageLoaderMachO::needsInitialization()
{
	return ( (fDashInit != NULL) || (fModInitSection != NULL) );
}


bool ImageLoaderMachO::needsTermination()
{
	return ( fModTermSection != NULL );
}

bool ImageLoaderMachO::hasImageNotification()
{
	return ( fImageNotifySection != NULL );
}


void ImageLoaderMachO::doTermination(const LinkContext& context)
{
	if ( fModTermSection != NULL ) {
		Terminator* terms = (Terminator*)(fModTermSection->addr + fSlide);
		const uint32_t count = fModTermSection->size / sizeof(uintptr_t);
		for (uint32_t i=count; i > 0; --i) {
			Terminator func = terms[i-1];
			if ( context.verboseInit )
				fprintf(stderr, "dyld: calling terminaton function %p in %s\n", func, this->getPath());
			func();
		}
	}
}

void ImageLoaderMachO::doNotification(enum dyld_image_mode mode, uint32_t infoCount, const struct dyld_image_info info[])
{
	if ( fImageNotifySection != NULL ) {
		dyld_image_notifier* notes = (dyld_image_notifier*)(fImageNotifySection->addr + fSlide);
		const uint32_t count = fImageNotifySection->size / sizeof(uintptr_t);
		for (uint32_t i=count; i > 0; --i) {
			dyld_image_notifier func = notes[i-1];
			func(mode, infoCount, info);
		}
	}
}

void ImageLoaderMachO::printStatistics(unsigned int imageCount)
{
	ImageLoader::printStatistics(imageCount);
	fprintf(stderr, "total hinted binary tree searches:    %d\n", fgHintedBinaryTreeSearchs);
	fprintf(stderr, "total unhinted binary tree searches:  %d\n", fgUnhintedBinaryTreeSearchs);
	
#if LINKEDIT_USAGE_DEBUG
	fprintf(stderr, "linkedit pages accessed (%lu):\n", sLinkEditPageBuckets.size());
#endif	
}

void ImageLoaderMachO::doPrebinding(const LinkContext& context, time_t timestamp, uint8_t* fileToPrebind)
{
	// update __DATA segment
	this->applyPrebindingToDATA(fileToPrebind);
	
	// update load commands 
	this->applyPrebindingToLoadCommands(context, fileToPrebind, timestamp);
	
	// update symbol table  
	this->applyPrebindingToLinkEdit(context, fileToPrebind);
}

void ImageLoaderMachO::applyPrebindingToDATA(uint8_t* fileToPrebind)
{
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i) {
		SegmentMachO* seg = (SegmentMachO*)fSegments[i];
		if ( seg->writeable() ) {
			memcpy(&fileToPrebind[seg->fFileOffset], (void*)seg->getActualLoadAddress(), seg->fFileSize);
		}
	}
}

void ImageLoaderMachO::applyPrebindingToLoadCommands(const LinkContext& context, uint8_t* fileToPrebind, time_t timestamp)
{
	macho_header* mh = (macho_header*)fileToPrebind;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fileToPrebind[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
				{
					// update each dylib load command with the timestamp of the target dylib
					struct dylib_command* dylib  = (struct dylib_command*)cmd;
					const char* name = (char*)cmd + dylib->dylib.name.offset;
					for (const DependentLibrary* dl=fLibraries; dl < &fLibraries[fLibrariesCount]; dl++) {
						if (strcmp(dl->name, name) == 0 ) {
							// found matching DependentLibrary for this load command
							ImageLoaderMachO* targetImage = (ImageLoaderMachO*)(dl->image); // !!! assume only mach-o images are prebound
							if ( ! targetImage->isPrebindable() )
								throw "dependent dylib is not prebound";
							// if the target is currently being re-prebound then its timestamp will be the same as this one
							if ( ! targetImage->usablePrebinding(context) ) {
								dylib->dylib.timestamp = timestamp;
							}
							else {
								// otherwise dependent library is already correctly prebound, so use its checksum
								dylib->dylib.timestamp = targetImage->doGetLibraryInfo().checksum;
							}
							break;
						}
					}
				}
				break;
			case LC_ID_DYLIB:
				{
					// update the ID of this library with the new timestamp
					struct dylib_command* dylib  = (struct dylib_command*)cmd;
					dylib->dylib.timestamp = timestamp;
				}
				break;
			case LC_SEGMENT_COMMAND:
				// if dylib was rebased, update segment commands
				if ( fSlide != 0 ) {
					struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					seg->vmaddr += fSlide;
					struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						sect->addr += fSlide;
					}
				}
				break;
			case LC_ROUTINES_COMMAND:
				// if dylib was rebased, update -init command
				if ( fSlide != 0 ) {
					struct macho_routines_command* routines = (struct macho_routines_command*)cmd;
					routines->init_address += fSlide;
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}	
}

void ImageLoaderMachO::applyPrebindingToLinkEdit(const LinkContext& context, uint8_t* fileToPrebind)
{
	// In prebound images, the n_value of the symbol table entry for is the prebound address
	// This is needed when prebinding can't be used, to back solve for any possible addend in non-lazy pointers
	const char* stringPool = NULL;
	struct macho_nlist* symbolTable = NULL;
	const struct dysymtab_command* dysymtab = NULL;
	
	// get symbol table info
	macho_header* mh = (macho_header*)fileToPrebind;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fileToPrebind[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SYMTAB:
				{
					const struct symtab_command* symtab = (struct symtab_command*)cmd;
					stringPool = (const char*)&fileToPrebind[symtab->stroff];
					symbolTable = (struct macho_nlist*)(&fileToPrebind[symtab->symoff]);
				}
				break;
			case LC_DYSYMTAB:
				dysymtab = (struct dysymtab_command*)cmd;
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}	

	// walk all imports and re-resolve their n_value (needed incase prebinding is invalid)
	struct macho_nlist* lastImport = &symbolTable[dysymtab->iundefsym+dysymtab->nundefsym];
	for (struct macho_nlist* entry = &symbolTable[dysymtab->iundefsym]; entry < lastImport; ++entry) {
		ImageLoader* dummy;
		entry->n_value = this->resolveUndefined(context, entry, this->usesTwoLevelNameSpace(), &dummy);
	}
	
	// walk all exports and slide their n_value
	struct macho_nlist* lastExport = &symbolTable[dysymtab->iextdefsym+dysymtab->nextdefsym];
	for (struct macho_nlist* entry = &symbolTable[dysymtab->iextdefsym]; entry < lastExport; ++entry) {
		if ( (entry->n_type & N_TYPE) == N_SECT )
			entry->n_value += fSlide;
	}

	// walk all local symbols and slide their n_value
	struct macho_nlist* lastLocal = &symbolTable[dysymtab->ilocalsym+dysymtab->nlocalsym];
	for (struct macho_nlist* entry = &symbolTable[dysymtab->ilocalsym]; entry < lastLocal; ++entry) {
		if ( entry->n_sect != NO_SECT )
			entry->n_value += fSlide;
	}
	
	// walk all local relocations and reset every PPC_RELOC_PB_LA_PTR r_value
	relocation_info* const relocsStart = (struct relocation_info*)(&fileToPrebind[dysymtab->locreloff]);
	relocation_info* const relocsEnd = &relocsStart[dysymtab->nlocrel];
	for (relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		if ( (reloc->r_address & R_SCATTERED) != 0 ) {
			struct scattered_relocation_info* sreloc = (struct scattered_relocation_info*)reloc;
			if (sreloc->r_length == RELOC_SIZE) {
				switch(sreloc->r_type) {
		#if __ppc__ || __ppc64__
					case PPC_RELOC_PB_LA_PTR:
		#elif __i386__
					case GENERIC_RELOC_PB_LA_PTR:
		#else
			#error unknown architecture
		#endif
						sreloc->r_value += fSlide;
						break;
				}
			}
		}
	}
	
	// if multi-module, fix up objc_addr (10.4 and later runtime does not use this, but we want to keep file checksum consistent)
	if ( dysymtab->nmodtab != 0 ) {
		dylib_module* const modulesStart = (struct dylib_module*)(&fileToPrebind[dysymtab->modtaboff]);
		dylib_module* const modulesEnd = &modulesStart[dysymtab->nmodtab];
		for (dylib_module* module=modulesStart; module < modulesEnd; ++module) {
			if ( module->objc_module_info_size != 0 ) {
				module->objc_module_info_addr += fSlide;
			}
		}
	}
}

// file on disk has been reprebound, but we are still mapped to old file
void ImageLoaderMachO::prebindUnmap(const LinkContext& context)
{
	// this removes all mappings to the old file, so the kernel will unlink (delete) it.
	//  We need to leave the load commands and __LINKEDIT in place
	for (std::vector<class Segment*>::iterator it=fSegments.begin(); it != fSegments.end(); ++it) {
		void* segmentAddress = (void*)((*it)->getActualLoadAddress());
		uintptr_t segmentSize = (*it)->getSize();
		//fprintf(stderr, "unmapping segment %s at %p for %s\n", (*it)->getName(), segmentAddress, this->getPath());
		// save load commands at beginning of __TEXT segment
		if ( segmentAddress == fMachOData ) {
			// typically load commands are one or two pages in size, so ok to alloc on stack
			uint32_t loadCmdSize = sizeof(macho_header) + ((macho_header*)fMachOData)->sizeofcmds;
			uint32_t loadCmdPages = (loadCmdSize+4095) & (-4096);
			uint8_t loadcommands[loadCmdPages];
			memcpy(loadcommands, fMachOData, loadCmdPages);
			// unmap whole __TEXT segment
			munmap((void*)(fMachOData), segmentSize);
			// allocate and copy back mach_header and load commands
			vm_address_t addr = (vm_address_t)fMachOData;
			int r2 = vm_allocate(mach_task_self(), &addr, loadCmdPages, false /*at this address*/);
			if ( r2 != 0 )
				fprintf(stderr, "prebindUnmap() vm_allocate for __TEXT %d failed\n", loadCmdPages);
			memcpy((void*)fMachOData, loadcommands, loadCmdPages);
			//fprintf(stderr, "copying back load commands to %p size=%u for %s\n", segmentAddress, loadCmdPages, this->getPath());
		}
		else if ( strcmp((*it)->getName(), "__LINKEDIT") == 0 ) {
			uint32_t linkEditSize = segmentSize;
			uint32_t linkEditPages = (linkEditSize+4095) & (-4096);
			void* linkEditTmp = malloc(linkEditPages);
			memcpy(linkEditTmp, segmentAddress, linkEditPages);
			// unmap whole __LINKEDIT segment
			munmap(segmentAddress, segmentSize);
			vm_address_t addr = (vm_address_t)segmentAddress;
			int r2 = vm_allocate(mach_task_self(), &addr, linkEditPages, false /*at this address*/);
			if ( r2 != 0 )
				fprintf(stderr, "prebindUnmap() vm_allocate for __LINKEDIT %d failed\n", linkEditPages);
			memcpy(segmentAddress, linkEditTmp, linkEditPages);
			//fprintf(stderr, "copying back __LINKEDIT to %p size=%u for %s\n", segmentAddress, linkEditPages, this->getPath());
			free(linkEditTmp);
		}
		else {
			// unmap any other segment
			munmap((void*)(segmentAddress), (*it)->getSize());
		}
	}
}



SegmentMachO::SegmentMachO(const struct macho_segment_command* cmd, ImageLoaderMachO* image, const uint8_t* fileData)
 : fImage(image), fSize(cmd->vmsize), fFileSize(cmd->filesize), fFileOffset(cmd->fileoff), fPreferredLoadAddress(cmd->vmaddr), 
	fVMProtection(cmd->initprot), fHasFixUps(false), fUnMapOnDestruction(false)
{
	strncpy(fName, cmd->segname, 16);
	fName[16] = '\0';
	// scan sections for fix-up bit
	const struct macho_section* const sectionsStart = (struct macho_section*)((char*)cmd + sizeof(struct macho_segment_command));
	const struct macho_section* const sectionsEnd = &sectionsStart[cmd->nsects];
	for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
		if ( (sect->flags & (S_ATTR_EXT_RELOC | S_ATTR_LOC_RELOC)) != 0 )
			fHasFixUps = true;
	}
}

SegmentMachO::~SegmentMachO()
{
	if ( fUnMapOnDestruction ) {
		//fprintf(stderr, "unmapping segment %s at 0x%08lX\n", getName(), getActualLoadAddress());
		munmap((void*)(this->getActualLoadAddress()), this->getSize());
	}
}

const ImageLoader* SegmentMachO::getImage()
{
	return fImage;
}

const char* SegmentMachO::getName()
{
	return fName;
}

uintptr_t SegmentMachO::getSize()
{
	return fSize;
}

uintptr_t SegmentMachO::getFileSize()
{
	return fFileSize;
}

uintptr_t SegmentMachO::getFileOffset()
{
	return fFileOffset;
}

bool SegmentMachO::readable()
{
	return ( (fVMProtection & VM_PROT_READ) != 0);
}

bool SegmentMachO::writeable()
{
	return ((fVMProtection & VM_PROT_WRITE) != 0);
}

bool SegmentMachO::executable()
{
	return ((fVMProtection & VM_PROT_EXECUTE) != 0);
}

bool SegmentMachO::unaccessible()
{
	return (fVMProtection == 0);
}

bool SegmentMachO::hasFixUps()
{
	return fHasFixUps;
}

uintptr_t SegmentMachO::getActualLoadAddress()
{
	return fPreferredLoadAddress + fImage->fSlide;
}

uintptr_t SegmentMachO::getPreferredLoadAddress()
{
	return fPreferredLoadAddress;
}

bool SegmentMachO::hasPreferredLoadAddress()
{
	return (fPreferredLoadAddress != 0);
}

void SegmentMachO::setUnMapWhenDestructed(bool unmap)
{
	fUnMapOnDestruction = unmap;
}

static uint32_t *buildCRCTable(void)
{
	uint32_t *table = new uint32_t[256];
	uint32_t p = 0xedb88320UL;  // standard CRC-32 polynomial

	for (unsigned int i = 0; i < 256; i++) {
		uint32_t c = i;
		for (unsigned int j = 0; j < 8; j++) {
			if ( c & 1 ) c = p ^ (c >> 1);
			else c = c >> 1;
		}
		table[i] = c;
	}

	return table;
}

uint32_t SegmentMachO::crc32()
{
	if ( !readable() ) return 0;

	static uint32_t *crcTable = NULL;
	if ( !crcTable ) crcTable = buildCRCTable();
	
	uint32_t crc = ~(uint32_t)0;
	uint8_t *p = (uint8_t *)getActualLoadAddress(); 
	uint8_t *end = p + getSize();
	while ( p < end ) {
		crc = crcTable[(crc & 0xff) ^ (*p++)] ^ (crc >> 8);
	}
	return crc ^ ~(uint32_t)0;
}





