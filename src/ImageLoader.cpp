/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2006 Apple Computer, Inc. All rights reserved.
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

#define __STDC_LIMIT_MACROS
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach-o/fat.h> 
#include <sys/types.h>
#include <sys/stat.h> 
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <libkern/OSAtomic.h>

#include "ImageLoader.h"

// in libc.a
extern "C" void _spin_lock(uint32_t*);
extern "C" void _spin_unlock(uint32_t*);

uint32_t								ImageLoader::fgImagesUsedFromSharedCache = 0;
uint32_t								ImageLoader::fgImagesWithUsedPrebinding = 0;
uint32_t								ImageLoader::fgImagesRequiringNoFixups = 0;
uint32_t								ImageLoader::fgTotalRebaseFixups = 0;
uint32_t								ImageLoader::fgTotalBindFixups = 0;
uint32_t								ImageLoader::fgTotalBindSymbolsResolved = 0;
uint32_t								ImageLoader::fgTotalBindImageSearches = 0;
uint32_t								ImageLoader::fgTotalLazyBindFixups = 0;
uint32_t								ImageLoader::fgTotalPossibleLazyBindFixups = 0;
uint32_t								ImageLoader::fgTotalSegmentsMapped = 0;
uint64_t								ImageLoader::fgTotalBytesMapped = 0;
uint64_t								ImageLoader::fgTotalBytesPreFetched = 0;
uint64_t								ImageLoader::fgTotalLoadLibrariesTime;
uint64_t								ImageLoader::fgTotalRebaseTime;
uint64_t								ImageLoader::fgTotalBindTime;
uint64_t								ImageLoader::fgTotalInitTime;
uintptr_t								ImageLoader::fgNextSplitSegAddress = 0x90000000;
uint16_t								ImageLoader::fgLoadOrdinal = 0;
uintptr_t								Segment::fgNextPIEDylibAddress = 0;


void ImageLoader::init(const char* path, uint64_t offsetInFat, dev_t device, ino_t inode, time_t modDate)
{
	fPathHash = 0;
	fPath = path;
	fLogicalPath = NULL;
	fDevice = device;
	fInode = inode;
	fLastModified = modDate;
	fOffsetInFatFile = offsetInFat;
	fLibraries = NULL;
	fLibrariesCount = 0;
	fDlopenReferenceCount = 0;
	fStaticReferenceCount = 0;
	fDynamicReferenceCount = 0;
	fDynamicReferences = NULL;
	fDepth = 0;
	fLoadOrder = fgLoadOrdinal++;
	fState = 0;
	fAllLibraryChecksumsAndLoadAddressesMatch = false;
	fLeaveMapped = false;
	fNeverUnload = false;
	fHideSymbols = false;
	fMatchByInstallName = false;
	fRegisteredDOF = false;
#if IMAGE_NOTIFY_SUPPORT
	fAnnounced = false;
#endif
	fAllLazyPointersBound = false;
	fBeingRemoved = false;
	fAddFuncNotified = false;
	fPathOwnedByImage = false;
#if RECURSIVE_INITIALIZER_LOCK
	fInitializerRecursiveLock = NULL;
#else
	fInitializerLock = 0;
#endif
	if ( fPath != NULL )
		fPathHash = hash(fPath);
}


ImageLoader::ImageLoader(const char* path, uint64_t offsetInFat, const struct stat& info)
{
	init(path, offsetInFat, info.st_dev, info.st_ino, info.st_mtime);
}

ImageLoader::ImageLoader(const char* moduleName)
{
	init(moduleName, 0, 0, 0, 0);
}


ImageLoader::~ImageLoader()
{
	if ( fPathOwnedByImage && (fPath != NULL) ) 
		delete [] fPath;
	if ( fLogicalPath != NULL ) 
		delete [] fLogicalPath;
	if ( fLibraries != NULL ) {
		for (uint32_t i = 0; i < fLibrariesCount; ++i) {
			if ( fLibraries[i].image != NULL )
				fLibraries[i].image->fStaticReferenceCount--;
		}
		delete [] fLibraries;
	}
	if ( fDynamicReferences != NULL ) {
		for (std::set<const ImageLoader*>::iterator it = fDynamicReferences->begin(); it != fDynamicReferences->end(); ++it ) {
			const_cast<ImageLoader*>(*it)->fDynamicReferenceCount--;
		}
		delete fDynamicReferences;
	}
}

void ImageLoader::setMapped(const LinkContext& context)
{
	fState = dyld_image_state_mapped;
	context.notifySingle(dyld_image_state_mapped, this->machHeader(), fPath, fLastModified);
}

void ImageLoader::addDynamicReference(const ImageLoader* target)
{
	if ( fDynamicReferences == NULL )
		fDynamicReferences = new std::set<const ImageLoader*>();
	if ( fDynamicReferences->count(target) == 0 ) {	
		fDynamicReferences->insert(target);
		const_cast<ImageLoader*>(target)->fDynamicReferenceCount++;
	}
	//dyld::log("dyld: addDynamicReference() from %s to %s, fDynamicReferences->size()=%lu\n", this->getPath(), target->getPath(), fDynamicReferences->size());
}

int ImageLoader::compare(const ImageLoader* right) const
{
	if ( this->fDepth == right->fDepth ) {
		if ( this->fLoadOrder == right->fLoadOrder )
			return 0;
		else if ( this->fLoadOrder < right->fLoadOrder )
			return -1;
		else
			return 1;
	}
	else {
		if ( this->fDepth < right->fDepth )
			return -1;
		else
			return 1;
	}
}

void ImageLoader::setPath(const char* path)
{
	if ( fPathOwnedByImage && (fPath != NULL) ) 
		delete [] fPath;
	fPath = new char[strlen(path)+1];
	strcpy((char*)fPath, path);
	fPathOwnedByImage = true;  // delete fPath when this image is destructed
	fPathHash = hash(fPath);
}

void ImageLoader::setPathUnowned(const char* path)
{
	if ( fPathOwnedByImage && (fPath != NULL) ) {
		delete [] fPath;
	}
	fPath = path;
	fPathOwnedByImage = false;  
	fPathHash = hash(fPath);
}

void ImageLoader::setLogicalPath(const char* path)
{
	if ( fPath == NULL ) {
		// no physical path set yet, so use this path as physical
		this->setPath(path);
	}
	else if ( strcmp(path, fPath) == 0 ) {
		// do not set logical path because it is the same as the physical path
		fLogicalPath = NULL;
	}
	else {
		fLogicalPath = new char[strlen(path)+1];
		strcpy((char*)fLogicalPath, path);
	}
}

const char* ImageLoader::getLogicalPath() const
{
	if ( fLogicalPath != NULL )
		return fLogicalPath;
	else
		return fPath;
}

uint32_t ImageLoader::hash(const char* path)
{
	// this does not need to be a great hash
	// it is just used to reduce the number of strcmp() calls
	// of existing images when loading a new image
	uint32_t h = 0;
	for (const char* s=path; *s != '\0'; ++s)
		h = h*5 + *s;
	return h;
}

bool ImageLoader::matchInstallPath() const
{
	return fMatchByInstallName;
}

void ImageLoader::setMatchInstallPath(bool match)
{
	fMatchByInstallName = match;
}

bool ImageLoader::statMatch(const struct stat& stat_buf) const
{
	return ( (this->fDevice == stat_buf.st_dev) && (this->fInode == stat_buf.st_ino) );	
}

const char* ImageLoader::getShortName() const
{
	// try to return leaf name
	if ( fPath != NULL ) {
		const char* s = strrchr(fPath, '/');
		if ( s != NULL ) 
			return &s[1];
	}
	return fPath; 
}

uint64_t ImageLoader::getOffsetInFatFile() const
{
	return fOffsetInFatFile;
}

void ImageLoader::setLeaveMapped()
{
	fLeaveMapped = true;
}

void ImageLoader::setHideExports(bool hide)
{
	fHideSymbols = hide;
}

bool ImageLoader::hasHiddenExports() const
{
	return fHideSymbols;
}

bool ImageLoader::isLinked() const
{
	return (fState >= dyld_image_state_bound);
}

time_t ImageLoader::lastModified() const
{
	return fLastModified;
}

bool ImageLoader::containsAddress(const void* addr) const
{
	if ( ! this->isLinked() )
		return false;
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		const uint8_t* start = (const uint8_t*)seg->getActualLoadAddress(this);
		const uint8_t* end = start + seg->getSize();
		if ( (start <= addr) && (addr < end) && !seg->unaccessible() )
			return true;
	}
	return false;
}

bool ImageLoader::overlapsWithAddressRange(const void* start, const void* end) const
{
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		const uint8_t* segStart = (const uint8_t*)(seg->getActualLoadAddress(this));
		const uint8_t* segEnd = segStart + seg->getSize();
		if ( (start <= segStart) && (segStart < end) )
			return true;
		if ( (start <= segEnd) && (segEnd < end) )
			return true;
		if ( (segStart < start) && (end < segEnd) )
			return true;
	}
	return false;
}

void ImageLoader::getMappedRegions(MappedRegion*& regions) const
{
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		MappedRegion region;
		region.address = seg->getActualLoadAddress(this);
		region.size = seg->getSize();
		*regions++ = region;
	}
}


static bool notInImgageList(const ImageLoader* image, const ImageLoader** dsiStart, const ImageLoader** dsiCur)
{
	for (const ImageLoader** p = dsiStart; p < dsiCur; ++p)
		if ( *p == image )
			return false;
	return true;
}


// private method that handles circular dependencies by only search any image once
const ImageLoader::Symbol* ImageLoader::findExportedSymbolInDependentImagesExcept(const char* name, 
			const ImageLoader** dsiStart, const ImageLoader**& dsiCur, const ImageLoader** dsiEnd, const ImageLoader** foundIn) const
{
	const ImageLoader::Symbol* sym;
	
	// search self
	if ( notInImgageList(this, dsiStart, dsiCur) ) {
		sym = this->findExportedSymbol(name, NULL, false, foundIn);
		if ( sym != NULL )
			return sym;
		*dsiCur++ = this;
	}

	// search directly dependent libraries
	for (uint32_t i=0; i < fLibrariesCount; ++i) {
		ImageLoader* dependentImage = fLibraries[i].image;
		if ( (dependentImage != NULL) && notInImgageList(dependentImage, dsiStart, dsiCur) ) {
			const ImageLoader::Symbol* sym = dependentImage->findExportedSymbol(name, NULL, false, foundIn);
			if ( sym != NULL )
				return sym;
		}
	}
	
	// search indirectly dependent libraries
	for (uint32_t i=0; i < fLibrariesCount; ++i) {
		ImageLoader* dependentImage = fLibraries[i].image;
		if ( (dependentImage != NULL) && notInImgageList(dependentImage, dsiStart, dsiCur) ) {
			*dsiCur++ = dependentImage; 
			const ImageLoader::Symbol* sym = dependentImage->findExportedSymbolInDependentImagesExcept(name, dsiStart, dsiCur, dsiEnd, foundIn);
			if ( sym != NULL )
				return sym;
		}
	}

	return NULL;
}


const ImageLoader::Symbol* ImageLoader::findExportedSymbolInDependentImages(const char* name, const LinkContext& context, const ImageLoader** foundIn) const
{
	unsigned int imageCount = context.imageCount();
	const ImageLoader* dontSearchImages[imageCount];
	dontSearchImages[0] = this; // don't search this image
	const ImageLoader** cur = &dontSearchImages[1];
	return this->findExportedSymbolInDependentImagesExcept(name, &dontSearchImages[0], cur, &dontSearchImages[imageCount], foundIn);
}

const ImageLoader::Symbol* ImageLoader::findExportedSymbolInImageOrDependentImages(const char* name, const LinkContext& context, const ImageLoader** foundIn) const
{
	unsigned int imageCount = context.imageCount();
	const ImageLoader* dontSearchImages[imageCount];
	const ImageLoader** cur = &dontSearchImages[0];
	return this->findExportedSymbolInDependentImagesExcept(name, &dontSearchImages[0], cur, &dontSearchImages[imageCount], foundIn);
}


void ImageLoader::link(const LinkContext& context, bool forceLazysBound, bool preflightOnly, const RPathChain& loaderRPaths)
{
	//dyld::log("ImageLoader::link(%s) refCount=%d, neverUnload=%d\n", this->getPath(), fStaticReferenceCount, fNeverUnload);
	
	uint64_t t0 = mach_absolute_time();
	this->recursiveLoadLibraries(context,loaderRPaths);
	context.notifyBatch(dyld_image_state_dependents_mapped);
	
	// we only do the loading step for preflights
	if ( preflightOnly )
		return;
		
	uint64_t t1 = mach_absolute_time();
	context.clearAllDepths();
	this->recursiveUpdateDepth(context.imageCount());

	uint64_t t2 = mach_absolute_time();
 	this->recursiveRebase(context);
	context.notifyBatch(dyld_image_state_rebased);
	
	uint64_t t3 = mach_absolute_time();
 	this->recursiveBind(context, forceLazysBound);
	context.notifyBatch(dyld_image_state_bound);

	uint64_t t4 = mach_absolute_time();	
	std::vector<DOFInfo> dofs;
	this->recursiveGetDOFSections(context, dofs);
	context.registerDOFs(dofs);

	
	fgTotalLoadLibrariesTime += t1 - t0;
	fgTotalRebaseTime += t3 - t2;
	fgTotalBindTime += t4 - t3;
	
	// done with initial dylib loads
	Segment::fgNextPIEDylibAddress = 0;
}


void ImageLoader::printReferenceCounts()
{
	dyld::log("      dlopen=%d, static=%d, dynamic=%d for %s\n", 
				fDlopenReferenceCount, fStaticReferenceCount, fDynamicReferenceCount, getPath() );
}


bool ImageLoader::decrementDlopenReferenceCount() 
{
	if ( fDlopenReferenceCount == 0 )
		return true;
	--fDlopenReferenceCount;
	return false;
}

void ImageLoader::runInitializers(const LinkContext& context)
{
#if IMAGE_NOTIFY_SUPPORT
	ImageLoader* newImages[context.imageCount()];
	ImageLoader** end = newImages;
	this->recursiveImageAnnouncement(context, end);	 // build bottom up list images being added
	context.notifyAdding(newImages, end-newImages);	// tell anyone who cares about these
#endif

	uint64_t t1 = mach_absolute_time();
	this->recursiveInitialization(context, mach_thread_self());
	context.notifyBatch(dyld_image_state_initialized);
	uint64_t t2 = mach_absolute_time();
	fgTotalInitTime += (t2 - t1);
}


void ImageLoader::bindAllLazyPointers(const LinkContext& context, bool recursive)
{
	if ( ! fAllLazyPointersBound ) {
		fAllLazyPointersBound = true;

		if ( recursive ) {
			// bind lower level libraries first
			for(unsigned int i=0; i < fLibrariesCount; ++i){
				DependentLibrary& libInfo = fLibraries[i];
				if ( libInfo.image != NULL )
					libInfo.image->bindAllLazyPointers(context, recursive);
			}
		}
		// bind lazies in this image
		this->doBindJustLazies(context);
	}
}


intptr_t ImageLoader::assignSegmentAddresses(const LinkContext& context)
{
	// preflight and calculate slide if needed
	intptr_t slide = 0;
	if ( this->segmentsCanSlide() && this->segmentsMustSlideTogether() ) {
		bool needsToSlide = false;
		uintptr_t lowAddr = UINTPTR_MAX;
		uintptr_t highAddr = 0;
		for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
			Segment* seg = *it;
			const uintptr_t segLow = seg->getPreferredLoadAddress();
			const uintptr_t segHigh = (segLow + seg->getSize() + 4095) & -4096;
			if ( segLow < lowAddr )
				lowAddr = segLow;
			if ( segHigh > highAddr )
				highAddr = segHigh;
				
			if ( !seg->hasPreferredLoadAddress() || !Segment::reserveAddressRange(seg->getPreferredLoadAddress(), seg->getSize()) )
				needsToSlide = true;
		}
		if ( needsToSlide ) {
			// find a chunk of address space to hold all segments
			uintptr_t addr = Segment::reserveAnAddressRange(highAddr-lowAddr, context);
			slide = addr - lowAddr;
		}
	} 
	else if ( ! this->segmentsCanSlide() ) {
		for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
			Segment* seg = *it;
			if ( strcmp(seg->getName(), "__PAGEZERO") == 0 )
				continue;
			if ( !Segment::reserveAddressRange(seg->getPreferredLoadAddress(), seg->getSize()) )
				throw "can't map";
		}
	}
	else {
		// mach-o does not support independently sliding segments
	}
	return slide;
}


void ImageLoader::mapSegments(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context)
{
	if ( context.verboseMapping )
		dyld::log("dyld: Mapping %s\n", this->getPath());
	// find address range for image
	intptr_t slide = this->assignSegmentAddresses(context);
	// map in all segments
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		seg->map(fd, offsetInFat, slide, this, context);		
	}
	// update slide to reflect load location			
	this->setSlide(slide);
}

void ImageLoader::mapSegments(const void* memoryImage, uint64_t imageLen, const LinkContext& context)
{
	if ( context.verboseMapping )
		dyld::log("dyld: Mapping memory %p\n", memoryImage);
	// find address range for image
	intptr_t slide = this->assignSegmentAddresses(context);
	// map in all segments
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		seg->map(memoryImage, slide, this, context);		
	}
	// update slide to reflect load location			
	this->setSlide(slide);
	// set R/W permissions on all segments at slide location
	for(ImageLoader::SegmentIterator it = this->beginSegments(); it != this->endSegments(); ++it ) {
		Segment* seg = *it;
		seg->setPermissions(context, this);		
	}
}

bool ImageLoader::allDependentLibrariesAsWhenPreBound() const
{
	return fAllLibraryChecksumsAndLoadAddressesMatch;
}


unsigned int ImageLoader::recursiveUpdateDepth(unsigned int maxDepth)
{
	// the purpose of this phase is to make the images sortable such that 
	// in a sort list of images, every image that an image depends on
	// occurs in the list before it.
	if ( fDepth == 0 ) {
		// break cycles
		fDepth = maxDepth;
		
		// get depth of dependents
		unsigned int minDependentDepth = maxDepth;
		for(unsigned int i=0; i < fLibrariesCount; ++i) {
			DependentLibrary& libInfo = fLibraries[i];
			if ( libInfo.image != NULL ) {
				unsigned int d = libInfo.image->recursiveUpdateDepth(maxDepth);
				if ( d < minDependentDepth )
					minDependentDepth = d;
			}
		}
	
		// make me less deep then all my dependents
		fDepth = minDependentDepth - 1;
	}
	
	return fDepth;
}


void ImageLoader::recursiveLoadLibraries(const LinkContext& context, const RPathChain& loaderRPaths)
{
	if ( fState < dyld_image_state_dependents_mapped ) {
		// break cycles
		fState = dyld_image_state_dependents_mapped;
		
		// get list of libraries this image needs
		fLibrariesCount = this->doGetDependentLibraryCount();
		fLibraries = new DependentLibrary[fLibrariesCount];
		bzero(fLibraries, sizeof(DependentLibrary)*fLibrariesCount);
		DependentLibraryInfo libraryInfos[fLibrariesCount]; 
		this->doGetDependentLibraries(libraryInfos);
		
		// get list of rpaths that this image adds
		std::vector<const char*> rpathsFromThisImage;
		this->getRPaths(context, rpathsFromThisImage);
		const RPathChain thisRPaths(&loaderRPaths, &rpathsFromThisImage);
		
		// try to load each
		bool canUsePrelinkingInfo = true; 
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& requiredLib = fLibraries[i];
			DependentLibraryInfo& requiredLibInfo = libraryInfos[i];
			try {
				bool depNamespace = false;
				requiredLib.image = context.loadLibrary(requiredLibInfo.name, true, depNamespace, this->getPath(), &thisRPaths);
				if ( requiredLib.image == this ) {
					// found circular reference, perhaps DYLD_LIBARY_PATH is causing this rdar://problem/3684168 
					requiredLib.image = context.loadLibrary(requiredLibInfo.name, false, depNamespace, NULL, NULL);
					if ( requiredLib.image != this )
						dyld::warn("DYLD_ setting caused circular dependency in %s\n", this->getPath());
				}
				if ( fNeverUnload )
					requiredLib.image->setNeverUnload();
				requiredLib.image->fStaticReferenceCount += 1;
				LibraryInfo actualInfo = requiredLib.image->doGetLibraryInfo();
				requiredLib.required = requiredLibInfo.required;
				requiredLib.checksumMatches = ( actualInfo.checksum == requiredLibInfo.info.checksum );
				requiredLib.isReExported = requiredLibInfo.reExported;
				if ( ! requiredLib.isReExported ) {
					requiredLib.isSubFramework = requiredLib.image->isSubframeworkOf(context, this);
					requiredLib.isReExported = requiredLib.isSubFramework || this->hasSubLibrary(context, requiredLib.image);
				}
				// check found library version is compatible
				if ( actualInfo.minVersion < requiredLibInfo.info.minVersion ) {
					dyld::throwf("Incompatible library version: %s requires version %d.%d.%d or later, but %s provides version %d.%d.%d",
							this->getShortName(), requiredLibInfo.info.minVersion >> 16, (requiredLibInfo.info.minVersion >> 8) & 0xff, requiredLibInfo.info.minVersion & 0xff,
							requiredLib.image->getShortName(), actualInfo.minVersion >> 16, (actualInfo.minVersion >> 8) & 0xff, actualInfo.minVersion & 0xff);
				}
				// prebinding for this image disabled if any dependent library changed or slid
				if ( !requiredLib.checksumMatches || (requiredLib.image->getSlide() != 0) )
					canUsePrelinkingInfo = false;
				//if ( context.verbosePrebinding ) {
				//	if ( !requiredLib.checksumMatches )
				//		fprintf(stderr, "dyld: checksum mismatch, (%u v %u) for %s referencing %s\n", 
				//			requiredLibInfo.info.checksum, actualInfo.checksum, this->getPath(), 	requiredLib.image->getPath());		
				//	if ( requiredLib.image->getSlide() != 0 )
				//		fprintf(stderr, "dyld: dependent library slid for %s referencing %s\n", this->getPath(), requiredLib.image->getPath());		
				//}
			}
			catch (const char* msg) {
				//if ( context.verbosePrebinding )
				//	fprintf(stderr, "dyld: exception during processing for %s referencing %s\n", this->getPath(), requiredLib.image->getPath());		
				if ( requiredLibInfo.required ) {
					fState = dyld_image_state_mapped;
					dyld::throwf("Library not loaded: %s\n  Referenced from: %s\n  Reason: %s", requiredLibInfo.name, this->getPath(), msg);
				}
				// ok if weak library not found
				requiredLib.image = NULL;
				canUsePrelinkingInfo = false;  // this disables all prebinding, we may want to just slam import vectors for this lib to zero
			}
		}
		fAllLibraryChecksumsAndLoadAddressesMatch = canUsePrelinkingInfo;

		// tell each to load its dependents
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& lib = fLibraries[i];
			if ( lib.image != NULL ) {	
				lib.image->recursiveLoadLibraries(context, thisRPaths);
			}
		}
		
		// do deep prebind check
		if ( fAllLibraryChecksumsAndLoadAddressesMatch ) {
			for(unsigned int i=0; i < fLibrariesCount; ++i){
				const DependentLibrary& libInfo = fLibraries[i];
				if ( libInfo.image != NULL ) {
					if ( !libInfo.image->allDependentLibrariesAsWhenPreBound() )
						fAllLibraryChecksumsAndLoadAddressesMatch = false;
				}
			}
		}
		
		// free rpaths (getRPaths() malloc'ed each string)
		for(std::vector<const char*>::iterator it=rpathsFromThisImage.begin(); it != rpathsFromThisImage.end(); ++it) {
			const char* str = *it;
			free((void*)str);
		}
		
	}
}

void ImageLoader::recursiveRebase(const LinkContext& context)
{ 
	if ( fState < dyld_image_state_rebased ) {
		// break cycles
		fState = dyld_image_state_rebased;
		
		try {
			// rebase lower level libraries first
			for(unsigned int i=0; i < fLibrariesCount; ++i){
				DependentLibrary& libInfo = fLibraries[i];
				if ( libInfo.image != NULL )
					libInfo.image->recursiveRebase(context);
			}
				
			// rebase this image
			doRebase(context);
			
			// notify
			context.notifySingle(dyld_image_state_rebased, this->machHeader(), fPath, fLastModified);
		}
		catch (const char* msg) {
			// this image is not rebased
			fState = dyld_image_state_dependents_mapped;
			throw;
		}
	}
}




void ImageLoader::recursiveBind(const LinkContext& context, bool forceLazysBound)
{
	// Normally just non-lazy pointers are bound immediately.
	// The exceptions are:
	//   1) DYLD_BIND_AT_LAUNCH will cause lazy pointers to be bound immediately
	//   2) some API's (e.g. RTLD_NOW) can cause lazy pointers to be bound immediately
	if ( fState < dyld_image_state_bound ) {
		// break cycles
		fState = dyld_image_state_bound;
	
		try {
			// bind lower level libraries first
			for(unsigned int i=0; i < fLibrariesCount; ++i){
				DependentLibrary& libInfo = fLibraries[i];
				if ( libInfo.image != NULL )
					libInfo.image->recursiveBind(context, forceLazysBound);
			}
			// bind this image
			this->doBind(context, forceLazysBound);	
			this->doUpdateMappingPermissions(context);
			// mark if lazys are also bound
			if ( forceLazysBound || this->usablePrebinding(context) )
				fAllLazyPointersBound = true;
				
			context.notifySingle(dyld_image_state_bound, this->machHeader(), fPath, fLastModified);
		}
		catch (const char* msg) {
			// restore state
			fState = dyld_image_state_rebased;
			throw;
		}
	}
}


#if IMAGE_NOTIFY_SUPPORT
void ImageLoader::recursiveImageAnnouncement(const LinkContext& context, ImageLoader**& newImages)
{
	if ( ! fAnnounced ) {
		// break cycles
		fAnnounced = true;
		
		// announce lower level libraries first
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& libInfo = fLibraries[i];
			if ( libInfo.image != NULL )
				libInfo.image->recursiveImageAnnouncement(context, newImages);
		}
		
		// add to list of images to notify about
		*newImages++ = this;
		//dyld::log("next size = %d\n", newImages.size());
		
		// remember that this image wants to be notified about other images
		 if ( this->hasImageNotification() )
			context.addImageNeedingNotification(this);
	}
}
#endif

void ImageLoader::recursiveGetDOFSections(const LinkContext& context, std::vector<DOFInfo>& dofs)
{
	if ( ! fRegisteredDOF ) {
		// break cycles
		fRegisteredDOF = true;
		
		// gather lower level libraries first
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& libInfo = fLibraries[i];
			if ( libInfo.image != NULL )
				libInfo.image->recursiveGetDOFSections(context, dofs);
		}
		this->doGetDOFSections(context, dofs);
	}
}


void ImageLoader::recursiveSpinLock(recursive_lock& rlock)
{
	// try to set image's ivar fInitializerRecursiveLock to point to this lock_info
	// keep trying until success (spin)
	while ( ! OSAtomicCompareAndSwapPtrBarrier(NULL, &rlock, (void**)&fInitializerRecursiveLock) ) {
		// if fInitializerRecursiveLock already points to a different lock_info, if it is for
		// the same thread we are on, the increment the lock count, otherwise continue to spin
		if ( (fInitializerRecursiveLock != NULL) && (fInitializerRecursiveLock->thread == rlock.thread) )
			break;
	}
	++(fInitializerRecursiveLock->count); 
}

void ImageLoader::recursiveSpinUnLock()
{
	if ( --(fInitializerRecursiveLock->count) == 0 )
		fInitializerRecursiveLock = NULL;
}


void ImageLoader::recursiveInitialization(const LinkContext& context, mach_port_t this_thread)
{
#if RECURSIVE_INITIALIZER_LOCK
	recursive_lock lock_info(this_thread);
	recursiveSpinLock(lock_info);
#else
	_spin_lock(&fInitializerLock);
#endif

	if ( fState < dyld_image_state_dependents_initialized-1 ) {
		uint8_t oldState = fState;
		// break cycles
		fState = dyld_image_state_dependents_initialized-1;

		try {
			// initialize lower level libraries first
			for(unsigned int i=0; i < fLibrariesCount; ++i){
				DependentLibrary& libInfo = fLibraries[i];
				// don't try to initialize stuff "above" me
				if ( (libInfo.image != NULL) && (libInfo.image->fDepth >= fDepth) )
					libInfo.image->recursiveInitialization(context, this_thread);
			}
			
			// record termination order
			if ( this->needsTermination() )
				context.terminationRecorder(this);
			
			// let objc know we are about to initalize this image
			fState = dyld_image_state_dependents_initialized;
			oldState = fState;
			context.notifySingle(dyld_image_state_dependents_initialized, this->machHeader(), fPath, fLastModified);

			// initialize this image
			this->doInitialization(context);
			// let anyone know we finished initalizing this image
			fState = dyld_image_state_initialized;
			oldState = fState;
			context.notifySingle(dyld_image_state_initialized, this->machHeader(), fPath, fLastModified);
		}
		catch (const char* msg) {
			// this image is not initialized
			fState = oldState;
		#if RECURSIVE_INITIALIZER_LOCK
			recursiveSpinUnLock();
		#else
			_spin_unlock(&fInitializerLock);
		#endif
			throw;
		}
	}
	
#if RECURSIVE_INITIALIZER_LOCK
	recursiveSpinUnLock();
#else
	_spin_unlock(&fInitializerLock);
#endif
}


static void printTime(const char* msg, uint64_t partTime, uint64_t totalTime)
{
	static uint64_t sUnitsPerSecond = 0;
	if ( sUnitsPerSecond == 0 ) {
		struct mach_timebase_info timeBaseInfo;
		if ( mach_timebase_info(&timeBaseInfo) == KERN_SUCCESS ) {
			sUnitsPerSecond = 1000000000ULL * timeBaseInfo.denom / timeBaseInfo.numer;
		}
	}
	if ( partTime < sUnitsPerSecond ) {
		uint32_t milliSecondsTimeTen = (partTime*10000)/sUnitsPerSecond;
		uint32_t milliSeconds = milliSecondsTimeTen/10;
		uint32_t percentTimesTen = (partTime*1000)/totalTime;
		uint32_t percent = percentTimesTen/10;
		dyld::log("%s: %u.%u milliseconds (%u.%u%%)\n", msg, milliSeconds, milliSecondsTimeTen-milliSeconds*10, percent, percentTimesTen-percent*10);
	}
	else {
		uint32_t secondsTimeTen = (partTime*10)/sUnitsPerSecond;
		uint32_t seconds = secondsTimeTen/10;
		uint32_t percentTimesTen = (partTime*1000)/totalTime;
		uint32_t percent = percentTimesTen/10;
		dyld::log("%s: %u.%u seconds (%u.%u%%)\n", msg, seconds, secondsTimeTen-seconds*10, percent, percentTimesTen-percent*10);
	}
}

static char* commatize(uint64_t in, char* out)
{
	uint64_t div10 = in / 10;
	uint8_t delta = in - div10*10;
	char* s = &out[32];
	int digitCount = 1;
	*s = '\0';
	*(--s) = '0' + delta;
	in = div10;
	while ( in != 0 ) {
		if ( (digitCount % 3) == 0 )
			*(--s) = ',';
		div10 = in / 10;
		delta = in - div10*10;
		*(--s) = '0' + delta;
		in = div10;
		++digitCount;
	}
	return s;
}



void ImageLoader::printStatistics(unsigned int imageCount)
{
	uint64_t totalTime = fgTotalLoadLibrariesTime + fgTotalRebaseTime + fgTotalBindTime + fgTotalInitTime;
	char commaNum1[40];
	char commaNum2[40];

	printTime("total time", totalTime, totalTime);
	dyld::log("total images loaded:  %d (%u from dyld shared cache, %u needed no fixups)\n", imageCount, fgImagesUsedFromSharedCache, fgImagesRequiringNoFixups);
	dyld::log("total segments mapped: %u, into %llu pages with %llu pages pre-fetched\n", fgTotalSegmentsMapped, fgTotalBytesMapped/4096, fgTotalBytesPreFetched/4096);
	printTime("total images loading time", fgTotalLoadLibrariesTime, totalTime);
	dyld::log("total rebase fixups:  %s\n", commatize(fgTotalRebaseFixups, commaNum1));
	printTime("total rebase fixups time", fgTotalRebaseTime, totalTime);
	dyld::log("total binding fixups: %s\n", commatize(fgTotalBindFixups, commaNum1));
	if ( fgTotalBindSymbolsResolved != 0 ) {
		uint32_t avgTimesTen = (fgTotalBindImageSearches * 10) / fgTotalBindSymbolsResolved;
		uint32_t avgInt = fgTotalBindImageSearches / fgTotalBindSymbolsResolved;
		uint32_t avgTenths = avgTimesTen - (avgInt*10);
		dyld::log("total binding symbol lookups: %s, average images searched per symbol: %u.%u\n", 
				commatize(fgTotalBindSymbolsResolved, commaNum1), avgInt, avgTenths);
	}
	printTime("total binding fixups time", fgTotalBindTime, totalTime);
	dyld::log("total bindings lazily fixed up: %s of %s\n", commatize(fgTotalLazyBindFixups, commaNum1), commatize(fgTotalPossibleLazyBindFixups, commaNum2));
	printTime("total init time time", fgTotalInitTime, totalTime);
}


//
// copy path and add suffix to result
//
//  /path/foo.dylib		_debug   =>   /path/foo_debug.dylib	
//  foo.dylib			_debug   =>   foo_debug.dylib
//  foo     			_debug   =>   foo_debug
//  /path/bar			_debug   =>   /path/bar_debug  
//  /path/bar.A.dylib   _debug   =>   /path/bar.A_debug.dylib
//
void ImageLoader::addSuffix(const char* path, const char* suffix, char* result)
{
	strcpy(result, path);
	
	char* start = strrchr(result, '/');
	if ( start != NULL )
		start++;
	else
		start = result;
		
	char* dot = strrchr(start, '.');
	if ( dot != NULL ) {
		strcpy(dot, suffix);
		strcat(&dot[strlen(suffix)], &path[dot-result]);
	}
	else {
		strcat(result, suffix);
	}
}


void Segment::map(int fd, uint64_t offsetInFatWrapper, intptr_t slide, const ImageLoader* image, const ImageLoader::LinkContext& context)
{
	vm_offset_t fileOffset = this->getFileOffset() + offsetInFatWrapper;
	vm_size_t size = this->getFileSize();
	void* requestedLoadAddress = (void*)(this->getPreferredLoadAddress() + slide);
	int protection = 0;
	if ( !this->unaccessible() ) {
		if ( this->executable() )
			protection   |= PROT_EXEC;
		if ( this->readable() )
			protection   |= PROT_READ;
		if ( this->writeable() )
			protection   |= PROT_WRITE;
	}
#if __i386__
	// initially map __IMPORT segments R/W so dyld can update them
	if ( this->readOnlyImportStubs() )
		protection |= PROT_WRITE;
#endif
	// wholly zero-fill segments have nothing to mmap() in
	if ( size > 0 ) {
		void* loadAddress = mmap(requestedLoadAddress, size, protection, MAP_FIXED | MAP_PRIVATE, fd, fileOffset);
		if ( loadAddress == ((void*)(-1)) )
			dyld::throwf("mmap() error %d at address=0x%08lX, size=0x%08lX segment=%s in Segment::map() mapping %s", errno, (uintptr_t)requestedLoadAddress, (uintptr_t)size, this->getName(), image->getPath());
	}
	// update stats
	++ImageLoader::fgTotalSegmentsMapped;
	ImageLoader::fgTotalBytesMapped += size;
	if ( context.verboseMapping )
		dyld::log("%18s at %p->%p with permissions %c%c%c\n", this->getName(), requestedLoadAddress, (char*)requestedLoadAddress+this->getFileSize()-1,
			(protection & PROT_READ) ? 'r' : '.',  (protection & PROT_WRITE) ? 'w' : '.',  (protection & PROT_EXEC) ? 'x' : '.' );
}

void Segment::map(const void* memoryImage, intptr_t slide, const ImageLoader* image, const ImageLoader::LinkContext& context)
{
	vm_address_t loadAddress = this->getPreferredLoadAddress() + slide;
	vm_address_t srcAddr = (uintptr_t)memoryImage + this->getFileOffset();
	vm_size_t size = this->getFileSize();
	kern_return_t r = vm_copy(mach_task_self(), srcAddr, size, loadAddress);
	if ( r != KERN_SUCCESS ) 
		throw "can't map segment";
		
	if ( context.verboseMapping )
		dyld::log("%18s at %p->%p\n", this->getName(), (char*)loadAddress, (char*)loadAddress+this->getFileSize()-1);
}

void Segment::setPermissions(const ImageLoader::LinkContext& context, const ImageLoader* image)
{
	vm_prot_t protection = 0;
	if ( !this->unaccessible() ) {
		if ( this->executable() )
			protection   |= VM_PROT_EXECUTE;
		if ( this->readable() )
			protection   |= VM_PROT_READ;
		if ( this->writeable() )
			protection   |= VM_PROT_WRITE;
	}
	vm_address_t addr = this->getActualLoadAddress(image);
	vm_size_t size = this->getSize();
	const bool setCurrentPermissions = false;
	kern_return_t r = vm_protect(mach_task_self(), addr, size, setCurrentPermissions, protection);
	if ( r != KERN_SUCCESS ) 
		throw "can't set vm permissions for mapped segment";
	if ( context.verboseMapping ) {
		dyld::log("%18s at %p->%p altered permissions to %c%c%c\n", this->getName(), (char*)addr, (char*)addr+this->getFileSize()-1,
			(protection & PROT_READ) ? 'r' : '.',  (protection & PROT_WRITE) ? 'w' : '.',  (protection & PROT_EXEC) ? 'x' : '.' );
	}
}	

void Segment::tempWritable(const ImageLoader::LinkContext& context, const ImageLoader* image)
{
	vm_address_t addr = this->getActualLoadAddress(image);
	vm_size_t size = this->getSize();
	const bool setCurrentPermissions = false;
	vm_prot_t protection = VM_PROT_WRITE | VM_PROT_READ;
	if ( this->executable() )
		protection |= VM_PROT_EXECUTE;
	kern_return_t r = vm_protect(mach_task_self(), addr, size, setCurrentPermissions, protection);
	if ( r != KERN_SUCCESS ) 
		throw "can't set vm permissions for mapped segment";
	if ( context.verboseMapping ) {
		dyld::log("%18s at %p->%p altered permissions to %c%c%c\n", this->getName(), (char*)addr, (char*)addr+this->getFileSize()-1,
			(protection & PROT_READ) ? 'r' : '.',  (protection & PROT_WRITE) ? 'w' : '.',  (protection & PROT_EXEC) ? 'x' : '.' );
	}
}


bool Segment::hasTrailingZeroFill()
{
	return ( this->writeable() && (this->getSize() > this->getFileSize()) );
}


uintptr_t Segment::reserveAnAddressRange(size_t length, const ImageLoader::LinkContext& context)
{
	vm_address_t addr = 0;
	vm_size_t size = length;
	// in PIE programs, load initial dylibs after main executable so they don't have fixed addresses either
	if ( fgNextPIEDylibAddress != 0 ) {
		addr = fgNextPIEDylibAddress + (arc4random() & 0x3) * 4096; // add small random padding between dylibs
		kern_return_t r = vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_FIXED);
		if ( r == KERN_SUCCESS ) {
			fgNextPIEDylibAddress = addr + size;
			return addr;
		}
		fgNextPIEDylibAddress = 0;
	}
	kern_return_t r = vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_ANYWHERE);
	if ( r != KERN_SUCCESS ) 
		throw "out of address space";
	
	return addr;
}

bool Segment::reserveAddressRange(uintptr_t start, size_t length)
{
	vm_address_t addr = start;
	vm_size_t size = length;
	kern_return_t r = vm_allocate(mach_task_self(), &addr, size, false /*only this range*/);
	if ( r != KERN_SUCCESS ) 
		return false;
	return true;
}



