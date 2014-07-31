/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004 Apple Computer, Inc. All rights reserved.
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


#ifndef __IMAGELOADER__
#define __IMAGELOADER__

#include <sys/types.h>
#include <mach/mach_time.h> // struct mach_timebase_info
#include <stdint.h>
#include <vector>
#include <set>

#include "mach-o/dyld_gdb.h"


// utility
__attribute__((noreturn)) void throwf(const char* format, ...);

//
// ImageLoader is an abstract base class.  To support loading a particular executable
// file format, you make a concrete subclass of ImageLoader.
//
// For each executable file (dynamic shared object) in use, an ImageLoader is instantiated.
//
// The ImageLoader base class does the work of linking together images, but it knows nothing
// about any particular file format.
//
//
class ImageLoader {
public:

	typedef uint32_t DefinitionFlags;
	static const DefinitionFlags kNoDefinitionOptions = 0;
	static const DefinitionFlags kWeakDefinition = 1;
	
	typedef uint32_t ReferenceFlags;
	static const ReferenceFlags kNoReferenceOptions = 0;
	static const ReferenceFlags kWeakReference = 1;
	static const ReferenceFlags kTentativeDefinition = 2;
	

	enum BindingLaziness { kNonLazyOnly, kLazyAndNonLazy, kLazyOnly, kLazyOnlyNoDependents };
	enum InitializerRunning { kDontRunInitializers, kRunInitializers, kDontRunInitializersButTellObjc };
	enum PrebindMode { kUseAllPrebinding, kUseSplitSegPrebinding, kUseAllButAppPredbinding, kUseNoPrebinding };
	enum BindingOptions { kBindingNone, kBindingLazyPointers, kBindingNeverSetLazyPointers };
	enum SharedRegionMode { kUseSharedRegion, kUsePrivateSharedRegion, kDontUseSharedRegion };
	
	struct Symbol;  // abstact symbol

	struct MappedRegion {
		uintptr_t	address;
		size_t		size;
	};
	typedef std::vector<MappedRegion> RegionsVector;

	struct LinkContext {
		ImageLoader*	(*loadLibrary)(const char* libraryName, bool search, const char* origin, const char* rpath[]);
		uint32_t		(*imageNotification)(ImageLoader* image, uint32_t startIndex);
		void			(*terminationRecorder)(ImageLoader* image);
		bool			(*flatExportFinder)(const char* name, const Symbol** sym, ImageLoader** image);
		bool			(*coalescedExportFinder)(const char* name, const Symbol** sym, ImageLoader** image);
		void			(*undefinedHandler)(const char* name);
		void			(*addImageNeedingNotification)(ImageLoader* image);
		void			(*notifyAdding)(std::vector<ImageLoader*>& images);
		void			(*getAllMappedRegions)(RegionsVector&);
		void *			(*bindingHandler)(const char *, const char *, void *);
		BindingOptions	bindingOptions;
		int				argc;
		const char**	argv;
		const char**	envp;
		const char**	apple;
		ImageLoader*	mainExecutable;
		const char*		imageSuffix;
		PrebindMode		prebindUsage;
		SharedRegionMode sharedRegionMode;
		bool			prebinding;
		bool			bindFlat;
		bool			slideAndPackDylibs;
		bool			verboseOpts;
		bool			verboseEnv;
		bool			verboseMapping;
		bool			verboseRebase;
		bool			verboseBind;
		bool			verboseInit;
		bool			verbosePrebinding;
		bool			verboseWarnings;
	};
	
										// constructor is protected, but anyone can delete an image
	virtual								~ImageLoader();
	
										// link() takes a newly instantiated ImageLoader and does all 
										// fixups needed to make it usable by the process
	void								link(const LinkContext& context, BindingLaziness mode, InitializerRunning inits, uint32_t notifyCount);
	
										// runInitializers() is normally called in link() but the main executable must 
										// run crt code before initializers
	void								runInitializers(const LinkContext& context);
	
										// runNotification() is normally called in link() but the main executable must 
										// run crt code before initializers
	void								runNotification(const LinkContext& context, uint32_t notifyCount);
		
										// used by dyld to see if a requested library is already loaded (might be symlink)
	bool								statMatch(const struct stat& stat_buf) const;

										// get short name of this image
	const char*							getShortName() const;

										// get path used to load this image, not necessarily the "real" path
	const char*							getPath() const { return fPath; }

	uint32_t							getPathHash() const { return fPathHash; }

										// get path used to load this image represents (ZeroLink only) which image this .o is part of
	const char*							getLogicalPath() const;

										// get path this image is intended to be placed on disk or NULL if no preferred install location
	virtual const char*					getInstallPath() const = 0;

										// image was loaded with NSADDIMAGE_OPTION_MATCH_FILENAME_BY_INSTALLNAME and all clients are looking for install path 
	bool								matchInstallPath() const;
	void								setMatchInstallPath(bool);
	
										// if path was a fat file, offset of image loaded in that fat file
	uint64_t							getOffsetInFatFile() const;

										// mark that this image's exported symbols should be ignored when linking other images (e.g. RTLD_LOCAL)
	void								setHideExports(bool hide = true);
	
										// check if this image's exported symbols should be ignored when linking other images 
	bool								hasHiddenExports() const;
	
										// checks if this image is already linked into the process
	bool								isLinked() const;
	
										// even if image is deleted, leave segments mapped in
	void								setLeaveMapped();
	
										// checks if the specifed address is within one of this image's segments
	virtual bool						containsAddress(const void* addr) const;

										// adds to list of ranges of memory mapped in
	void								addMappedRegions(RegionsVector& regions) const;

										// st_mtime from stat() on file
	time_t								lastModified();

										// image should create prebound version of itself and return freespace remaining on disk
	uint64_t							reprebind(const LinkContext& context, time_t timestamp);
	
										// if 'commit', the prebound version should be swapped in, otherwise deleted
	void								reprebindCommit(const LinkContext& context, bool commit, bool unmapOld);
	
										// only valid for main executables, returns a pointer its entry point
	virtual void*						getMain() const = 0;
	
										// dyld API's require each image to have an associated mach_header
	virtual const struct mach_header*   machHeader() const = 0;
	
										// dyld API's require each image to have a slide (actual load address minus preferred load address)
	virtual uintptr_t					getSlide() const = 0;
	
										// dyld API's require each image to have a slide (actual load address minus preferred load address)
	virtual const void*					getBaseAddress() const = 0;
	
										// image has exports that participate in runtime coalescing
	virtual bool						hasCoalescedExports() const = 0;
	
										// search symbol table of definitions in this image for requested name
	virtual const Symbol*				findExportedSymbol(const char* name, const void* hint, bool searchReExports, ImageLoader** foundIn) const = 0;
	
										// gets address of implementation (code) of the specified exported symbol
	virtual uintptr_t					getExportedSymbolAddress(const Symbol* sym) const = 0;
	
										// gets attributes of the specified exported symbol
	virtual DefinitionFlags				getExportedSymbolInfo(const Symbol* sym) const = 0;
	
										// gets name of the specified exported symbol
	virtual const char*					getExportedSymbolName(const Symbol* sym) const = 0;
	
										// gets how many symbols are exported by this image
	virtual uint32_t					getExportedSymbolCount() const = 0;
			
										// gets the i'th exported symbol
	virtual const Symbol*				getIndexedExportedSymbol(uint32_t index) const = 0;
			
										// find exported symbol as if imported by this image
										// used by RTLD_NEXT
	virtual const Symbol*				findExportedSymbolInDependentImages(const char* name, ImageLoader** foundIn) const;
	
										// find exported symbol as if imported by this image
										// used by RTLD_SELF
	virtual const Symbol*				findExportedSymbolInImageOrDependentImages(const char* name, ImageLoader** foundIn) const;
	
										// gets how many symbols are imported by this image
	virtual uint32_t					getImportedSymbolCount() const = 0;
	
										// gets the i'th imported symbol
	virtual const Symbol*				getIndexedImportedSymbol(uint32_t index) const = 0;
			
										// gets attributes of the specified imported symbol
	virtual ReferenceFlags				geImportedSymbolInfo(const Symbol* sym) const = 0;
			
										// gets name of the specified imported symbol
	virtual const char*					getImportedSymbolName(const Symbol* sym) const = 0;
			
										// checks if this image is a bundle and can be loaded but not linked
	virtual bool						isBundle() const = 0;
	
										// checks if this image is a dylib 
	virtual bool						isDylib() const = 0;
	
										// only for main executable
	virtual bool						forceFlat() const = 0;
	
										// called at runtime when a lazily bound function is first called
	virtual uintptr_t					doBindLazySymbol(uintptr_t* lazyPointer, const LinkContext& context) = 0;
	
										// calls termination routines (e.g. C++ static destructors for image)
	virtual void						doTermination(const LinkContext& context) = 0;
					
										// tell this image about other images
	virtual void						doNotification(enum dyld_image_mode mode, uint32_t infoCount, const struct dyld_image_info info[]) = 0;
					
										// return if this image has initialization routines
	virtual bool						needsInitialization() = 0;
			
										// return if this image has a routine to be called when any image is loaded or unloaded
	virtual bool						hasImageNotification() = 0;
	
										// return if this image has specified section and set start and length
	virtual bool						getSectionContent(const char* segmentName, const char* sectionName, void** start, size_t* length) = 0;
			
										// given a pointer into an image, find which segment and section it is in
	virtual bool						findSection(const void* imageInterior, const char** segmentName, const char** sectionName, size_t* sectionOffset) = 0;
	
										// the image supports being prebound
	virtual bool						isPrebindable() const = 0;
	
										// the image is prebindable and its prebinding is valid
	virtual bool						usablePrebinding(const LinkContext& context) const = 0;
	
										// used to implement refernce counting of images
	void								incrementReferenceCount();
	bool								decrementReferenceCount();

										// triggered by DYLD_PRINT_STATISTICS to write info on work done and how fast
	static void							printStatistics(unsigned int imageCount);
				
										// used with DYLD_IMAGE_SUFFIX
	static void							addSuffix(const char* path, const char* suffix, char* result);
	
	static uint32_t						hash(const char*);
	
	
			void						setPath(const char* path);	// only called for images created from memory
			void						setLogicalPath(const char* path);
			
		
protected:
	// abstract base class so all constructors protected
					ImageLoader(const char* path, uint64_t offsetInFat, const struct stat& info); 
					ImageLoader(const char* moduleName); 
					ImageLoader(const ImageLoader&);
	void			operator=(const ImageLoader&);
	

	struct LibraryInfo {
		uint64_t		checksum;
		uint32_t		minVersion;
		uint32_t		maxVersion;
	};

	struct DependentLibrary {
		const char*		name;
		ImageLoader*	image;
		LibraryInfo		info;
		bool			required;
		bool			checksumMatches;
		bool			isReExported;
		bool			isSubFramework;
	};
	
	typedef void (*Initializer)(int argc, const char* argv[], const char* envp[],const char* apple[]);
	typedef void (*Terminator)(void);
	
						// To link() an image, its dependent libraries are loaded, it is rebased, bound, and initialized.
						// These methods do the above, exactly once, and it the right order
	void				recursiveLoadLibraries(const LinkContext& context);
	void				recursiveRebase(const LinkContext& context);
	void				recursiveBind(const LinkContext& context, BindingLaziness bindness);
	void				recursiveImageAnnouncement(const LinkContext& context, std::vector<ImageLoader*>& newImages);
	void				recursiveImageNotification(const LinkContext& context, uint32_t addImageCount);
	void				recursiveInitialization(const LinkContext& context);

								// map any segments this image has into memory and build fSegments
								// this is called before doGetDependentLibraryCount so if metadata is in segments it is mapped in
	virtual void				instantiateSegments(const uint8_t* fileData) = 0;
	
								// return how many libraries this image depends on
	virtual uint32_t			doGetDependentLibraryCount() = 0;
	
								// fill in information about dependent libraries (array length is doGetDependentLibraryCount())
	virtual void				doGetDependentLibraries(DependentLibrary libs[]) = 0;
	
								// called on images that are libraries, returns info about itself
	virtual LibraryInfo			doGetLibraryInfo() = 0;
	
								// do any fix ups in this image that depend only on the load address of the image
	virtual void				doRebase(const LinkContext& context) = 0;
	
								// do any symbolic fix ups in this image
	virtual void				doBind(const LinkContext& context, BindingLaziness bindness) = 0;
	
								// run any initialization routines in this image
	virtual void				doInitialization(const LinkContext& context) = 0;
	
								// write prebinding updates to mapped file fileToPrebind
	virtual void				doPrebinding(const LinkContext& context, time_t timestamp, uint8_t* fileToPrebind) = 0;

								// return if this image has termination routines
	virtual bool				needsTermination() = 0;
	
								// support for runtimes in which segments don't have to maintain their relative positions
	virtual bool				segmentsMustSlideTogether() const = 0;	
	
								// built with PIC code and can load at any address
	virtual bool				segmentsCanSlide() const = 0;		
	
								// set how much all segments slide
	virtual void				setSlide(intptr_t slide) = 0;		
	
								// utility routine to map in all segements in fSegments from a file
	virtual void				mapSegments(int fd, uint64_t offsetInFat, uint64_t lenInFat, uint64_t fileLen, const LinkContext& context);
	
								// utility routine to map in all segements in fSegments from a memory image
	virtual void				mapSegments(const void* memoryImage, uint64_t imageLen, const LinkContext& context);

								// returns if all dependent libraries checksum's were as expected and none slide
			bool				allDependentLibrariesAsWhenPreBound() const;

								// in mach-o a child tells it parent to re-export, instead of the other way around...
	virtual	bool				isSubframeworkOf(const LinkContext& context, const ImageLoader* image) const = 0;

								// in mach-o a parent library knows name of sub libraries it re-exports..
	virtual	bool				hasSubLibrary(const LinkContext& context, const ImageLoader* child) const  = 0;
	
								// file has been reprebound on disk, unmap this file so original file is released
	virtual void				prebindUnmap(const LinkContext& context) = 0;
	
	static uint32_t				fgImagesWithUsedPrebinding;
	static uint32_t				fgTotalRebaseFixups;
	static uint32_t				fgTotalBindFixups;
	static uint32_t				fgTotalLazyBindFixups;
	static uint32_t				fgTotalPossibleLazyBindFixups;
	static uint64_t				fgTotalLoadLibrariesTime;
	static uint64_t				fgTotalRebaseTime;
	static uint64_t				fgTotalBindTime;
	static uint64_t				fgTotalNotifyTime;
	static uint64_t				fgTotalInitTime;
	static uintptr_t			fgNextSplitSegAddress;
	const char*					fPath;
	const char*					fLogicalPath; // for ZeroLink - the image which this bundle is part of
	dev_t						fDevice;
	ino_t						fInode;
	time_t						fLastModified;
	uint64_t					fOffsetInFatFile;
	std::vector<class Segment*> fSegments;
	DependentLibrary*			fLibraries;
	uint32_t					fLibrariesCount;
	uint32_t					fPathHash;
	uint32_t					fReferenceCount;
	bool						fAllLibraryChecksumsAndLoadAddressesMatch;
	bool						fLeaveMapped;		// when unloaded, leave image mapped in cause some other code may have pointers into it


private:
	void						init(const char* path, uint64_t offsetInFat, dev_t device, ino_t inode, time_t modDate);
	intptr_t					assignSegmentAddresses(const LinkContext& context);
	uint64_t					copyAndMap(const char* tempFile, uint8_t** fileToPrebind, uint64_t* fileToPrebindSize);
	const ImageLoader::Symbol*	findExportedSymbolInDependentImagesExcept(const char* name, std::set<const ImageLoader*>& dontSearchImages, ImageLoader** foundIn) const;


	bool						fHideSymbols;		// ignore this image's exported symbols when linking other images
	bool						fMatchByInstallName;// look at image's install-path not its load path
	bool						fLibrariesLoaded;
	bool						fBased;
	bool						fBoundAllNonLazy;
	bool						fBoundAllLazy;
	bool						fAnnounced;
	bool						fInitialized;
	uint16_t					fNextAddImageIndex;
};


//
// Segment is an abstract base class.  A segment is a chunk of an executable
// file that is mapped into memory.  Each subclass of ImageLoader typically
// implements its own concrete subclass of Segment.
//
//  
class Segment {
public:
	virtual						~Segment() {}

	virtual const ImageLoader*	getImage() = 0;
	virtual const char*			getName() = 0;
	virtual uintptr_t			getSize() = 0;
	virtual uintptr_t			getFileSize() = 0;
	virtual bool				hasTrailingZeroFill();
	virtual uintptr_t			getFileOffset() = 0;
	virtual bool				readable() = 0;
	virtual bool				writeable() = 0;
	virtual bool				executable() = 0;
	virtual bool				unaccessible() = 0;
	virtual bool				hasFixUps() = 0;
	virtual uintptr_t			getActualLoadAddress() = 0;
	virtual uintptr_t			getPreferredLoadAddress() = 0;
	virtual void				setUnMapWhenDestructed(bool unmap) = 0;
	
protected:
	// abstract base class so all constructors protected
								Segment() {}
								Segment(const Segment&);
	void						operator=(const Segment&);

	virtual bool				hasPreferredLoadAddress() = 0;
	//virtual void				setActualLoadAddress(uint64_t addr) = 0;
	
	static bool					reserveAddressRange(uintptr_t start, size_t length);
	static uintptr_t			reserveAnAddressRange(size_t length, const ImageLoader::LinkContext& context);
	static uintptr_t			fgNextNonSplitSegAddress;

private:
	void						map(int fd, uint64_t offsetInFatWrapper, intptr_t slide, const ImageLoader::LinkContext& context);
	void						map(const void* memoryImage, intptr_t slide, const ImageLoader::LinkContext& context);
	void						setPermissions();
	void						tempWritable();
	
	friend class ImageLoader;
	friend class ImageLoaderMachO;
};



#endif

