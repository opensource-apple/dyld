/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2007 Apple Inc. All rights reserved.
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

// work around until conformance work is complete rdar://problem/4508801
#define __srr0	srr0 
#define __eip	eip 
#define __rip	rip 


#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/stat.h> 
#include <sys/mman.h>
#include <mach/shared_memory_server.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#include <mach-o/loader.h> 
#include <mach-o/reloc.h> 
#include <mach-o/nlist.h> 
#include <sys/sysctl.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSCacheControl.h>
#if __ppc__ || __ppc64__
	#include <mach-o/ppc/reloc.h>
#endif
#if __x86_64__
	#include <mach-o/x86_64/reloc.h>
#endif

#ifndef MH_PIE
	#define MH_PIE 0x200000 
#endif

#ifndef S_DTRACE_DOF
  #define S_DTRACE_DOF 0xF
#endif

#ifndef S_ATTR_SELF_MODIFYING_CODE
  #define S_ATTR_SELF_MODIFYING_CODE 0x04000000
#endif

#include "ImageLoaderMachO.h"
#include "mach-o/dyld_images.h"

// optimize strcmp for ppc
#if __ppc__
	#include <ppc_intrinsics.h>
#else
	#define astrcmp(a,b) strcmp(a,b)
#endif

// in libc.a
extern "C" void _spin_lock(uint32_t*);
extern "C" void _spin_unlock(uint32_t*);


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

#if __x86_64__
	#define POINTER_RELOC X86_64_RELOC_UNSIGNED
#else
	#define POINTER_RELOC GENERIC_RELOC_VANILLA
#endif

uint32_t ImageLoaderMachO::fgHintedBinaryTreeSearchs = 0;
uint32_t ImageLoaderMachO::fgUnhintedBinaryTreeSearchs = 0;
uint32_t ImageLoaderMachO::fgCountOfImagesWithWeakExports = 0;

#if __i386__
uint32_t ImageLoaderMachO::fgReadOnlyImportSpinLock = 0;
#endif

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
		if ( sLinkEditPageBuckets.count(page) == 0 ) {
			ImageLoader* inImage = dyld::findImageContainingAddress(addr);
			dyld::log("dyld: accessing page 0x%016lX in __LINKEDIT of %s\n", page, inImage != NULL ? inImage->getPath() : "unknown" );
		}
		sLinkEditPageBuckets.insert(page);
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
	fTwoLevelHints	= NULL;
	fDylibID		= NULL;
#if TEXT_RELOC_SUPPORT
	fTextSegmentWithFixups = NULL;
#endif
#if __i386__
	fReadOnlyImportSegment = NULL;
#endif
	fIsSplitSeg		= false;
	fInSharedCache	= false;
#if __ppc64__
	f4GBWritable	= false;
#endif
	fHasSubLibraries= false;
	fHasSubUmbrella = false;
	fInUmbrella     = false;
	fHasDOFSections = false;
	fHasDashInit	= false;
	fHasInitializers= false;
	fHasTerminators = false;
#if IMAGE_NOTIFY_SUPPORT
	fHasImageNotifySection = false;
#endif
}

// create image for main executable
ImageLoaderMachO::ImageLoaderMachO(const struct mach_header* mh, uintptr_t slide, const char* path, const LinkContext& context)
 : ImageLoader(path)
{
	// clean slate
	this->init();

	// temporary use this buffer until TEXT is mapped in
	fMachOData = (const uint8_t*)mh;

	// create segments
	this->instantiateSegments((const uint8_t*)mh);
		
	// set slide for PIE programs
	this->setSlide(slide);

	// get pointers to interesting things 
	this->parseLoadCmds();

	// update segments to reference load commands in mapped in __TEXT segment
	this->adjustSegments();

#if __i386__
	// kernel may have mapped in __IMPORT segment read-only, we need it read/write to do binding
	if ( fReadOnlyImportSegment != NULL )
		fReadOnlyImportSegment->tempWritable(context, this);
#endif
	
	// for PIE record end of program, to know where to start loading dylibs
	if ( mh->flags & MH_PIE )
		Segment::fgNextPIEDylibAddress = (uintptr_t)this->getEnd();
	
	// notify state change
	this->setMapped(context);
	
	if ( context.verboseMapping ) {
		dyld::log("dyld: Main executable mapped %s\n", this->getPath());
		for (ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
			Segment* seg = *it;
			if ( (strcmp(seg->getName(), "__PAGEZERO") == 0) || (strcmp(seg->getName(), "__UNIXSTACK") == 0)  )
				dyld::log("%18s at 0x%08lX->0x%08lX\n", seg->getName(), seg->getPreferredLoadAddress(), seg->getPreferredLoadAddress()+seg->getSize());
			else
				dyld::log("%18s at 0x%08lX->0x%08lX\n", seg->getName(), seg->getActualLoadAddress(this), seg->getActualLoadAddress(this)+seg->getSize());
		}
	}
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
	if ( mh->filetype == MH_EXECUTE ) {
		throw "can't load another MH_EXECUTE";
	}
	else {
		ImageLoader::mapSegments((const void*)mh, len, context);
	}
	
	// for compatibility, never unload dylibs loaded from memory
	this->setNeverUnload();

	// get pointers to interesting things 
	this->parseLoadCmds();

	// update segments to reference load commands in mapped in __TEXT segment
	this->adjustSegments();

	// bundle loads need path copied
	if ( moduleName != NULL ) 
		this->setPath(moduleName);
	
	// notify state change
	this->setMapped(context);
		
}

// create image by using cached mach-o file
ImageLoaderMachO::ImageLoaderMachO(const struct mach_header* mh, const char* path, const struct stat& info, const LinkContext& context)
 : ImageLoader(path, 0, info)
{
	// clean slate
	this->init();

	// already mapped to mh address
	fMachOData = (const uint8_t*)mh;

	// usually a split seg
	fIsSplitSeg = ((mh->flags & MH_SPLIT_SEGS) != 0);	
	
	// remember this is from shared cache and cannot be unloaded
	fInSharedCache = true;
	this->setNeverUnload();

	// create segments
	this->instantiateSegments((const uint8_t*)mh);
		
	// segments already mapped in cache
	if ( context.verboseMapping ) {
		dyld::log("dyld: Using shared cached for %s\n", path);
		for (ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
			Segment* seg = *it;
			dyld::log("%18s at 0x%08lX->0x%08lX\n", seg->getName(), seg->getActualLoadAddress(this), seg->getActualLoadAddress(this)+seg->getSize());
		}
	}

	// get pointers to interesting things 
	this->parseLoadCmds();

	// note: path is mapped into cache so no need for ImageLoader to make a copy

	// notify state change
	this->setMapped(context);
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
	
	// update segments to reference load commands in mapped in __TEXT segment
	this->adjustSegments();
	
	// notify state change
	this->setMapped(context);
	
	// if path happens to be same as in LC_DYLIB_ID load command use that, otherwise malloc a copy of the path
	const char* installName = getInstallPath();
	if ( (installName != NULL) && (strcmp(installName, path) == 0) && (path[0] == '/') )
		this->setPathUnowned(installName);
	if ( path[0] != '/' ) {
		// rdar://problem/5135363 turn relative paths into absolute paths so gdb, Symbolication can later find them
		char realPath[MAXPATHLEN];
		if ( realpath(path, realPath) != NULL )
			this->setPath(realPath);
		else
			this->setPath(path);
	}
	else 
		this->setPath(path);
	
	// tell kernel about pages we are going to need soon
	if ( ! context.preFetchDisabled )
		this->preFetch(fd, offsetInFat, context);

}



ImageLoaderMachO::~ImageLoaderMachO()
{
	// keep count of images with weak exports
	if ( this->hasCoalescedExports() )
		--fgCountOfImagesWithWeakExports;

	// keep count of images used in shared cache
	if ( fInSharedCache )
		--fgImagesUsedFromSharedCache;

	// usually unmap image when done
	if ( ! this->leaveMapped() && (this->getState() >= dyld_image_state_mapped) ) {
		// first segment has load commands, so unmap last
		Segment* firstSeg = *(this->beginSegments());
		for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
			Segment* seg = *it;
			if ( seg != firstSeg )
				seg->unmap(this);
		}
		firstSeg->unmap(this);
	}
	// free segment objects
	free(fSegmentsArray);
}



void ImageLoaderMachO::instantiateSegments(const uint8_t* fileData)
{
	const uint32_t cmd_count = ((macho_header*)fileData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fileData[sizeof(macho_header)];

	// count LC_SEGMENT cmd and reserve that many segment slots
	uint32_t segCount = 0;
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			// ignore zero-sized segments
			if ( ((struct macho_segment_command*)cmd)->vmsize != 0 )
				++segCount;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	// fSegmentsArrayCount is only 8-bits
	if ( segCount > 255 )
		dyld::throwf("more than 255 segments in %s", this->getPath());
		
	// allocate array of segment objects in one call to malloc()
	//fSegmentsArray = static_cast<SegmentMachO*>(operator new[](segCount*sizeof(SegmentMachO)));
	fSegmentsArray = static_cast<SegmentMachO*>(malloc(segCount*sizeof(SegmentMachO)));
	fSegmentsArrayCount = segCount;
	
	// construct Segment object for each LC_SEGMENT cmd using "placment new"
	uint32_t segIndex = 0;
	cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* segCmd = (struct macho_segment_command*)cmd;
			// ignore zero-sized segments
			if ( segCmd->vmsize != 0 ) 
				new (&fSegmentsArray[segIndex++]) SegmentMachO(segCmd);
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}


void ImageLoaderMachO::adjustSegments()
{
	// tell each segment where is load command is finally mapped
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	uint32_t segIndex = 0;
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* segCmd = (struct macho_segment_command*)cmd;
			// ignore zero-sized segments
			if ( segCmd->vmsize != 0 ) {
				fSegmentsArray[segIndex].adjust(segCmd);
				++segIndex;
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

void ImageLoaderMachO::preFetch(int fd, uint64_t offsetInFat, const LinkContext& context)
{
	// always prefetch a subrange of __LINKEDIT pages
	uintptr_t symbolTableOffset = (uintptr_t)fSymbolTable - (uintptr_t)fLinkEditBase;
	uintptr_t stringTableOffset = (uintptr_t)fStrings - (uintptr_t)fLinkEditBase;
	uintptr_t start;
	// if image did not load at preferred address
	if ( fSegmentsArray[0].getPreferredLoadAddress() != (uintptr_t)fMachOData ) {
		// local relocations will be processed, so start pre-fetch at local symbols
		start = offsetInFat + fDynamicInfo->locreloff;
	}
	else {
		// otherwise start pre-fetch at global symbols section of symbol table
		start = offsetInFat + symbolTableOffset + fDynamicInfo->iextdefsym * sizeof(macho_nlist);
	}
	// prefetch ends at end of last undefined string in string pool
	uintptr_t end = offsetInFat + stringTableOffset;
	if ( fDynamicInfo->nundefsym != 0 )
		end += fSymbolTable[fDynamicInfo->iundefsym+fDynamicInfo->nundefsym-1].n_un.n_strx;
	else if ( fDynamicInfo->nextdefsym != 0 )
		end += fSymbolTable[fDynamicInfo->iextdefsym+fDynamicInfo->nextdefsym-1].n_un.n_strx;
	
	radvisory advice;
	advice.ra_offset = start & (-4096); // page align
	advice.ra_count = (end-advice.ra_offset+4095) & (-4096);
	fgTotalBytesPreFetched += advice.ra_count;
	fcntl(fd, F_RDADVISE, &advice);
	if ( context.verboseMapping ) {
		dyld::log("%18s prefetching 0x%0llX -> 0x%0llX\n", 
			"__LINKEDIT", advice.ra_offset+(uintptr_t)fLinkEditBase-offsetInFat, advice.ra_offset+advice.ra_count+(uintptr_t)fLinkEditBase-offsetInFat);
	}
	
	// prefetch __DATA/__OBJC pages during launch, but not for dynamically loaded code
	if ( context.linkingMainExecutable ) {
		for (ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
			Segment* seg = *it;
			if ( seg->writeable() && (seg->getFileSize() > 0) ) {
				// prefetch writable segment that have mmap'ed regions
				advice.ra_offset = offsetInFat + seg->getFileOffset();
				advice.ra_count = seg->getFileSize();
				// limit prefetch to 1MB (256 pages)
				if ( advice.ra_count > 1024*1024 )
					advice.ra_count = 1024*1024;
				fgTotalBytesPreFetched += advice.ra_count;
				fcntl(fd, F_RDADVISE, &advice);
				if ( context.verboseMapping ) {
					dyld::log("%18s prefetching 0x%0lX -> 0x%0lX\n", 
						seg->getName(), seg->getActualLoadAddress(this), seg->getActualLoadAddress(this)+advice.ra_count-1);
				}
			}
		}
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






// hack until kernel headers and glue are in system
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
		
#if SPLIT_SEG_SHARED_REGION_SUPPORT	
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
	//dyld::log("%s(%i, %u, %8p, %8p)\n", __func__, fd, regionCount, regions, slide);
	//for ( unsigned int i=0; i < regionCount; ++i) {
	//	dyld::log("\taddress=0x%08llX, size=0x%08llX\n", regions[i].address, regions[i].size);
	//}
	int r = syscall(299, fd, regionCount, regions, slide);
// 	if(0 != r)
// 		dyld::log("%s(%i, %u, %8p, %8p) errno=%i (%s)\n", __func__, fd, regionCount, regions, slide, errno, strerror(errno));
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
 	//dyld::log("%s(%u, %8p)\n", __func__, rangeCount, ranges);
	int r = syscall(300, rangeCount, ranges);
// 	if(0 != r)
// 		dyld::log("%s(%u, %8p) errno=%i (%s)\n", __func__, rangeCount, ranges, errno, strerror(errno));
    return r;
}
#define KERN_SHREG_PRIVATIZABLE	54


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
			//dyld::log("mmap(%p, 0x%08lX, block=0x%08X, %s\n", mmapAddress, size, biggestDiff, fPath);
			mmapAddress = mmap(mmapAddress, size, protection, MAP_FIXED | MAP_PRIVATE, fd, offset);
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

#endif // SPLIT_SEG_SHARED_REGION_SUPPORT	


#if SPLIT_SEG_DYLIB_SUPPORT	
unsigned int
ImageLoaderMachO::getExtraZeroFillEntriesCount()
{
	// calculate mapping entries
	unsigned int extraZeroFillEntries = 0;
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		if ( seg->hasTrailingZeroFill() )
			++extraZeroFillEntries;
	}
	
	return extraZeroFillEntries;
}

void
ImageLoaderMachO::initMappingTable(uint64_t offsetInFat,
								   _shared_region_mapping_np *mappingTable)
{
	unsigned int segmentCount = fSegmentsArrayCount;
	for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
		Segment* seg = &fSegmentsArray[segIndex];
		_shared_region_mapping_np* entry = &mappingTable[entryIndex];
		entry->address			= seg->getActualLoadAddress(this);
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
ImageLoaderMachO::sharedRegionMapFilePrivateOutside(int fd,
													uint64_t offsetInFat,
													uint64_t lenInFat,
													uint64_t fileLen,
													const LinkContext& context)
{
	static uintptr_t sNextAltLoadAddress 
	#if __ppc_
		= 0xC0000000;
	#else
		= 0;
	#endif

	const unsigned int segmentCount = fSegmentsArrayCount;
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
						sNextAltLoadAddress = 0xB0000000;
					if ( (sNextAltLoadAddress & 0xF0000000) == 0xF0000000 )
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
			if ( ((regions[i].init_prot & VM_PROT_ZF) != 0) || (regions[i].size == 0) ) {
				// nothing to mmap for zero-fills areas, they are just vm_allocated 
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
				//dyld::log("mmap(%p, 0x%08lX, block=0x%08X, %s\n", mmapAddress, size, biggestDiff, fPath);
				mmapAddress = mmap(mmapAddress, size, protection, MAP_FIXED | MAP_PRIVATE, fd, offset);
				if ( mmapAddress == ((void*)(-1)) )
					throw "mmap error";
			}
		}
		// set so next maps right after this one
		sNextAltLoadAddress += biggestDiff; 
		sNextAltLoadAddress = (sNextAltLoadAddress + 4095) & (-4096);
		
		// logging
		if ( context.verboseMapping ) {
			dyld::log("dyld: Mapping split-seg outside shared region, slid by 0x%08lX %s\n", this->fSlide, this->getPath());
			for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
				Segment* seg = &fSegmentsArray[segIndex];
				const _shared_region_mapping_np* entry = &regions[entryIndex];
				if ( (entry->init_prot & VM_PROT_ZF) == 0 ) 
					dyld::log("%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), seg->getActualLoadAddress(this), seg->getActualLoadAddress(this)+seg->getFileSize()-1);
				if ( entryIndex < (regionCount-1) ) {
					const _shared_region_mapping_np* nextEntry = &regions[entryIndex+1];
					if ( (nextEntry->init_prot & VM_PROT_ZF) != 0 ) {
						uint64_t segOffset = nextEntry->address - entry->address;
						dyld::log("%18s at 0x%08lX->0x%08lX (zerofill)\n",
								seg->getName(), (uintptr_t)(seg->getActualLoadAddress(this) + segOffset), (uintptr_t)(seg->getActualLoadAddress(this) + segOffset + nextEntry->size - 1));
						++entryIndex;
					}
				}
			}
		}
		
		return r;
}


void ImageLoaderMachO::mapSegments(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context)
{
	// non-split segment libraries handled by super class
	if ( !fIsSplitSeg )
		return ImageLoader::mapSegments(fd, offsetInFat, lenInFat, fileLen, context);

#if SPLIT_SEG_SHARED_REGION_SUPPORT	
	enum SharedRegionState
	{
		kSharedRegionStartState = 0,
		kSharedRegionMapFileState,
		kSharedRegionMapFilePrivateState,
		kSharedRegionMapFilePrivateMMapState,
		kSharedRegionMapFilePrivateOutsideState,
	};
	static SharedRegionState sSharedRegionState = kSharedRegionStartState;

	if ( kSharedRegionStartState == sSharedRegionState ) {
		if ( hasSharedRegionMapFile() ) {
			if ( context.sharedRegionMode == kUsePrivateSharedRegion ) { 
				sharedRegionMakePrivate(context);
				sSharedRegionState = kSharedRegionMapFilePrivateState;
			}
			else if ( context.sharedRegionMode == kDontUseSharedRegion ) {
				sSharedRegionState = kSharedRegionMapFilePrivateOutsideState;
			}
			else if ( context.sharedRegionMode == kSharedRegionIsSharedCache ) {
				sSharedRegionState = kSharedRegionMapFilePrivateOutsideState;
			}
			else {
				sSharedRegionState = kSharedRegionMapFileState;
			}
		}
		else {
			sSharedRegionState = kSharedRegionMapFilePrivateOutsideState;
		}
	}
	
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
#else
	// support old split-seg dylibs by mapping them where ever we find space
	if ( sharedRegionMapFilePrivateOutside(fd, offsetInFat, lenInFat, fileLen, context) != 0 ) {
		throw "mapping error";
	}
#endif
}
#endif // SPLIT_SEG_DYLIB_SUPPORT


#if SPLIT_SEG_SHARED_REGION_SUPPORT	
int ImageLoaderMachO::sharedRegionMakePrivate(const LinkContext& context)
{
	if ( context.verboseMapping )
		dyld::log("dyld: making shared regions private\n");

	// shared mapping failed, so make private copy of shared region and try mapping private
	MappedRegion allRegions[context.imageCount()*8]; // assume average of less that eight segments per image
	MappedRegion* end = context.getAllMappedRegions(allRegions);
	_shared_region_range_np	splitSegRegions[end-allRegions];
	_shared_region_range_np* sp = splitSegRegions;
	for (MappedRegion* p=allRegions; p < end; ++p) {
		uint8_t highByte = p->address >> 28;
		if ( (highByte == 9) || (highByte == 0xA) ) {
			_shared_region_range_np splitRegion;
			splitRegion.address = p->address;
			splitRegion.size = p->size;
			*sp++ = splitRegion;
		}
	}
	int result = _shared_region_make_private_np(sp-splitSegRegions, splitSegRegions);
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
	const unsigned int segmentCount = fSegmentsArrayCount;
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
		this->setNeverUnload();
		if ( context.verboseMapping ) {
			dyld::log("dyld: Mapping split-seg shared %s\n", this->getPath());
			for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
				Segment* seg = &fSegmentsArray[segIndex];
				const _shared_region_mapping_np* entry = &mappingTable[entryIndex];
				if ( (entry->init_prot & VM_PROT_ZF) == 0 ) 
					dyld::log("%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), seg->getActualLoadAddress(this), seg->getActualLoadAddress(this)+seg->getFileSize()-1);
				if ( entryIndex < (mappingTableCount-1) ) {
					const _shared_region_mapping_np* nextEntry = &mappingTable[entryIndex+1];
					if ( (nextEntry->init_prot & VM_PROT_ZF) != 0 ) {
						uint64_t segOffset = nextEntry->address - entry->address;
						dyld::log("%18s at 0x%08lX->0x%08lX\n",
								seg->getName(), (uintptr_t)(seg->getActualLoadAddress(this) + segOffset), (uintptr_t)(seg->getActualLoadAddress(this) + segOffset + nextEntry->size - 1));
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
	const unsigned int segmentCount = fSegmentsArrayCount;

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
		r = _shared_region_map_file_np(fd, mappingTableCount, mappingTable, &slide);
	if ( 0 == r ) {
		if ( 0 != slide ) {
			slide = (slide) & (-4096); // round down to page boundary
			this->setSlide(slide);
		}
		this->setNeverUnload();
		if ( context.verboseMapping ) {
			if ( slide == 0 )
				dyld::log("dyld: Mapping split-seg un-shared %s\n", this->getPath());
			else
				dyld::log("dyld: Mapping split-seg un-shared slid by 0x%08llX %s\n", slide, this->getPath());
			for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
				Segment* seg = &fSegmentsArray[segIndex];
				const _shared_region_mapping_np* entry = &mappingTable[entryIndex];
				if ( (entry->init_prot & VM_PROT_ZF) == 0 ) 
					dyld::log("%18s at 0x%08lX->0x%08lX\n",
							seg->getName(), seg->getActualLoadAddress(this), seg->getActualLoadAddress(this)+seg->getFileSize()-1);
				if ( entryIndex < (mappingTableCount-1) ) {
					const _shared_region_mapping_np* nextEntry = &mappingTable[entryIndex+1];
					if ( (nextEntry->init_prot & VM_PROT_ZF) != 0 ) {
						uint64_t segOffset = nextEntry->address - entry->address;
						dyld::log("%18s at 0x%08lX->0x%08lX (zerofill)\n",
								seg->getName(), (uintptr_t)(seg->getActualLoadAddress(this) + segOffset), (uintptr_t)(seg->getActualLoadAddress(this) + segOffset + nextEntry->size - 1));
						++entryIndex;
					}
				}
			}
		}
	}
	if ( r != 0 )
		dyld::throwf("can't rebase split-seg dylib %s because shared_region_map_file_np() returned %d", this->getPath(), r);
	
	return r;
}

void
ImageLoaderMachO::initMappingTable(uint64_t offsetInFat,
								   sf_mapping *mappingTable,
								   uintptr_t baseAddress)
{
	unsigned int segmentCount = fSegmentsArrayCount;
	for(unsigned int segIndex=0,entryIndex=0; segIndex < segmentCount; ++segIndex, ++entryIndex){
		Segment* seg = &fSegmentsArray[segIndex];
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

#endif // SPLIT_SEG_SHARED_REGION_SUPPORT	



void ImageLoaderMachO::setSlide(intptr_t slide)
{
	fSlide = slide;
}

void ImageLoaderMachO::parseLoadCmds()
{
	// now that segments are mapped in, get real fMachOData, fLinkEditBase, and fSlide
	for (ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		// set up pointer to __LINKEDIT segment
		if ( strcmp(seg->getName(),"__LINKEDIT") == 0 ) 
			fLinkEditBase = (uint8_t*)(seg->getActualLoadAddress(this) - seg->getFileOffset());
#if TEXT_RELOC_SUPPORT
		// __TEXT segment always starts at beginning of file and contains mach_header and load commands
		if ( strcmp(seg->getName(),"__TEXT") == 0 ) {
			if ( ((SegmentMachO*)seg)->hasFixUps() )
				fTextSegmentWithFixups = (SegmentMachO*)seg;
		}
#endif
#if __i386__
		if ( seg->readOnlyImportStubs() )
			fReadOnlyImportSegment = (SegmentMachO*)seg;
#endif
		// some segment always starts at beginning of file and contains mach_header and load commands
		if ( (seg->getFileOffset() == 0) && (seg->getFileSize() != 0) ) {
			fMachOData = (uint8_t*)(seg->getActualLoadAddress(this));
		}
	#if __ppc64__
		// in 10.5 to support images that span 4GB (including pagezero) switch meaning of r_address
		if ( ((seg->getPreferredLoadAddress() + seg->getSize() - fSegmentsArray[0].getPreferredLoadAddress()) > 0x100000000) 
		 && seg->writeable() )
			f4GBWritable = true;
	#endif
	}
	
	// keep count of prebound images with weak exports
	if ( this->hasCoalescedExports() )
		++fgCountOfImagesWithWeakExports;

	// keep count of images used in shared cache
	if ( fInSharedCache )
		++fgImagesUsedFromSharedCache;

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
				fInUmbrella = true;
				break;
			case LC_SUB_LIBRARY:
				fHasSubLibraries = true;
				break;
			case LC_ROUTINES_COMMAND:
				fHasDashInit = true;
				break;
			case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
#if IMAGE_NOTIFY_SUPPORT
					const bool isDataSeg = (strcmp(seg->segname, "__DATA") == 0);
#endif
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( type == S_MOD_INIT_FUNC_POINTERS )
							fHasInitializers = true;
						else if ( type == S_MOD_TERM_FUNC_POINTERS )
							fHasTerminators = true;
						else if ( type == S_DTRACE_DOF )
							fHasDOFSections = true;
#if IMAGE_NOTIFY_SUPPORT
						else if ( isDataSeg && (strcmp(sect->sectname, "__image_notify") == 0) )
							fHasImageNotifySection = true;
#endif
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
			case LC_RPATH:
			case LC_LOAD_WEAK_DYLIB:
		    case LC_REEXPORT_DYLIB:
				// do nothing, just prevent LC_REQ_DYLD exception from occuring
				break;
			default:
				if ( (cmd->cmd & LC_REQ_DYLD) != 0 )
					dyld::throwf("unknown required load command 0x%08X", cmd->cmd);
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
	if ( fInUmbrella ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			if (cmd->cmd == LC_SUB_FRAMEWORK) {
				const struct sub_framework_command* subf = (struct sub_framework_command*)cmd;
				const char* exportThruName = (char*)cmd + subf->umbrella.offset;
				// need to match LC_SUB_FRAMEWORK string against the leaf name of the install location of parent...
				const char* parentInstallPath = parent->getInstallPath();
				if ( parentInstallPath != NULL ) {
					const char* lastSlash = strrchr(parentInstallPath, '/');
					if ( lastSlash != NULL ) {
						if ( strcmp(&lastSlash[1], exportThruName) == 0 )
							return true;
						if ( context.imageSuffix != NULL ) {
							// when DYLD_IMAGE_SUFFIX is used, lastSlash string needs imageSuffix removed from end
							char reexportAndSuffix[strlen(context.imageSuffix)+strlen(exportThruName)+1];
							strcpy(reexportAndSuffix, exportThruName);
							strcat(reexportAndSuffix, context.imageSuffix);
							if ( strcmp(&lastSlash[1], reexportAndSuffix) == 0 )
								return true;
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
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
				return (void*)(registers->srr0 + fSlide);
			#elif __ppc64__
				const ppc_thread_state64_t* registers = (ppc_thread_state64_t*)(((char*)cmd) + 16);
				return (void*)(registers->srr0 + fSlide);
			#elif __i386__
				const i386_thread_state_t* registers = (i386_thread_state_t*)(((char*)cmd) + 16);
				return (void*)(registers->eip + fSlide);
			#elif __x86_64__
				const x86_thread_state64_t* registers = (x86_thread_state64_t*)(((char*)cmd) + 16);
				return (void*)(registers->rip + fSlide);
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
			case LC_REEXPORT_DYLIB:
				++count;
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return count;
}

void ImageLoaderMachO::doGetDependentLibraries(DependentLibraryInfo libs[])
{
	uint32_t index = 0;
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_REEXPORT_DYLIB:
			{
				const struct dylib_command* dylib = (struct dylib_command*)cmd;
				DependentLibraryInfo* lib = &libs[index++];
				lib->name = (char*)cmd + dylib->dylib.name.offset;
				//lib->name = strdup((char*)cmd + dylib->dylib.name.offset);
				lib->info.checksum = dylib->dylib.timestamp;
				lib->info.minVersion = dylib->dylib.compatibility_version;
				lib->info.maxVersion = dylib->dylib.current_version;
				lib->required = (cmd->cmd != LC_LOAD_WEAK_DYLIB);
				lib->reExported = (cmd->cmd == LC_REEXPORT_DYLIB);
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

void ImageLoaderMachO::getRPaths(const LinkContext& context, std::vector<const char*>& paths) const
{
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_RPATH:
				const char* path = (char*)cmd + ((struct rpath_command*)cmd)->path.offset;
				if ( strncmp(path, "@loader_path/", 13) == 0 ) {
					if ( issetugid() && (context.mainExecutable == this) ) {
						dyld::warn("LC_RPATH %s in %s being ignored in setuid program because of @loader_path\n", path, this->getPath());
						break;
					}
					char resolvedPath[PATH_MAX];
					if ( realpath(this->getPath(), resolvedPath) != NULL ) {
						char newRealPath[strlen(resolvedPath) + strlen(path)];
						strcpy(newRealPath, resolvedPath);
						char* addPoint = strrchr(newRealPath,'/');
						if ( addPoint != NULL )
							strcpy(&addPoint[1], &path[13]);
						else
							strcpy(newRealPath, &path[13]);
						path = strdup(newRealPath);
					}
				}
				else if ( strncmp(path, "@executable_path/", 17) == 0 ) {
					if ( issetugid() ) {
						dyld::warn("LC_RPATH %s in %s being ignored in setuid program because of @executable_path\n", path, this->getPath());
						break;
					}
					char resolvedPath[PATH_MAX];
					if ( realpath(context.mainExecutable->getPath(), resolvedPath) != NULL ) {
						char newRealPath[strlen(resolvedPath) + strlen(path)];
						strcpy(newRealPath, resolvedPath);
						char* addPoint = strrchr(newRealPath,'/');
						if ( addPoint != NULL )
							strcpy(&addPoint[1], &path[17]);
						else
							strcpy(newRealPath, &path[17]);
						path = strdup(newRealPath);
					}
				}
				else if ( (path[0] != '/') && issetugid() ) {
					dyld::warn("LC_RPATH %s in %s being ignored in setuid program because it is a relative path\n", path, this->getPath());
					break;
				}
				else {
					// make copy so that all elements of 'paths' can be freed
					path = strdup(path);
				}
				paths.push_back(path);
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

uintptr_t ImageLoaderMachO::getFirstWritableSegmentAddress()
{
	// in split segment libraries r_address is offset from first writable segment
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		if ( seg->writeable() ) 
			return seg->getActualLoadAddress(this);
	}
	throw "no writable segment";
}

uintptr_t ImageLoaderMachO::getRelocBase()
{
	// r_address is either an offset from the first segment address
	// or from the first writable segment address
#if __ppc__ || __i386__
	if ( fIsSplitSeg )
		return getFirstWritableSegmentAddress();
	else
		return fSegmentsArray[0].getActualLoadAddress(this);
#elif __ppc64__
	if ( f4GBWritable )
		return getFirstWritableSegmentAddress();
	else
		return fSegmentsArray[0].getActualLoadAddress(this);
#elif __x86_64__
	return getFirstWritableSegmentAddress();
#endif
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
	//dyld::log("dyld: ppc fixup %0p type %d from 0x%08X to 0x%08X\n", locationToFix, relocationType, before, after);
}
#endif

#if __ppc__ || __i386__
void ImageLoaderMachO::resetPreboundLazyPointers(const LinkContext& context, uintptr_t relocBase)
{
	// loop through all local (internal) relocation records looking for pre-bound-lazy-pointer values
	register const uintptr_t slide = this->fSlide;
	const relocation_info* const relocsStart = (struct relocation_info*)(&fLinkEditBase[fDynamicInfo->locreloff]);
	const relocation_info* const relocsEnd = &relocsStart[fDynamicInfo->nlocrel];
	for (const relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		if ( (reloc->r_address & R_SCATTERED) != 0 ) {
			const struct scattered_relocation_info* sreloc = (struct scattered_relocation_info*)reloc;
			if (sreloc->r_length == RELOC_SIZE) {
				uintptr_t* locationToFix = (uintptr_t*)(sreloc->r_address + relocBase);
				switch(sreloc->r_type) {
		#if __ppc__ 
					case PPC_RELOC_PB_LA_PTR:
						*locationToFix = sreloc->r_value + slide;
						break;
		#endif
		#if __i386__
					case GENERIC_RELOC_PB_LA_PTR:
						*locationToFix = sreloc->r_value + slide;
						break;
		#endif
				}
			}
		}
	}
}
#endif

void ImageLoaderMachO::doRebase(const LinkContext& context)
{
	// if prebound and loaded at prebound address, then no need to rebase
	if ( this->usablePrebinding(context) ) {
		// skip rebasing because prebinding is valid
		++fgImagesWithUsedPrebinding; // bump totals for statistics
		return;
	}
	
	// print why prebinding was not used
	if ( context.verbosePrebinding ) {
		if ( !this->isPrebindable() ) {
			dyld::log("dyld: image not prebound, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( fSlide != 0 ) {
			dyld::log("dyld: image slid, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( !this->allDependentLibrariesAsWhenPreBound() ) {
			dyld::log("dyld: dependent libraries changed, so could not use prebinding in %s\n", this->getPath());
		}
		else if ( !this->usesTwoLevelNameSpace() ){
			dyld::log("dyld: image uses flat-namespace so, parts of prebinding ignored %s\n", this->getPath());
		}
		else {
			dyld::log("dyld: environment variable disabled use of prebinding in %s\n", this->getPath());
		}
	}

	// cache values that are used in the following loop
	const uintptr_t relocBase = this->getRelocBase();
	register const uintptr_t slide = this->fSlide;

	//dyld::log("slide=0x%08lX for %s\n", slide, this->getPath());

#if __ppc__ || __i386__
	// if prebound and we got here, then prebinding is not valid, so reset all lazy pointers
	// if this image is in the shared cache, do not reset, they will be bound in doBind()
	if ( this->isPrebindable() && !fInSharedCache )
		this->resetPreboundLazyPointers(context, relocBase);
#endif

	// if in shared cache and got here, then we depend on something not in the shared cache
	if ( fInSharedCache ) 
		context.notifySharedCacheInvalid();

	// if loaded at preferred address, no rebasing necessary
	if ( slide == 0 ) 
		return;

#if TEXT_RELOC_SUPPORT
	// if there are __TEXT fixups, temporarily make __TEXT writable
	if ( fTextSegmentWithFixups != NULL ) 
		fTextSegmentWithFixups->tempWritable(context, this);
#endif
	// loop through all local (internal) relocation records
	const relocation_info* const relocsStart = (struct relocation_info*)(&fLinkEditBase[fDynamicInfo->locreloff]);
	const relocation_info* const relocsEnd = &relocsStart[fDynamicInfo->nlocrel];
	for (const relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
#if LINKEDIT_USAGE_DEBUG
		noteAccessedLinkEditAddress(reloc);
#endif
	#if __x86_64__
		// only one kind of local relocation supported for x86_64
		if ( reloc->r_length != 3 ) 
			throw "bad local relocation length";
		if ( reloc->r_type != X86_64_RELOC_UNSIGNED ) 
			throw "unknown local relocation type";
		if ( reloc->r_pcrel != 0 ) 
			throw "bad local relocation pc_rel";
		if ( reloc->r_extern != 0 ) 
			throw "extern relocation found with local relocations";
		*((uintptr_t*)(reloc->r_address + relocBase)) += slide;
	#else	
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
		#if __ppc__
					case PPC_RELOC_HI16: 
					case PPC_RELOC_LO16: 
					case PPC_RELOC_HA16: 
						// Metrowerks compiler sometimes leaves object file relocations in linked images???
						++reloc; // these relocations come in pairs, get next one
						otherRelocsPPC(locationToFix, sreloc->r_type, reloc->r_address, slide);
						break;
					case PPC_RELOC_PB_LA_PTR:
						// do nothing
						break;
		#elif __ppc64__
					case PPC_RELOC_PB_LA_PTR:
						// needed for compatibility with ppc64 binaries built with the first ld64
						// which used PPC_RELOC_PB_LA_PTR relocs instead of GENERIC_RELOC_VANILLA for lazy pointers
						*locationToFix += slide;
						break;
		#elif __i386__
					case GENERIC_RELOC_PB_LA_PTR:
						// do nothing
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
	#endif // x86_64
	}
	
#if TEXT_RELOC_SUPPORT
	// if there were __TEXT fixups, restore write protection
	if ( fTextSegmentWithFixups != NULL ) {
		fTextSegmentWithFixups->setPermissions(context,this);
		sys_icache_invalidate((void*)fTextSegmentWithFixups->getActualLoadAddress(this), fTextSegmentWithFixups->getSize());
	}
#endif	
	// update stats
	fgTotalRebaseFixups += fDynamicInfo->nlocrel;
}


const struct macho_nlist* ImageLoaderMachO::binarySearchWithToc(const char* key, const char stringPool[], const struct macho_nlist symbols[], 
												const struct dylib_table_of_contents toc[], uint32_t symbolCount, uint32_t hintIndex) const
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
	++fgTotalBindImageSearches;	

	//dyld::log("dyld: binarySearchWithToc for %s in %s\n", key, this->getShortName());

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

const struct macho_nlist* ImageLoaderMachO::binarySearch(const char* key, const char stringPool[], const struct macho_nlist symbols[], uint32_t symbolCount) const
{
	// update stats
	++fgTotalBindImageSearches;	
	++ImageLoaderMachO::fgUnhintedBinaryTreeSearchs;
	
	//dyld::log("dyld: binarySearch for %s in %s, stringpool=%p, symbols=%p, symbolCount=%u\n", 
	//				key, this->getShortName(), stringPool, symbols, symbolCount);

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

const ImageLoader::Symbol* ImageLoaderMachO::findExportedSymbol(const char* name, const void* hint, bool searchReExports, const ImageLoader** foundIn) const
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




uintptr_t ImageLoaderMachO::getExportedSymbolAddress(const Symbol* sym, const LinkContext& context, const ImageLoader* requestor) const
{
	return this->getSymbolAddress((const struct macho_nlist*)sym, requestor, context);
}

uintptr_t ImageLoaderMachO::getSymbolAddress(const struct macho_nlist* sym, const ImageLoader* requestor, const LinkContext& context) const
{
	uintptr_t result = sym->n_value + fSlide;
	return result;
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
	dyld::throwf("Symbol not found: %s\n  Referenced from: %s\n  Expected in: %s\n", symbol, referencedFrom, expectedIn); 
}

uintptr_t ImageLoaderMachO::resolveUndefined(const LinkContext& context, const struct macho_nlist* undefinedSymbol, bool twoLevel, const ImageLoader** foundIn)
{
	++fgTotalBindSymbolsResolved;
	const char* symbolName = &fStrings[undefinedSymbol->n_un.n_strx];

#if LINKEDIT_USAGE_DEBUG
	noteAccessedLinkEditAddress(undefinedSymbol);
	noteAccessedLinkEditAddress(symbolName);
#endif
	if ( context.bindFlat || !twoLevel ) {
		// flat lookup
		if ( ((undefinedSymbol->n_type & N_PEXT) != 0) && ((undefinedSymbol->n_type & N_TYPE) == N_SECT) ) {
			// is a multi-module private_extern internal reference that the linker did not optimize away
			uintptr_t addr = this->getSymbolAddress(undefinedSymbol, this, context);
			*foundIn = this;
			return addr;
		}
		const Symbol* sym;
		if ( context.flatExportFinder(symbolName, &sym, foundIn) ) {
			if ( (*foundIn != this) && !(*foundIn)->neverUnload() )
					this->addDynamicReference(*foundIn);
			return (*foundIn)->getExportedSymbolAddress(sym, context, this);
		}
		// if a bundle is loaded privately the above will not find its exports
		if ( this->isBundle() && this->hasHiddenExports() ) {
			// look in self for needed symbol
			sym = this->findExportedSymbol(symbolName, NULL, false, foundIn);
			if ( sym != NULL )
				return (*foundIn)->getExportedSymbolAddress(sym, context, this);
		}
		if ( (undefinedSymbol->n_desc & N_WEAK_REF) != 0 ) {
			// definition can't be found anywhere
			// if reference is weak_import, then it is ok, just return 0
			return 0;
		}
		throwSymbolNotFound(symbolName, this->getPath(), "flat namespace");
	}
	else {
		// symbol requires searching images with coalesced symbols (not done during prebinding)
		if ( !context.prebinding && this->needsCoalescing() && symbolRequiresCoalescing(undefinedSymbol) ) {
			const Symbol* sym;
			if ( context.coalescedExportFinder(symbolName, &sym, foundIn) ) {
				if ( (*foundIn != this) && !(*foundIn)->neverUnload() )
					this->addDynamicReference(*foundIn);
				return (*foundIn)->getExportedSymbolAddress(sym, context, this);
			}
			//throwSymbolNotFound(symbolName, this->getPath(), "coalesced namespace");
			//dyld::log("dyld: coalesced symbol %s not found in any coalesced image, falling back to two-level lookup", symbolName);
		}
		
		// if this is a real definition (not an undefined symbol) there is no ordinal
		if ( (undefinedSymbol->n_type & N_TYPE) == N_SECT ) {
			// static linker should never generate this case, but if it does, do something sane
			uintptr_t addr = this->getSymbolAddress(undefinedSymbol, this, context);
			*foundIn = this;
			return addr;
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
				return (*foundIn)->getExportedSymbolAddress(sym, context, this);
			// no image has exports this symbol
			// either report error or hope ZeroLink can just-in-time load an image
			context.undefinedHandler(symbolName);
			// try looking again
			if ( context.flatExportFinder(symbolName, &sym, foundIn) )
				return (*foundIn)->getExportedSymbolAddress(sym, context, this);
			
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
			dyld::throwf("bad mach-o binary, library ordinal (%u) too big (max %u) for symbol %s in %s",
				ord, fLibrariesCount, symbolName, this->getPath());
		}
		
		if ( target == NULL ) {
			//dyld::log("resolveUndefined(%s) in %s\n", symbolName, this->getPath());
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
			return (*foundIn)->getExportedSymbolAddress(sym, context, this);
		}
		else if ( (undefinedSymbol->n_type & N_PEXT) != 0 ) {
			// don't know why the static linker did not eliminate the internal reference to a private extern definition
			*foundIn = this;
			return this->getSymbolAddress(undefinedSymbol, this, context);
		}
		else if ( (undefinedSymbol->n_desc & N_WEAK_REF) != 0 ) {
			// if definition not found and reference is weak return 0
			return 0;
		}
		
		// nowhere to be found
		throwSymbolNotFound(symbolName, this->getPath(), target->getPath());
	}
}

// returns if 'addr' is within the address range of section 'sectionIndex'
// fSlide is not used.  'addr' is assumed to be a prebound address in this image 
bool ImageLoaderMachO::isAddrInSection(uintptr_t addr, uint8_t sectionIndex)
{
	uint8_t currentSectionIndex = 1;
	const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd = cmds;
	for (unsigned long i = 0; i < cmd_count; ++i) {
		if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
			const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
			if ( (currentSectionIndex <= sectionIndex) && (sectionIndex < currentSectionIndex+seg->nsects) ) {
				// 'sectionIndex' is in this segment, get section info
				const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
				const struct macho_section* const section = &sectionsStart[sectionIndex-currentSectionIndex];
				return ( (section->addr <= addr) && (addr < section->addr+section->size) );
			}
			else {
				// 'sectionIndex' not in this segment, skip to next segment
				currentSectionIndex += seg->nsects;
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
	return false;
}

void ImageLoaderMachO::doBindExternalRelocations(const LinkContext& context, bool onlyCoalescedSymbols)
{
	const uintptr_t relocBase = this->getRelocBase();
	const bool twoLevel = this->usesTwoLevelNameSpace();
	const bool prebound = this->isPrebindable();
	
#if TEXT_RELOC_SUPPORT
	// if there are __TEXT fixups, temporarily make __TEXT writable
	if ( fTextSegmentWithFixups != NULL ) 
		fTextSegmentWithFixups->tempWritable(context, this);
#endif
	// cache last lookup
	const struct macho_nlist*	lastUndefinedSymbol = NULL;
	uintptr_t					symbolAddr = 0;
	const ImageLoader*			image = NULL;
	
	// loop through all external relocation records and bind each
	const relocation_info* const relocsStart = (struct relocation_info*)(&fLinkEditBase[fDynamicInfo->extreloff]);
	const relocation_info* const relocsEnd = &relocsStart[fDynamicInfo->nextrel];
	for (const relocation_info* reloc=relocsStart; reloc < relocsEnd; ++reloc) {
		if (reloc->r_length == RELOC_SIZE) {
			switch(reloc->r_type) {
				case POINTER_RELOC:
					{
						const struct macho_nlist* undefinedSymbol = &fSymbolTable[reloc->r_symbolnum];
						// if only processing coalesced symbols and this one does not require coalesceing, skip to next
						if ( onlyCoalescedSymbols && !symbolRequiresCoalescing(undefinedSymbol) )
							continue;
						uintptr_t* location = ((uintptr_t*)(reloc->r_address + relocBase));
						uintptr_t value = *location;
						bool symbolAddrCached = true;
					#if __i386__
						if ( reloc->r_pcrel ) {
							value += (uintptr_t)location + 4 - fSlide;
						}
					#endif
						if ( prebound ) {
							// we are doing relocations, so prebinding was not usable
							// in a prebound executable, the n_value field of an undefined symbol is set to the address where the symbol was found when prebound
							// so, subtracting that gives the initial displacement which we need to add to the newly found symbol address
							// if mach-o relocation structs had an "addend" field this complication would not be necessary.
							if ( ((undefinedSymbol->n_type & N_TYPE) == N_SECT) && ((undefinedSymbol->n_desc & N_WEAK_DEF) != 0) ) {
								// weak symbols need special casing, since *location may have been prebound to a definition in another image.
								// If *location is currently prebound to somewhere in the same section as the weak definition, we assume 
								// that we can subtract off the weak symbol address to get the addend.
								// If prebound elsewhere, we've lost the addend and have to assume it is zero.
								// The prebinding to elsewhere only happens with 10.4+ update_prebinding which only operates on a small set of Apple dylibs
								if ( (value == undefinedSymbol->n_value) || this->isAddrInSection(value, undefinedSymbol->n_sect) )
									value -= undefinedSymbol->n_value;
								else
									value = 0;
							} 
							else {
								// is undefined or non-weak symbol, so do subtraction to get addend
								value -= undefinedSymbol->n_value;
							}
						}
						// if undefinedSymbol is same as last time, then symbolAddr and image will resolve to the same too
						if ( undefinedSymbol != lastUndefinedSymbol ) {
							symbolAddr = this->resolveUndefined(context, undefinedSymbol, twoLevel, &image);
							lastUndefinedSymbol = undefinedSymbol;
							symbolAddrCached = false;
						}
						if ( context.verboseBind ) {
							const char *path = NULL;
							if ( image != NULL ) {
								path = image->getShortName();
							}
							const char* cachedString = "(cached)";
							if ( !symbolAddrCached ) 
								cachedString = "";
							if ( value == 0 ) {
								dyld::log("dyld: bind: %s:0x%08lX = %s:%s, *0x%08lX = 0x%08lX%s\n",
										this->getShortName(), (uintptr_t)location,
										path, &fStrings[undefinedSymbol->n_un.n_strx], (uintptr_t)location, symbolAddr, cachedString);
							}
							else {
								dyld::log("dyld: bind: %s:0x%08lX = %s:%s, *0x%08lX = 0x%08lX%s + %ld\n",
										this->getShortName(), (uintptr_t)location,
										path, &fStrings[undefinedSymbol->n_un.n_strx], (uintptr_t)location, symbolAddr, cachedString, value);
							}
						}
						value += symbolAddr;
					#if __i386__
						if ( reloc->r_pcrel ) {
							*location = value - ((uintptr_t)location + 4);
						}
						else {
							// don't dirty page if prebound value was correct
							if ( !prebound || (*location != value) )
								*location = value; 
						}
					#else
						// don't dirty page if prebound value was correct
						if ( !prebound || (*location != value) )
							*location = value; 
					#endif
						// update stats
						++fgTotalBindFixups;
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
	
#if TEXT_RELOC_SUPPORT
	// if there were __TEXT fixups, restore write protection
	if ( fTextSegmentWithFixups != NULL ) {
		fTextSegmentWithFixups->setPermissions(context, this);
		sys_icache_invalidate((void*)fTextSegmentWithFixups->getActualLoadAddress(this), fTextSegmentWithFixups->getSize());
	}
#endif
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
const void* ImageLoaderMachO::getEnd() const
{
	uintptr_t lastAddress = 0;
	for (ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		uintptr_t segEnd = seg->getActualLoadAddress(this) + seg->getSize();
		if ( segEnd > lastAddress )
			lastAddress = segEnd;
	}
	return (const void*)lastAddress;
}

uintptr_t ImageLoaderMachO::bindIndirectSymbol(uintptr_t* ptrToBind, const struct macho_section* sect, const char* symbolName, uintptr_t targetAddr, const ImageLoader* targetImage, const LinkContext& context)
{
	if ( context.verboseBind ) {
		const char* path = NULL;
		if ( targetImage != NULL )
			path = targetImage->getShortName();
		dyld::log("dyld: bind: %s:%s$%s = %s:%s, *0x%08lx = 0x%08lx\n",
				this->getShortName(), symbolName, (((sect->flags & SECTION_TYPE)==S_NON_LAZY_SYMBOL_POINTERS) ? "non_lazy_ptr" : "lazy_ptr"),
				path, symbolName, (uintptr_t)ptrToBind, targetAddr);
	}
	if ( context.bindingHandler != NULL ) {
		const char* path = NULL;
		if ( targetImage != NULL )
			path = targetImage->getShortName();
		targetAddr = (uintptr_t)context.bindingHandler(path, symbolName, (void *)targetAddr);
	}
#if __i386__
	// i386 has special self-modifying stubs that change from "CALL rel32" to "JMP rel32"
	if ( ((sect->flags & SECTION_TYPE) == S_SYMBOL_STUBS) && ((sect->flags & S_ATTR_SELF_MODIFYING_CODE) != 0) && (sect->reserved2 == 5) ) {
		uint32_t rel32 = targetAddr - (((uint32_t)ptrToBind)+5);
		// re-write instruction in a thread-safe manner
		// use 8-byte compare-and-swap to alter 5-byte jump table entries
		// loop is required in case the extra three bytes that cover the next entry are altered by another thread
		bool done = false;
		while ( !done ) {
			volatile int64_t* jumpPtr = (int64_t*)ptrToBind;
			int pad = 0;
			// By default the three extra bytes swapped follow the 5-byte JMP.
			// But, if the 5-byte jump is up against the end of the __IMPORT segment
			// We don't want to access bytes off the end of the segment, so we shift
			// the extra bytes to precede the 5-byte JMP.
			if ( (((uint32_t)ptrToBind + 8) & 0x00000FFC) == 0x00000000 ) {
				jumpPtr = (int64_t*)((uint32_t)ptrToBind - 3);
				pad = 3;
			}
			int64_t oldEntry = *jumpPtr;
			union {
				int64_t int64;
				uint8_t bytes[8];
			} newEntry;
			newEntry.int64 = oldEntry;
			newEntry.bytes[pad+0] = 0xE9; // JMP rel32
			newEntry.bytes[pad+1] = rel32 & 0xFF;
			newEntry.bytes[pad+2] = (rel32 >> 8) & 0xFF;
			newEntry.bytes[pad+3] = (rel32 >> 16) & 0xFF;
			newEntry.bytes[pad+4] = (rel32 >> 24) & 0xFF;
			done = OSAtomicCompareAndSwap64Barrier(oldEntry, newEntry.int64, (int64_t*)jumpPtr);
		}
	}
	else
#endif
	*ptrToBind = targetAddr;
	return targetAddr;
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
						uint32_t symbolIndex = INDIRECT_SYMBOL_LOCAL;
						if ( type == S_LAZY_SYMBOL_POINTERS ) {
							const uint32_t pointerCount = sect->size / sizeof(uintptr_t);
							uintptr_t* const symbolPointers = (uintptr_t*)(sect->addr + fSlide);
							if ( (lazyPointer >= symbolPointers) && (lazyPointer < &symbolPointers[pointerCount]) ) {
								const uint32_t indirectTableOffset = sect->reserved1;
								const uint32_t lazyIndex = lazyPointer - symbolPointers;
								symbolIndex = indirectTable[indirectTableOffset + lazyIndex];
							}
						}
					#if __i386__
						else if ( (type == S_SYMBOL_STUBS) && (sect->flags & S_ATTR_SELF_MODIFYING_CODE) && (sect->reserved2 == 5) ) {
							// 5 bytes stubs on i386 are new "fast stubs"
							uint8_t* const jmpTableBase = (uint8_t*)(sect->addr + fSlide);
							uint8_t* const jmpTableEnd = jmpTableBase + sect->size;
							// initial CALL instruction in jump table leaves pointer to next entry, so back up
							uint8_t* const jmpTableEntryToPatch = ((uint8_t*)lazyPointer) - 5;  
							lazyPointer = (uintptr_t*)jmpTableEntryToPatch; 
							if ( (jmpTableEntryToPatch >= jmpTableBase) && (jmpTableEntryToPatch < jmpTableEnd) ) {
								const uint32_t indirectTableOffset = sect->reserved1;
								const uint32_t entryIndex = (jmpTableEntryToPatch - jmpTableBase)/5;
								symbolIndex = indirectTable[indirectTableOffset + entryIndex];
							}
						}
					#endif
						if ( symbolIndex != INDIRECT_SYMBOL_ABS && symbolIndex != INDIRECT_SYMBOL_LOCAL ) {
							const char* symbolName = &fStrings[fSymbolTable[symbolIndex].n_un.n_strx];
							const ImageLoader* image = NULL;
							uintptr_t symbolAddr = this->resolveUndefined(context, &fSymbolTable[symbolIndex],  twoLevel, &image);
					#if __i386__
							this->makeImportSegmentWritable(context);
					#endif
							symbolAddr = this->bindIndirectSymbol(lazyPointer, sect, symbolName, symbolAddr, image,  context);
							++fgTotalLazyBindFixups;
					#if __i386__
							this->makeImportSegmentReadOnly(context);
					#endif
							return symbolAddr;
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	dyld::throwf("lazy pointer not found at address %p in image %s", lazyPointer, this->getPath());
}


#if __i386__
//
// For security the __IMPORT segments in the shared cache are normally not writable.
// Any image can also be linked with -read_only_stubs to make their __IMPORT segments normally not writable.
// For these images, dyld must change the page protection before updating them.
// The spin lock is required because lazy symbol binding is done without taking the global dyld lock.
// It keeps only one __IMPORT segment writable at a time.
// Pre-main(), the shared cache __IMPORT segments are always writable, so we don't need to change the protection.
//
void ImageLoaderMachO::makeImportSegmentWritable(const LinkContext& context)
{
	if ( fReadOnlyImportSegment != NULL ) {
		if ( fInSharedCache ) {
			if ( context.startedInitializingMainExecutable ) {
				_spin_lock(&fgReadOnlyImportSpinLock);
				context.makeSharedCacheImportSegmentsWritable(true);
			}
		}
		else {
			_spin_lock(&fgReadOnlyImportSpinLock);
			fReadOnlyImportSegment->tempWritable(context, this);
		}
	}
}

void ImageLoaderMachO::makeImportSegmentReadOnly(const LinkContext& context)
{
	if ( fReadOnlyImportSegment != NULL ) {
		if ( fInSharedCache ) {
			if ( context.startedInitializingMainExecutable ) {
				context.makeSharedCacheImportSegmentsWritable(false);
				_spin_unlock(&fgReadOnlyImportSpinLock);
			}
		}
		else {
			fReadOnlyImportSegment->setPermissions(context, this);
			_spin_unlock(&fgReadOnlyImportSpinLock);
		}
	}
}
#endif

void ImageLoaderMachO::doBindIndirectSymbolPointers(const LinkContext& context, bool bindNonLazys, bool bindLazys, bool onlyCoalescedSymbols)
{
#if __i386__
	this->makeImportSegmentWritable(context);
#endif
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
						uint32_t elementSize = sizeof(uintptr_t);
						uint32_t elementCount = sect->size / elementSize;
						if ( type == S_NON_LAZY_SYMBOL_POINTERS ) {
							if ( ! bindNonLazys )
								continue;
						}
						else if ( type == S_LAZY_SYMBOL_POINTERS ) {
							// process each symbol pointer in this section
							fgTotalPossibleLazyBindFixups += elementCount;
							if ( ! bindLazys )
								continue;
						}
				#if __i386__
						else if ( (type == S_SYMBOL_STUBS) && (sect->flags & S_ATTR_SELF_MODIFYING_CODE) && (sect->reserved2 == 5) ) {
							// process each jmp entry in this section
							elementCount = sect->size / 5;
							elementSize = 5;
							fgTotalPossibleLazyBindFixups += elementCount;
							if ( ! bindLazys )
								continue;
						}
				#endif
						else {
							continue;
						}
						const uint32_t indirectTableOffset = sect->reserved1;
						uint8_t* ptrToBind = (uint8_t*)(sect->addr + fSlide);
						for (uint32_t j=0; j < elementCount; ++j, ptrToBind += elementSize) {
				#if LINKEDIT_USAGE_DEBUG
							noteAccessedLinkEditAddress(&indirectTable[indirectTableOffset + j]);
				#endif
							uint32_t symbolIndex = indirectTable[indirectTableOffset + j];
							if ( symbolIndex == INDIRECT_SYMBOL_LOCAL) {
								*((uintptr_t*)ptrToBind) += this->fSlide;
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
												dyld::log("dyld: malformed executable '%s', skipping indirect symbol to %s\n",
														this->getPath(), &fStrings[sym->n_un.n_strx]);
												alreadyWarned = true;
											}
											continue;
										}
									}
								}
								const ImageLoader* image = NULL;
								// if only processing coalesced symbols and this one does not require coalesceing, skip to next
								if ( onlyCoalescedSymbols && !symbolRequiresCoalescing(sym) )
									continue;
								uintptr_t symbolAddr;
									symbolAddr = resolveUndefined(context, sym, twoLevel, &image);
									
								// update pointer
								symbolAddr = this->bindIndirectSymbol((uintptr_t*)ptrToBind, sect, &fStrings[sym->n_un.n_strx], symbolAddr, image,  context);
								// update stats
								++fgTotalBindFixups;
							}
						}
					}
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
#if __i386__
	this->makeImportSegmentReadOnly(context);
#endif
}

#if SUPPORT_OLD_CRT_INITIALIZATION
// first 16 bytes of "start" in crt1.o
#if __ppc__
	static uint32_t sStandardEntryPointInstructions[4] = { 0x7c3a0b78, 0x3821fffc, 0x54210034, 0x38000000 };
#elif __i386__
	static uint8_t sStandardEntryPointInstructions[16] = { 0x6a, 0x00, 0x89, 0xe5, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x10, 0x8b, 0x5d, 0x04, 0x89, 0x5c, 0x24 };
#endif
#endif

struct DATAdyld {
	void*			dyldLazyBinder;		// filled in at launch by dyld to point into dyld to &stub_binding_helper_interface
	void*			dyldFuncLookup;		// filled in at launch by dyld to point into dyld to &_dyld_func_lookup
	// the following only exist in main executables built for 10.5 or later
	ProgramVars		vars;
};

// These are defined in dyldStartup.s
extern "C" void stub_binding_helper();
extern "C" bool dyld_func_lookup(const char* name, uintptr_t* address);
extern "C" void fast_stub_binding_helper_interface();


void ImageLoaderMachO::setupLazyPointerHandler(const LinkContext& context)
{
	const macho_header* mh = (macho_header*)fMachOData;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
	const struct load_command* cmd;
	// set up __dyld section
	// optimizations:
	//   1) do nothing if image is in dyld shared cache and dyld loaded at same address as when cache built
	//	 2) first read __dyld value, if already correct don't write, this prevents dirtying a page
	if ( !fInSharedCache || !context.dyldLoadedAtSameAddressNeededBySharedCache ) {
		cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				if ( strcmp(seg->segname, "__DATA") == 0 ) {
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if ( strcmp(sect->sectname, "__dyld" ) == 0 ) {
							struct DATAdyld* dd = (struct DATAdyld*)(sect->addr + fSlide);
							if ( sect->size > offsetof(DATAdyld, dyldLazyBinder) ) {
								if ( dd->dyldLazyBinder != (void*)&stub_binding_helper )
									dd->dyldLazyBinder = (void*)&stub_binding_helper;
							}
							if ( sect->size > offsetof(DATAdyld, dyldFuncLookup) ) {
								if ( dd->dyldFuncLookup != (void*)&dyld_func_lookup )
									dd->dyldFuncLookup = (void*)&dyld_func_lookup;
							}
							if ( mh->filetype == MH_EXECUTE ) {
								// there are two ways to get the program variables
								if ( (sect->size > offsetof(DATAdyld, vars)) && (dd->vars.mh == mh) ) {
									// some really old binaries have space for vars, but it is zero filled
									// main executable has 10.5 style __dyld section that has program variable pointers
									context.setNewProgramVars(dd->vars);
								}
								else {
									// main executable is pre-10.5 and requires the symbols names to be looked up
									this->lookupProgramVars(context);
				#if SUPPORT_OLD_CRT_INITIALIZATION
									// If the first 16 bytes of the entry point's instructions do not 
									// match what crt1.o supplies, then the program has a custom entry point.
									// This means it might be doing something that needs to be executed before 
									// initializers are run. 
									if ( memcmp(this->getMain(), sStandardEntryPointInstructions, 16) != 0 ) {
										if ( context.verboseInit )
											dyld::log("dyld: program uses non-standard entry point so delaying running of initializers\n");
										context.setRunInitialzersOldWay();
									}
				#endif
								}
							}
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
#if __i386__
	if ( ! this->usablePrebinding(context) ) {
		// reset all "fast" stubs
		this->makeImportSegmentWritable(context);
		cmd = cmds;
		for (uint32_t i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd) {
				case LC_SEGMENT_COMMAND:
				{
					const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						const uint8_t type = sect->flags & SECTION_TYPE;
						if ( (type == S_SYMBOL_STUBS) && (sect->flags & S_ATTR_SELF_MODIFYING_CODE) && (sect->reserved2 == 5) ) {
							// reset each jmp entry in this section
							const uint32_t indirectTableOffset = sect->reserved1;
							const uint32_t* const indirectTable = (uint32_t*)&fLinkEditBase[fDynamicInfo->indirectsymoff];
							uint8_t* start = (uint8_t*)(sect->addr + this->fSlide);
							uint8_t* end = start + sect->size;
							uintptr_t dyldHandler = (uintptr_t)&fast_stub_binding_helper_interface;
							uint32_t entryIndex = 0;
							for (uint8_t* entry = start; entry < end; entry += 5, ++entryIndex) {
								bool installLazyHandler = true;
								// jump table entries that cross a (64-byte) cache line boundary have the potential to cause crashes
								// if the instruction is updated by one thread while being executed by another
								if ( ((uint32_t)entry & 0xFFFFFFC0) != ((uint32_t)entry+4 & 0xFFFFFFC0) ) {
									// need to bind this now to avoid a potential problem if bound lazily
									uint32_t symbolIndex = indirectTable[indirectTableOffset + entryIndex];
									// the latest linker marks 64-byte crossing stubs with INDIRECT_SYMBOL_ABS so they are not used
									if ( symbolIndex != INDIRECT_SYMBOL_ABS ) {
										const char* symbolName = &fStrings[fSymbolTable[symbolIndex].n_un.n_strx];
										const ImageLoader* image = NULL;
										try {
											uintptr_t symbolAddr = this->resolveUndefined(context, &fSymbolTable[symbolIndex], this->usesTwoLevelNameSpace(), &image);
											symbolAddr = this->bindIndirectSymbol((uintptr_t*)entry, sect, symbolName, symbolAddr, image, context);
											++fgTotalBindFixups;
											uint32_t rel32 = symbolAddr - (((uint32_t)entry)+5);
											entry[0] = 0xE9; // JMP rel32
											entry[1] = rel32 & 0xFF;
											entry[2] = (rel32 >> 8) & 0xFF;
											entry[3] = (rel32 >> 16) & 0xFF;
											entry[4] = (rel32 >> 24) & 0xFF;
											installLazyHandler = false;
										} 
										catch (const char* msg) {
											// ignore errors when binding symbols early
											// maybe the function is never called, and therefore erroring out now would be a regression
										}
									}
								}
								if ( installLazyHandler ) {
									uint32_t rel32 = dyldHandler - (((uint32_t)entry)+5);
									entry[0] = 0xE8; // CALL rel32
									entry[1] = rel32 & 0xFF;
									entry[2] = (rel32 >> 8) & 0xFF;
									entry[3] = (rel32 >> 16) & 0xFF;
									entry[4] = (rel32 >> 24) & 0xFF;
								}
							}
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
		this->makeImportSegmentReadOnly(context);
	}
#endif
}

									
void ImageLoaderMachO::lookupProgramVars(const LinkContext& context) const
{
	ProgramVars vars = context.programVars;
	const ImageLoader::Symbol* sym;
	
	// get mach header directly
	vars.mh = (macho_header*)fMachOData;
	
	// lookup _NXArgc
	sym = this->findExportedSymbol("_NXArgc", NULL, false, NULL);
	if ( sym != NULL )
		vars.NXArgcPtr = (int*)this->getExportedSymbolAddress(sym, context, this);
		
	// lookup _NXArgv
	sym = this->findExportedSymbol("_NXArgv", NULL, false, NULL);
	if ( sym != NULL )
		vars.NXArgvPtr = (const char***)this->getExportedSymbolAddress(sym, context, this);
		
	// lookup _environ
	sym = this->findExportedSymbol("_environ", NULL, false, NULL);
	if ( sym != NULL )
		vars.environPtr = (const char***)this->getExportedSymbolAddress(sym, context, this);
		
	// lookup __progname
	sym = this->findExportedSymbol("___progname", NULL, false, NULL);
	if ( sym != NULL )
		vars.__prognamePtr = (const char**)this->getExportedSymbolAddress(sym, context, this);
		
	context.setNewProgramVars(vars);
}


bool ImageLoaderMachO::usablePrebinding(const LinkContext& context) const
{
	// if prebound and loaded at prebound address, and all libraries are same as when this was prebound, then no need to bind
	if ( (this->isPrebindable() || fInSharedCache)
		&& (this->getSlide() == 0) 
		&& this->usesTwoLevelNameSpace()
		&& this->allDependentLibrariesAsWhenPreBound() ) {
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

void ImageLoaderMachO::doBindJustLazies(const LinkContext& context)
{
	// some API called requested that all lazy pointers in this image be force bound
	this->doBindIndirectSymbolPointers(context, false, true, false);
}

void ImageLoaderMachO::doBind(const LinkContext& context, bool forceLazysBound)
{
	// set dyld entry points in image
	this->setupLazyPointerHandler(context);

	// if prebound and loaded at prebound address, and all libraries are same as when this was prebound, then no need to bind
	// note: flat-namespace binaries need to have imports rebound (even if correctly prebound)
	if ( this->usablePrebinding(context) ) {
		// if image has coalesced symbols, then these need to be rebound, unless this is the only image with weak symbols
		if ( this->needsCoalescing() && (fgCountOfImagesWithWeakExports > 1) ) {
			this->doBindExternalRelocations(context, true);
			this->doBindIndirectSymbolPointers(context, true, true, true);
		}
		else {
			++fgImagesRequiringNoFixups;
		}
		
		// skip binding because prebound and prebinding not disabled
		return;
	}
	
 	// values bound by name are stored two different ways in mach-o:
	
	// 1) external relocations are used for data initialized to external symbols
	this->doBindExternalRelocations(context, false);
	
	// 2) "indirect symbols" are used for code references to external symbols
	// if this image is in the shared cache, there is noway to reset the lazy pointers, so bind them now
	this->doBindIndirectSymbolPointers(context, true, forceLazysBound || fInSharedCache, false);
}

void ImageLoaderMachO::doUpdateMappingPermissions(const LinkContext& context)
{
#if __i386__
	if ( (fReadOnlyImportSegment != NULL) && !fInSharedCache )
		fReadOnlyImportSegment->setPermissions(context, this);
#endif
}

void ImageLoaderMachO::doImageInit(const LinkContext& context)
{
	if ( fHasDashInit ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (unsigned long i = 0; i < cmd_count; ++i) {
			switch (cmd->cmd) {
				case LC_ROUTINES_COMMAND:
					Initializer func = (Initializer)(((struct macho_routines_command*)cmd)->init_address + fSlide);
					if ( context.verboseInit )
						dyld::log("dyld: calling -init function 0x%p in %s\n", func, this->getPath());
					func(context.argc, context.argv, context.envp, context.apple, &context.programVars);
					break;
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}

void ImageLoaderMachO::doModInitFunctions(const LinkContext& context)
{
	if ( fHasInitializers ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (unsigned long i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
				const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
				for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
					const uint8_t type = sect->flags & SECTION_TYPE;
					if ( type == S_MOD_INIT_FUNC_POINTERS ) {
						Initializer* inits = (Initializer*)(sect->addr + fSlide);
						const uint32_t count = sect->size / sizeof(uintptr_t);
						for (uint32_t i=0; i < count; ++i) {
							Initializer func = inits[i];
							if ( context.verboseInit )
								dyld::log("dyld: calling initializer function %p in %s\n", func, this->getPath());
							func(context.argc, context.argv, context.envp, context.apple, &context.programVars);
						}
					}
				}
				cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
			}
		}
	}
}




void ImageLoaderMachO::doGetDOFSections(const LinkContext& context, std::vector<ImageLoader::DOFInfo>& dofs)
{
	if ( fHasDOFSections ) {
		// walk load commands (mapped in at start of __TEXT segment)
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
							if ( (sect->flags & SECTION_TYPE) == S_DTRACE_DOF ) {
								ImageLoader::DOFInfo info;
								info.dof			= (void*)(sect->addr + fSlide);
								info.imageHeader	= this->machHeader();
								info.imageShortName = this->getShortName();
								dofs.push_back(info);
							}
						}
					}
					break;
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
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
	return ( fHasDashInit || fHasInitializers );
}


bool ImageLoaderMachO::needsTermination()
{
	return fHasTerminators;
}

#if IMAGE_NOTIFY_SUPPORT
bool ImageLoaderMachO::hasImageNotification()
{
	return fHasImageNotifySection;
}
#endif

void ImageLoaderMachO::doTermination(const LinkContext& context)
{
	if ( fHasTerminators ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (unsigned long i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
				const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
				for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
					const uint8_t type = sect->flags & SECTION_TYPE;
					if ( type == S_MOD_TERM_FUNC_POINTERS ) {
						Terminator* terms = (Terminator*)(sect->addr + fSlide);
						const uint32_t count = sect->size / sizeof(uintptr_t);
						for (uint32_t i=count; i > 0; --i) {
							Terminator func = terms[i-1];
							if ( context.verboseInit )
								dyld::log("dyld: calling terminaton function %p in %s\n", func, this->getPath());
							func();
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}

#if IMAGE_NOTIFY_SUPPORT
void ImageLoaderMachO::doNotification(enum dyld_image_mode mode, uint32_t infoCount, const struct dyld_image_info info[])
{
	if ( fHasImageNotifySection ) {
		const uint32_t cmd_count = ((macho_header*)fMachOData)->ncmds;
		const struct load_command* const cmds = (struct load_command*)&fMachOData[sizeof(macho_header)];
		const struct load_command* cmd = cmds;
		for (unsigned long i = 0; i < cmd_count; ++i) {
			if ( cmd->cmd == LC_SEGMENT_COMMAND ) {
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				if ( strcmp(seg->segname, "__DATA") == 0 ) {
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if ( strcmp(sect->sectname, "__image_notify") == 0 ) {
							dyld_image_notifier* notes = (dyld_image_notifier*)(sect->addr + fSlide);
							const uint32_t count = sect->size / sizeof(uintptr_t);
							for (uint32_t i=count; i > 0; --i) {
								dyld_image_notifier func = notes[i-1];
								func(mode, infoCount, info);
							}
						}
					}
				}
			}
			cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		}
	}
}
#endif

void ImageLoaderMachO::printStatistics(unsigned int imageCount)
{
	ImageLoader::printStatistics(imageCount);
	//dyld::log("total hinted binary tree searches:    %d\n", fgHintedBinaryTreeSearchs);
	//dyld::log("total unhinted binary tree searches:  %d\n", fgUnhintedBinaryTreeSearchs);
	dyld::log("total images with weak exports:  %d\n", fgCountOfImagesWithWeakExports);
	
#if LINKEDIT_USAGE_DEBUG
	dyld::log("linkedit pages accessed (%lu):\n", sLinkEditPageBuckets.size());
#endif	
}


ImageLoader::SegmentIterator	ImageLoaderMachO::beginSegments() const
{
	return SegmentIterator(fSegmentsArray);
}

ImageLoader::SegmentIterator	ImageLoaderMachO::endSegments() const
{
	return SegmentIterator(&fSegmentsArray[fSegmentsArrayCount]);
}

SegmentMachO::SegmentMachO(const struct macho_segment_command* cmd)
 : fSegmentLoadCommand(cmd)
{
}

SegmentMachO::~SegmentMachO()
{
}

void SegmentMachO::adjust(const struct macho_segment_command* cmd)
{
	fSegmentLoadCommand = cmd;
}

void SegmentMachO::unmap(const ImageLoader* image)
{
	// update stats
	--ImageLoader::fgTotalSegmentsMapped;
	ImageLoader::fgTotalBytesMapped -= fSegmentLoadCommand->vmsize;
	munmap((void*)(this->getActualLoadAddress(image)), fSegmentLoadCommand->vmsize);
}


const char* SegmentMachO::getName()
{
	return fSegmentLoadCommand->segname;
}

uintptr_t SegmentMachO::getSize()
{
	return fSegmentLoadCommand->vmsize;
}

uintptr_t SegmentMachO::getFileSize()
{
	return fSegmentLoadCommand->filesize;
}

uintptr_t SegmentMachO::getFileOffset()
{
	return fSegmentLoadCommand->fileoff;
}

bool SegmentMachO::readable()
{
	return ( (fSegmentLoadCommand->initprot & VM_PROT_READ) != 0);
}

bool SegmentMachO::writeable()
{
	return ((fSegmentLoadCommand->initprot & VM_PROT_WRITE) != 0);
}

bool SegmentMachO::executable()
{
	return ((fSegmentLoadCommand->initprot & VM_PROT_EXECUTE) != 0);
}

bool SegmentMachO::unaccessible()
{
	return (fSegmentLoadCommand->initprot == 0);
}

#if TEXT_RELOC_SUPPORT
bool SegmentMachO::hasFixUps()
{
	// scan sections for fix-up bit
	const struct macho_section* const sectionsStart = (struct macho_section*)((char*)fSegmentLoadCommand + sizeof(struct macho_segment_command));
	const struct macho_section* const sectionsEnd = &sectionsStart[fSegmentLoadCommand->nsects];
	for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
		if ( (sect->flags & (S_ATTR_EXT_RELOC | S_ATTR_LOC_RELOC)) != 0 )
			return true;
	}
	return false;
}
#endif

#if __i386__
bool SegmentMachO::readOnlyImportStubs()
{
	return (    (fSegmentLoadCommand->initprot & VM_PROT_EXECUTE) 
			&& ((fSegmentLoadCommand->initprot & VM_PROT_WRITE) == 0) 
			&& (strcmp(fSegmentLoadCommand->segname, "__IMPORT") == 0) );
}
#endif
	
uintptr_t SegmentMachO::getActualLoadAddress(const ImageLoader* inImage)
{
	return fSegmentLoadCommand->vmaddr + inImage->getSlide();
}

uintptr_t SegmentMachO::getPreferredLoadAddress()
{
	return fSegmentLoadCommand->vmaddr;
}

bool SegmentMachO::hasPreferredLoadAddress()
{
	return (fSegmentLoadCommand->vmaddr != 0);
}


Segment* SegmentMachO::next(Segment* location)
{
	return &((SegmentMachO*)location)[1];
}






