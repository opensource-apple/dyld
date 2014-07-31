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

#include "ImageLoader.h"


uint32_t								ImageLoader::fgImagesWithUsedPrebinding = 0;
uint32_t								ImageLoader::fgTotalRebaseFixups = 0;
uint32_t								ImageLoader::fgTotalBindFixups = 0;
uint32_t								ImageLoader::fgTotalLazyBindFixups = 0;
uint32_t								ImageLoader::fgTotalPossibleLazyBindFixups = 0;
uint64_t								ImageLoader::fgTotalLoadLibrariesTime;
uint64_t								ImageLoader::fgTotalRebaseTime;
uint64_t								ImageLoader::fgTotalBindTime;
uint64_t								ImageLoader::fgTotalNotifyTime;
uint64_t								ImageLoader::fgTotalInitTime;
uintptr_t								ImageLoader::fgNextSplitSegAddress = 0x90000000;
uintptr_t								Segment::fgNextNonSplitSegAddress = 0x8F000000;



__attribute__((noreturn))
void throwf(const char* format, ...) 
{
	va_list	list;
	char*	p;
	va_start(list, format);
	vasprintf(&p, format, list);
	va_end(list);
	
	const char*	t = p;
	throw t;
}

void ImageLoader::init(const char* path, uint64_t offsetInFat, dev_t device, ino_t inode, time_t modDate)
{
	fPathHash = 0;
	fPath = NULL;
	if ( path != NULL )
		this->setPath(path);
	fLogicalPath = NULL;
	fDevice = device;
	fInode = inode;
	fLastModified = modDate;
	fOffsetInFatFile = offsetInFat;
	//fSegments = NULL;
	fLibraries = NULL;
	fLibrariesCount = 0;
	fReferenceCount = 0;
	fAllLibraryChecksumsAndLoadAddressesMatch = false;
	fLeaveMapped = false;
	fHideSymbols = false;
	fMatchByInstallName = false;
	fLibrariesLoaded = false;
	fBased = false;
	fBoundAllNonLazy = false;
	fBoundAllLazy = false;
	fAnnounced = false;
	fInitialized = false;
	fNextAddImageIndex = 0;
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
	// need to read up on STL and see if this is right way to destruct vector contents
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		delete seg;
	}
	if ( fPath != NULL ) 
		delete [] fPath;
	if ( fLogicalPath != NULL ) 
		delete [] fLogicalPath;
}

			
void ImageLoader::setPath(const char* path)
{
	if ( fPath != NULL ) {
		// if duplicate path, do nothing
		if ( strcmp(path, fPath) == 0 )
			return;
		delete [] fPath;
	}
	fPath = new char[strlen(path)+1];
	strcpy((char*)fPath, path);
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
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i){
		fSegments[i]->setUnMapWhenDestructed(false);
	}
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
	return fBoundAllNonLazy;
}

time_t ImageLoader::lastModified()
{
	return fLastModified;
}

bool ImageLoader::containsAddress(const void* addr) const
{
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		const uint8_t* start = (const uint8_t*)seg->getActualLoadAddress();
		const uint8_t* end = start + seg->getSize();
		if ( (start <= addr) && (addr < end) && !seg->unaccessible() )
			return true;
	}
	return false;
}

void ImageLoader::addMappedRegions(RegionsVector& regions) const
{
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		MappedRegion region;
		region.address = seg->getActualLoadAddress();
		region.size = seg->getSize();
		regions.push_back(region);
	}
}


void ImageLoader::incrementReferenceCount()
{
	++fReferenceCount;
}

bool ImageLoader::decrementReferenceCount()
{
	return ( --fReferenceCount == 0 );
}

// private method that handles circular dependencies by only search any image once
const ImageLoader::Symbol* ImageLoader::findExportedSymbolInDependentImagesExcept(const char* name, std::set<const ImageLoader*>& dontSearchImages, ImageLoader** foundIn) const
{
	const ImageLoader::Symbol* sym;
	// search self
	if ( dontSearchImages.count(this) == 0 ) {
		sym = this->findExportedSymbol(name, NULL, false, foundIn);
		if ( sym != NULL )
			return sym;
		dontSearchImages.insert(this);
	}

	// search directly dependent libraries
	for (uint32_t i=0; i < fLibrariesCount; ++i) {
		ImageLoader* dependentImage = fLibraries[i].image;
		if ( (dependentImage != NULL) && (dontSearchImages.count(dependentImage) == 0) ) {
			const ImageLoader::Symbol* sym = dependentImage->findExportedSymbol(name, NULL, false, foundIn);
			if ( sym != NULL )
				return sym;
		}
	}
	
	// search indirectly dependent libraries
	for (uint32_t i=0; i < fLibrariesCount; ++i) {
		ImageLoader* dependentImage = fLibraries[i].image;
		if ( (dependentImage != NULL) && (dontSearchImages.count(dependentImage) == 0) ) {
			const ImageLoader::Symbol* sym = dependentImage->findExportedSymbolInDependentImagesExcept(name, dontSearchImages, foundIn);
			if ( sym != NULL )
				return sym;
			dontSearchImages.insert(dependentImage);
		}
	}

	return NULL;
}


const ImageLoader::Symbol* ImageLoader::findExportedSymbolInDependentImages(const char* name, ImageLoader** foundIn) const
{
	std::set<const ImageLoader*> dontSearchImages;
	dontSearchImages.insert(this);	// don't search this image
	return this->findExportedSymbolInDependentImagesExcept(name, dontSearchImages, foundIn);
}

const ImageLoader::Symbol* ImageLoader::findExportedSymbolInImageOrDependentImages(const char* name, ImageLoader** foundIn) const
{
	std::set<const ImageLoader*> dontSearchImages;
	return this->findExportedSymbolInDependentImagesExcept(name, dontSearchImages, foundIn);
}


void ImageLoader::link(const LinkContext& context, BindingLaziness bindness, InitializerRunning inits, uint32_t notifyCount)
{
	uint64_t t1 = mach_absolute_time();
	this->recursiveLoadLibraries(context);
	
	uint64_t t2 = mach_absolute_time();
 	this->recursiveRebase(context);
	
	uint64_t t3 = mach_absolute_time();
 	this->recursiveBind(context, bindness);
	
	uint64_t t4 = mach_absolute_time();	
	this->recursiveImageNotification(context, notifyCount);	
	
	if ( (inits == kRunInitializers) || (inits == kDontRunInitializersButTellObjc) ) {
		std::vector<ImageLoader*>	newImages;
		this->recursiveImageAnnouncement(context, newImages);	 // build bottom up list images being added
		context.notifyAdding(newImages);	// tell gdb or anyone who cares about these
	}
	
	uint64_t t5 = mach_absolute_time();
	if ( inits == kRunInitializers ) {
		this->recursiveInitialization(context);
		uint64_t t6 = mach_absolute_time();
		fgTotalInitTime += t6 - t5;
	}
	fgTotalLoadLibrariesTime += t2 - t1;
	fgTotalRebaseTime += t3 - t2;
	fgTotalBindTime += t4 - t3;
	fgTotalNotifyTime += t5 - t4;
}


// only called pre-main on main executable
// if crt.c is ever cleaned up, this could go away
void ImageLoader::runInitializers(const LinkContext& context)
{
		std::vector<ImageLoader*>	newImages;
		this->recursiveImageAnnouncement(context, newImages);	 // build bottom up list images being added
		context.notifyAdding(newImages);	// tell gdb or anyone who cares about these

	this->recursiveInitialization(context);
}

// called inside _dyld_register_func_for_add_image()
void ImageLoader::runNotification(const LinkContext& context, uint32_t notifyCount)
{
	this->recursiveImageNotification(context, notifyCount);
}


intptr_t ImageLoader::assignSegmentAddresses(const LinkContext& context)
{
	// preflight and calculate slide if needed
	const unsigned int segmentCount = fSegments.size();
	intptr_t slide = 0;
	if ( this->segmentsCanSlide() && this->segmentsMustSlideTogether() ) {
		bool needsToSlide = false;
		uintptr_t lowAddr = UINTPTR_MAX;
		uintptr_t highAddr = 0;
		for(unsigned int i=0; i < segmentCount; ++i){
			Segment* seg = fSegments[i];
			const uintptr_t segLow = seg->getPreferredLoadAddress();
			const uintptr_t segHigh = segLow + seg->getSize();
			if ( segLow < lowAddr )
				lowAddr = segLow;
			if ( segHigh > highAddr )
				highAddr = segHigh;
				
			if ( context.slideAndPackDylibs || !seg->hasPreferredLoadAddress() || !Segment::reserveAddressRange(seg->getPreferredLoadAddress(), seg->getSize()) )
				needsToSlide = true;
		}
		if ( needsToSlide ) {
			// find a chunk of address space to hold all segments
			uintptr_t addr = Segment::reserveAnAddressRange(highAddr-lowAddr, context);
			slide = addr - lowAddr;
		}
	} 
	else if ( ! this->segmentsCanSlide() ) {
		for(unsigned int i=0; i < segmentCount; ++i){
			Segment* seg = fSegments[i];
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
		fprintf(stderr, "dyld: Mapping %s\n", this->getPath());
	// find address range for image
	intptr_t slide = this->assignSegmentAddresses(context);
	// map in all segments
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		seg->map(fd, offsetInFat, slide, context);		
	}
	// update slide to reflect load location			
	this->setSlide(slide);
	
	// now that it is mapped and slide is set, mark that we should unmap it when done
	for(unsigned int i=0; i < segmentCount; ++i){
		fSegments[i]->setUnMapWhenDestructed(true);
	}
}

void ImageLoader::mapSegments(const void* memoryImage, uint64_t imageLen, const LinkContext& context)
{
	if ( context.verboseMapping )
		fprintf(stderr, "dyld: Mapping memory %p\n", memoryImage);
	// find address range for image
	intptr_t slide = this->assignSegmentAddresses(context);
	// map in all segments
	const unsigned int segmentCount = fSegments.size();
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		seg->map(memoryImage, slide, context);		
	}
	// update slide to reflect load location			
	this->setSlide(slide);
	// set R/W permissions on all segments at slide location
	for(unsigned int i=0; i < segmentCount; ++i){
		Segment* seg = fSegments[i];
		seg->setPermissions();		
	}
}

bool ImageLoader::allDependentLibrariesAsWhenPreBound() const
{
	return fAllLibraryChecksumsAndLoadAddressesMatch;
}


void ImageLoader::recursiveLoadLibraries(const LinkContext& context)
{
	if ( ! fLibrariesLoaded ) {
		// break cycles
		fLibrariesLoaded = true;
		
		// get list of libraries this image needs
		fLibrariesCount = this->doGetDependentLibraryCount();
		fLibraries = new DependentLibrary[fLibrariesCount];
		this->doGetDependentLibraries(fLibraries);
		
		// try to load each
		bool canUsePrelinkingInfo = true; 
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& requiredLib = fLibraries[i];
			try {
				requiredLib.image = context.loadLibrary(requiredLib.name, true, this->getPath(), NULL);
				if ( requiredLib.image == this ) {
					// found circular reference, perhaps DYLD_LIBARY_PATH is causing this rdar://problem/3684168 
					requiredLib.image = context.loadLibrary(requiredLib.name, false, NULL, NULL);
					if ( requiredLib.image != this )
						fprintf(stderr, "dyld: warning DYLD_ setting caused circular dependency in %s\n", this->getPath());
				}
				LibraryInfo actualInfo = requiredLib.image->doGetLibraryInfo();
				requiredLib.checksumMatches = ( actualInfo.checksum == requiredLib.info.checksum );
				// check found library version is compatible
				if ( actualInfo.minVersion < requiredLib.info.minVersion ) {
					throwf("Incompatible library version: %s requires version %d.%d.%d or later, but %s provides version %d.%d.%d",
							this->getShortName(), requiredLib.info.minVersion >> 16, (requiredLib.info.minVersion >> 8) & 0xff, requiredLib.info.minVersion & 0xff,
							requiredLib.image->getShortName(), actualInfo.minVersion >> 16, (actualInfo.minVersion >> 8) & 0xff, actualInfo.minVersion & 0xff);
				}
				// prebinding for this image disabled if any dependent library changed or slid
				if ( !requiredLib.checksumMatches || (requiredLib.image->getSlide() != 0) )
					canUsePrelinkingInfo = false;
				//if ( context.verbosePrebinding ) {
				//	if ( !requiredLib.checksumMatches )
				//		fprintf(stderr, "dyld: checksum mismatch, (%lld v %lld) for %s referencing %s\n", requiredLib.info.checksum, actualInfo.checksum, this->getPath(), 	requiredLib.image->getPath());		
				//	if ( requiredLib.image->getSlide() != 0 )
				//		fprintf(stderr, "dyld: dependent library slid for %s referencing %s\n", this->getPath(), requiredLib.image->getPath());		
				//}
			}
			catch (const char* msg) {
				//if ( context.verbosePrebinding )
				//	fprintf(stderr, "dyld: exception during processing for %s referencing %s\n", this->getPath(), requiredLib.image->getPath());		
				if ( requiredLib.required ) {
					const char* formatString = "Library not loaded: %s\n  Referenced from: %s\n  Reason: %s";
					const char* referencedFrom = this->getPath();
					char buf[strlen(requiredLib.name)+strlen(referencedFrom)+strlen(formatString)+strlen(msg)+2];
					sprintf(buf, formatString, requiredLib.name, referencedFrom, msg);
					fLibrariesLoaded = false;
					throw strdup(buf);  // this is a leak if exception doesn't halt program
				}
				// ok if weak library not found
				requiredLib.image = NULL;
				canUsePrelinkingInfo = false;  // this disables all prebinding, we may want to just slam import vectors for this lib to zero
			}
		}
		fAllLibraryChecksumsAndLoadAddressesMatch = canUsePrelinkingInfo;

		// tell each to load its dependents
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& libInfo = fLibraries[i];
			if ( libInfo.image != NULL ) {	
				libInfo.isSubFramework = libInfo.image->isSubframeworkOf(context, this);
				libInfo.isReExported = libInfo.isSubFramework || this->hasSubLibrary(context, libInfo.image);
				//if ( libInfo.isReExported  )
				//	fprintf(stderr, "%s re-exports %s\n", strrchr(this->getPath(), '/'), strrchr(libInfo.image->getPath(),'/'));
				libInfo.image->recursiveLoadLibraries(context);
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
		
	}
}

void ImageLoader::recursiveRebase(const LinkContext& context)
{ 
	if ( ! fBased ) {
		// break cycles
		fBased = true;
		
		try {
			// rebase lower level libraries first
			for(unsigned int i=0; i < fLibrariesCount; ++i){
				DependentLibrary& libInfo = fLibraries[i];
				if ( libInfo.image != NULL )
					libInfo.image->recursiveRebase(context);
			}
				
			// rebase this image
			doRebase(context);
		}
		catch (const char* msg) {
			// this image is not rebased
			fBased = false;
			throw msg;
		}
	}
}




void ImageLoader::recursiveBind(const LinkContext& context, BindingLaziness bindness)
{
	// normally just non-lazy pointers are bound up front,
	// but DYLD_BIND_AT_LAUNCH will cause lazy pointers to be bound up from
	// and some dyld API's bind all lazys at runtime
	bool nonLazy = false;
	bool lazy = false;
	switch( bindness ) {
		case kNonLazyOnly:
			nonLazy = true;
			break;
		case kLazyAndNonLazy:
			nonLazy = true;
			lazy = true;
			break;
		case kLazyOnly:
		case kLazyOnlyNoDependents:
			lazy = true;
			break;
	}
	const bool doNonLazy = nonLazy && !fBoundAllNonLazy;
	const bool doLazy = lazy && !fBoundAllLazy;
	if ( doNonLazy || doLazy ) {
		// break cycles
		bool oldBoundAllNonLazy = fBoundAllNonLazy;
		bool oldBoundAllLazy = fBoundAllLazy;
		fBoundAllNonLazy = fBoundAllNonLazy || nonLazy;
		fBoundAllLazy = fBoundAllLazy || lazy;
		
		try {
			// bind lower level libraries first
			if ( bindness != kLazyOnlyNoDependents ) {
				for(unsigned int i=0; i < fLibrariesCount; ++i){
					DependentLibrary& libInfo = fLibraries[i];
					if ( libInfo.image != NULL )
						libInfo.image->recursiveBind(context, bindness);
				}
			}
			// bind this image
			if ( doLazy && !doNonLazy )
				doBind(context, kLazyOnly);	
			else if ( !doLazy && doNonLazy )
				doBind(context, kNonLazyOnly);
			else 
				doBind(context, kLazyAndNonLazy);
		}
		catch (const char* msg) {
			// restore state
			fBoundAllNonLazy = oldBoundAllNonLazy;
			fBoundAllLazy = oldBoundAllLazy;
			throw msg;
		}
	}
}

//
// This is complex because _dyld_register_func_for_add_image() is defined to not only
// notify you of future image loads, but also of all currently loaded images.  Therefore
// each image needs to track that all add-image-funcs have been notified about it.
// Since add-image-funcs cannot be removed, each has a unique index and each image
// records the thru which index notificiation has already been done.
//
void ImageLoader::recursiveImageNotification(const LinkContext& context, uint32_t addImageCount)
{
	if ( fNextAddImageIndex < addImageCount ) {
		// break cycles
		const uint32_t initIndex = fNextAddImageIndex;
		fNextAddImageIndex = addImageCount;
	
		// notify all requestors about this image
		context.imageNotification(this, initIndex);
	
		// notify about lower level libraries first
		for(unsigned int i=0; i < fLibrariesCount; ++i){
			DependentLibrary& libInfo = fLibraries[i];
			if ( libInfo.image != NULL )
				libInfo.image->recursiveImageNotification(context, addImageCount);
		}
	}
}


void ImageLoader::recursiveImageAnnouncement(const LinkContext& context, std::vector<ImageLoader*>& newImages)
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
		
		// add to list of images to notify gdb about
		newImages.push_back(this);
		//fprintf(stderr, "next size = %d\n", newImages.size());
		
		// remember that this image wants to be notified about other images
		 if ( this->hasImageNotification() )
			context.addImageNeedingNotification(this);
	}
}



void ImageLoader::recursiveInitialization(const LinkContext& context)
{
	if ( ! fInitialized ) {
		// break cycles
		fInitialized = true;

		try {
			// initialize lower level libraries first
			for(unsigned int i=0; i < fLibrariesCount; ++i){
				DependentLibrary& libInfo = fLibraries[i];
				if ( libInfo.image != NULL )
					libInfo.image->recursiveInitialization(context);
			}
			
			// record termination order
			if ( this->needsTermination() )
				context.terminationRecorder(this);
			
			// initialize this image
			this->doInitialization(context);
		}
		catch (const char* msg) {
			// this image is not initialized
			fInitialized = false;
			throw msg;
		}
	}
}

void ImageLoader::reprebindCommit(const LinkContext& context, bool commit, bool unmapOld)
{
	// do nothing on unprebound images
	if ( ! this->isPrebindable() )
		return;

	// do nothing if prebinding is up to date
	if ( this->usablePrebinding(context) )
		return;
		
	// make sure we are not replacing a symlink with a file
	char realFilePath[PATH_MAX];
	if ( realpath(this->getPath(), realFilePath) == NULL ) {
		throwf("realpath() failed on %s, errno=%d", this->getPath(), errno);
	}
	// recreate temp file name
	char tempFilePath[PATH_MAX];
	char suffix[64];
	sprintf(suffix, "_redoprebinding%d", getpid());
	ImageLoader::addSuffix(realFilePath, suffix, tempFilePath);

	if ( commit ) {
		// all files successfully reprebound, so do swap
		int result = rename(tempFilePath, realFilePath);
		if ( result != 0 ) {
			// if there are two dylibs with the same install path, the second will fail to prebind
			// because the _redoprebinding temp file is gone.  In that case, log and go on.
			if ( errno == ENOENT )
				fprintf(stderr, "update_prebinding: temp file missing: %s\n", tempFilePath);
			else
				throwf("can't swap temporary re-prebound file: rename(%s,%s) returned errno=%d", tempFilePath, realFilePath, errno);
		}
		else if ( unmapOld ) {
			this->prebindUnmap(context);
		}
	}
	else {
		// something went wrong during prebinding, delete the temp files
		unlink(tempFilePath);
	}
}

uint64_t ImageLoader::reprebind(const LinkContext& context, time_t timestamp)
{
	// do nothing on unprebound images
	if ( ! this->isPrebindable() )
		return INT64_MAX;

	// do nothing if prebinding is up to date
	if ( this->usablePrebinding(context) ) {
		if ( context.verbosePrebinding )
			fprintf(stderr, "dyld: no need to re-prebind: %s\n", this->getPath());
		return INT64_MAX;
	}
	// recreate temp file name
	char realFilePath[PATH_MAX];
	if ( realpath(this->getPath(), realFilePath) == NULL ) {
		throwf("realpath() failed on %s, errno=%d", this->getPath(), errno);
	}
	char tempFilePath[PATH_MAX];
	char suffix[64];
	sprintf(suffix, "_redoprebinding%d", getpid());
	ImageLoader::addSuffix(realFilePath, suffix, tempFilePath);

	// make copy of file and map it in
	uint8_t* fileToPrebind;
	uint64_t fileToPrebindSize;
	uint64_t freespace = this->copyAndMap(tempFilePath, &fileToPrebind, &fileToPrebindSize);

	// do format specific prebinding
	this->doPrebinding(context, timestamp, fileToPrebind);
	
	// flush and swap files
	int result = msync(fileToPrebind, fileToPrebindSize, MS_ASYNC);
	if ( result != 0 )
		throw "error syncing re-prebound file";
	result = munmap(fileToPrebind, fileToPrebindSize);
	if ( result != 0 )
		throw "error unmapping re-prebound file";

	// log
	if ( context.verbosePrebinding )
		fprintf(stderr, "dyld: re-prebound: %p %s\n", this->machHeader(), this->getPath());
	
	return freespace;
}

uint64_t ImageLoader::copyAndMap(const char* tempFile, uint8_t** fileToPrebind, uint64_t* fileToPrebindSize) 
{
	// reopen dylib 
	int src = open(this->getPath(), O_RDONLY);	
	if ( src == -1 )
		throw "can't open image";
	struct stat stat_buf;
	if ( fstat(src, &stat_buf) == -1)
		throw "can't stat image";
	if ( stat_buf.st_mtime != fLastModified )
		throw "image file changed since it was loaded";
		
	// create new file with all same permissions to hold copy of dylib 
	unlink(tempFile);
	int dst = open(tempFile, O_CREAT | O_RDWR | O_TRUNC, stat_buf.st_mode);	
	if ( dst == -1 )
		throw "can't create temp image";

	// mark source as "don't cache"
	(void)fcntl(src, F_NOCACHE, 1);
	// we want to cache the dst because we are about to map it in and modify it
	
	// copy permission bits
	if ( chmod(tempFile, stat_buf.st_mode & 07777) == -1 )
		throwf("can't chmod temp image.  errno=%d for %s", errno, this->getPath());
	if ( chown(tempFile, stat_buf.st_uid, stat_buf.st_gid) == -1)
		throwf("can't chown temp image.  errno=%d for %s", errno, this->getPath());
		  
	// copy contents
	ssize_t len;
	const uint32_t kBufferSize = 128*1024;
	static uint8_t* buffer = NULL;
	if ( buffer == NULL ) {
		vm_address_t addr = 0;
		if ( vm_allocate(mach_task_self(), &addr, kBufferSize, true /*find range*/) == KERN_SUCCESS )
			buffer = (uint8_t*)addr;
		else
			throw "can't allcoate copy buffer";
	}
	while ( (len = read(src, buffer, kBufferSize)) > 0 ) {
		if ( write(dst, buffer, len) == -1 )
			throwf("write failure copying dylib errno=%d for %s", errno, this->getPath());
	}
	
	// map in dst file
	*fileToPrebindSize = stat_buf.st_size - fOffsetInFatFile; // this may map in too much, but it does not matter
	*fileToPrebind = (uint8_t*)mmap(NULL, *fileToPrebindSize, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, dst, fOffsetInFatFile);
	if ( *fileToPrebind == (uint8_t*)(-1) )
		throw "can't mmap temp image";
		
	// get free space remaining on dst volume
	struct statfs statfs_buf;
	if ( fstatfs(dst, &statfs_buf) != 0 )
		throwf("can't fstatfs(), errno=%d for %s", errno, tempFile);
	uint64_t freespace = (uint64_t)statfs_buf.f_bavail * (uint64_t)statfs_buf.f_bsize;
	
	// closing notes:  
	//		ok to close file after mapped in
	//		ok to throw above without closing file because the throw will terminate update_prebinding
	int result1 = close(dst);
	int result2 = close(src);
	if ( (result1 != 0) || (result2 != 0) )
		throw "can't close file";
		
	return freespace;
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
		fprintf(stderr, "%s: %u.%u milliseconds (%u.%u%%)\n", msg, milliSeconds, milliSecondsTimeTen-milliSeconds*10, percent, percentTimesTen-percent*10);
	}
	else {
		uint32_t secondsTimeTen = (partTime*10)/sUnitsPerSecond;
		uint32_t seconds = secondsTimeTen/10;
		uint32_t percentTimesTen = (partTime*1000)/totalTime;
		uint32_t percent = percentTimesTen/10;
		fprintf(stderr, "%s: %u.%u seconds (%u.%u%%)\n", msg, seconds, secondsTimeTen-seconds*10, percent, percentTimesTen-percent*10);
	}
}

static char* commatize(uint64_t in, char* out)
{
	char* result = out;
	char rawNum[30];
	sprintf(rawNum, "%llu", in);
	const int rawNumLen = strlen(rawNum);
	for(int i=0; i < rawNumLen-1; ++i) {
		*out++ = rawNum[i];
		if ( ((rawNumLen-i) % 3) == 1 )
			*out++ = ',';
	}
	*out++ = rawNum[rawNumLen-1];
	*out = '\0';
	return result;
} 


void ImageLoader::printStatistics(unsigned int imageCount)
{
	uint64_t totalTime = fgTotalLoadLibrariesTime + fgTotalRebaseTime + fgTotalBindTime + fgTotalNotifyTime + fgTotalInitTime;
	char commaNum1[40];
	char commaNum2[40];

	printTime("total time", totalTime, totalTime);
	fprintf(stderr, "total images loaded:  %d (%d used prebinding)\n", imageCount, fgImagesWithUsedPrebinding);
	printTime("total images loading time", fgTotalLoadLibrariesTime, totalTime);
	fprintf(stderr, "total rebase fixups:  %s\n", commatize(fgTotalRebaseFixups, commaNum1));
	printTime("total rebase fixups time", fgTotalRebaseTime, totalTime);
	fprintf(stderr, "total binding fixups: %s\n", commatize(fgTotalBindFixups, commaNum1));
	printTime("total binding fixups time", fgTotalBindTime, totalTime);
	fprintf(stderr, "total bindings lazily fixed up: %s of %s\n", commatize(fgTotalLazyBindFixups, commaNum1), commatize(fgTotalPossibleLazyBindFixups, commaNum2));
	printTime("total notify time time", fgTotalNotifyTime, totalTime);
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


void Segment::map(int fd, uint64_t offsetInFatWrapper, intptr_t slide, const ImageLoader::LinkContext& context)
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
	void* loadAddress = mmap(requestedLoadAddress, size, protection, MAP_FILE | MAP_FIXED | MAP_PRIVATE, fd, fileOffset);
	if ( loadAddress == ((void*)(-1)) )
		throwf("mmap() error %d at address=0x%08lX, size=0x%08lX in Segment::map() mapping %s", errno, (uintptr_t)requestedLoadAddress, (uintptr_t)size, this->getImage()->getPath());
	
	if ( context.verboseMapping )
		fprintf(stderr, "%18s at %p->%p\n", this->getName(), loadAddress, (char*)loadAddress+this->getFileSize()-1);
}

void Segment::map(const void* memoryImage, intptr_t slide, const ImageLoader::LinkContext& context)
{
	vm_address_t loadAddress = this->getPreferredLoadAddress() + slide;
	vm_address_t srcAddr = (uintptr_t)memoryImage + this->getFileOffset();
	vm_size_t size = this->getFileSize();
	kern_return_t r = vm_copy(mach_task_self(), srcAddr, size, loadAddress);
	if ( r != KERN_SUCCESS ) 
		throw "can't map segment";
		
	if ( context.verboseMapping )
		fprintf(stderr, "%18s at %p->%p\n", this->getName(), (char*)loadAddress, (char*)loadAddress+this->getFileSize()-1);
}

void Segment::setPermissions()
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
	vm_address_t addr = this->getActualLoadAddress();
	vm_size_t size = this->getSize();
	const bool setCurrentPermissions = false;
	kern_return_t r = vm_protect(mach_task_self(), addr, size, setCurrentPermissions, protection);
	if ( r != KERN_SUCCESS ) 
		throw "can't set vm permissions for mapped segment";
}	

void Segment::tempWritable()
{
	vm_address_t addr = this->getActualLoadAddress();
	vm_size_t size = this->getSize();
	const bool setCurrentPermissions = false;
	kern_return_t r = vm_protect(mach_task_self(), addr, size, setCurrentPermissions, VM_PROT_WRITE | VM_PROT_READ);
	if ( r != KERN_SUCCESS ) 
		throw "can't set vm permissions for mapped segment";
}


bool Segment::hasTrailingZeroFill()
{
	return ( this->writeable() && (this->getSize() > this->getFileSize()) );
}


uintptr_t Segment::reserveAnAddressRange(size_t length, const ImageLoader::LinkContext& context)
{
	vm_address_t addr = 0;
	vm_size_t size = length;
	if ( context.slideAndPackDylibs ) {
		addr = (fgNextNonSplitSegAddress - length) & (-4096); // page align
		kern_return_t r = vm_allocate(mach_task_self(), &addr, size, false /*use this range*/);
		if ( r != KERN_SUCCESS ) 
			throw "out of address space";
		fgNextNonSplitSegAddress = addr;
	}
	else {
		kern_return_t r = vm_allocate(mach_task_self(), &addr, size, true /*find range*/);
		if ( r != KERN_SUCCESS ) 
			throw "out of address space";
	}
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



