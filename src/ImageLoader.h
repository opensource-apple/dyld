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


#ifndef __IMAGELOADER__
#define __IMAGELOADER__

#include <sys/types.h>
#include <mach/mach_time.h> // struct mach_timebase_info
#include <mach/mach_init.h> // struct mach_thread_self
#include <stdint.h>
#include <vector>
#include <set>

#include "mach-o/dyld_images.h"
#include "mach-o/dyld_priv.h"


#define SPLIT_SEG_SHARED_REGION_SUPPORT 0
#define SPLIT_SEG_DYLIB_SUPPORT (__ppc__ || __i386__)
#define TEXT_RELOC_SUPPORT (__ppc__ || __i386__)
#define DYLD_SHARED_CACHE_SUPPORT (__ppc__ || __i386__ || __ppc64__ || __x86_64__)
#define IMAGE_NOTIFY_SUPPORT 0
#define RECURSIVE_INITIALIZER_LOCK 1
#define SUPPORT_OLD_CRT_INITIALIZATION (__ppc__ || __i386__)


// utilities
namespace dyld {
	extern __attribute__((noreturn)) void throwf(const char* format, ...)  __attribute__((format(printf, 1, 2)));
	extern void log(const char* format, ...)  __attribute__((format(printf, 1, 2)));
	extern void warn(const char* format, ...)  __attribute__((format(printf, 1, 2)));
	extern const char* mkstringf(const char* format, ...)  __attribute__((format(printf, 1, 2)));
};


struct ProgramVars
{
	const void*		mh;
	int*			NXArgcPtr;
	const char***	NXArgvPtr;
	const char***	environPtr;
	const char**	__prognamePtr;
};



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
	
	enum PrebindMode { kUseAllPrebinding, kUseSplitSegPrebinding, kUseAllButAppPredbinding, kUseNoPrebinding };
	enum BindingOptions { kBindingNone, kBindingLazyPointers, kBindingNeverSetLazyPointers };
	enum SharedRegionMode { kUseSharedRegion, kUsePrivateSharedRegion, kDontUseSharedRegion, kSharedRegionIsSharedCache };
	
	struct Symbol;  // abstact symbol

	struct MappedRegion {
		uintptr_t	address;
		size_t		size;
	};

	struct RPathChain {
		RPathChain(const RPathChain* n, std::vector<const char*>* p) : next(n), paths(p) {};
		const RPathChain*			next;
		std::vector<const char*>*	paths;
	};

	struct DOFInfo {
		void*				dof;
		const mach_header*	imageHeader;
		const char*			imageShortName;
	};

	struct LinkContext {
		ImageLoader*	(*loadLibrary)(const char* libraryName, bool search, bool findDLL, const char* origin, const RPathChain* rpaths);
		void			(*terminationRecorder)(ImageLoader* image);
		bool			(*flatExportFinder)(const char* name, const Symbol** sym, const ImageLoader** image);
		bool			(*coalescedExportFinder)(const char* name, const Symbol** sym, const ImageLoader** image);
		void			(*undefinedHandler)(const char* name);
#if IMAGE_NOTIFY_SUPPORT
		void			(*addImageNeedingNotification)(ImageLoader* image);
		void			(*notifyAdding)(const ImageLoader* const * images, unsigned int count);
#endif
		MappedRegion*	(*getAllMappedRegions)(MappedRegion*);
		void *			(*bindingHandler)(const char *, const char *, void *);
		void			(*notifySingle)(dyld_image_states, const struct mach_header*, const char* path, time_t modDate);
		void			(*notifyBatch)(dyld_image_states state);
		void			(*removeImage)(ImageLoader* image);
		void			(*registerDOFs)(const std::vector<DOFInfo>& dofs);
		void			(*clearAllDepths)();
		unsigned int	(*imageCount)();
		void			(*notifySharedCacheInvalid)();
#if __i386__
		void			(*makeSharedCacheImportSegmentsWritable)(bool writable);
#endif
		void			(*setNewProgramVars)(const ProgramVars&);
#if SUPPORT_OLD_CRT_INITIALIZATION
		void			(*setRunInitialzersOldWay)();
#endif
		BindingOptions	bindingOptions;
		int				argc;
		const char**	argv;
		const char**	envp;
		const char**	apple;
		const char*		progname;
		ProgramVars		programVars;
		ImageLoader*	mainExecutable;
		const char*		imageSuffix;
		PrebindMode		prebindUsage;
		SharedRegionMode sharedRegionMode;
		bool			dyldLoadedAtSameAddressNeededBySharedCache; 
		bool			preFetchDisabled;
		bool			prebinding;
		bool			bindFlat;
		bool			linkingMainExecutable;
		bool			startedInitializingMainExecutable;
		bool			verboseOpts;
		bool			verboseEnv;
		bool			verboseMapping;
		bool			verboseRebase;
		bool			verboseBind;
		bool			verboseInit;
		bool			verboseDOF;
		bool			verbosePrebinding;
		bool			verboseWarnings;
	};
	
										// constructor is protected, but anyone can delete an image
	virtual								~ImageLoader();
	
										// link() takes a newly instantiated ImageLoader and does all 
										// fixups needed to make it usable by the process
	void								link(const LinkContext& context, bool forceLazysBound, bool preflight, const RPathChain& loaderRPaths);
	
										// runInitializers() is normally called in link() but the main executable must 
										// run crt code before initializers
	void								runInitializers(const LinkContext& context);
	
										// called after link() forces all lazy pointers to be bound
	void								bindAllLazyPointers(const LinkContext& context, bool recursive);
	
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
	
										// even if image is deleted, leave segments mapped in
	bool								leaveMapped() { return fLeaveMapped; }

										// checks if the specifed address is within one of this image's segments
	virtual bool						containsAddress(const void* addr) const;

										// checks if the specifed address range overlaps any of this image's segments
	virtual bool						overlapsWithAddressRange(const void* start, const void* end) const;

										// adds to list of ranges of memory mapped in
	void								getMappedRegions(MappedRegion*& region) const;

										// st_mtime from stat() on file
	time_t								lastModified() const;

										// only valid for main executables, returns a pointer its entry point
	virtual void*						getMain() const = 0;
	
										// dyld API's require each image to have an associated mach_header
	virtual const struct mach_header*   machHeader() const = 0;
	
										// dyld API's require each image to have a slide (actual load address minus preferred load address)
	virtual uintptr_t					getSlide() const = 0;
	
										// last address mapped by image
	virtual const void*					getEnd() const = 0;
	
										// image has exports that participate in runtime coalescing
	virtual bool						hasCoalescedExports() const = 0;
	
										// search symbol table of definitions in this image for requested name
	virtual const Symbol*				findExportedSymbol(const char* name, const void* hint, bool searchReExports, const ImageLoader** foundIn) const = 0;
	
										// gets address of implementation (code) of the specified exported symbol
	virtual uintptr_t					getExportedSymbolAddress(const Symbol* sym, const LinkContext& context, const ImageLoader* requestor=NULL) const = 0;
	
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
	virtual const Symbol*				findExportedSymbolInDependentImages(const char* name, const LinkContext& context, const ImageLoader** foundIn) const;
	
										// find exported symbol as if imported by this image
										// used by RTLD_SELF
	virtual const Symbol*				findExportedSymbolInImageOrDependentImages(const char* name, const LinkContext& context, const ImageLoader** foundIn) const;
	
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
					
#if IMAGE_NOTIFY_SUPPORT
										// tell this image about other images
	virtual void						doNotification(enum dyld_image_mode mode, uint32_t infoCount, const struct dyld_image_info info[]) = 0;
#endif					
										// return if this image has initialization routines
	virtual bool						needsInitialization() = 0;
			
#if IMAGE_NOTIFY_SUPPORT
										// return if this image has a routine to be called when any image is loaded or unloaded
	virtual bool						hasImageNotification() = 0;
#endif	
										// return if this image has specified section and set start and length
	virtual bool						getSectionContent(const char* segmentName, const char* sectionName, void** start, size_t* length) = 0;
			
										// given a pointer into an image, find which segment and section it is in
	virtual bool						findSection(const void* imageInterior, const char** segmentName, const char** sectionName, size_t* sectionOffset) = 0;
	
										// the image supports being prebound
	virtual bool						isPrebindable() const = 0;
	
										// the image is prebindable and its prebinding is valid
	virtual bool						usablePrebinding(const LinkContext& context) const = 0;
	
										// add all RPATH paths this image contains
	virtual	void						getRPaths(const LinkContext& context, std::vector<const char*>&) const = 0;
	
	

	dyld_image_states					getState() { return (dyld_image_states)fState; }
	
										// used to sort images bottom-up
	int									compare(const ImageLoader* right) const;
	
	void								incrementDlopenReferenceCount() { ++fDlopenReferenceCount; }

	bool								decrementDlopenReferenceCount();
	
	void								printReferenceCounts();

	uint32_t							referenceCount() const { return fDlopenReferenceCount + fStaticReferenceCount + fDynamicReferenceCount; }

	bool								neverUnload() const { return fNeverUnload; }

	void								setNeverUnload() { fNeverUnload = true; fLeaveMapped = true; }

										// triggered by DYLD_PRINT_STATISTICS to write info on work done and how fast
	static void							printStatistics(unsigned int imageCount);
				
										// used with DYLD_IMAGE_SUFFIX
	static void							addSuffix(const char* path, const char* suffix, char* result);
	
	static uint32_t						hash(const char*);
		
			void						setPath(const char* path);	// only called for images created from memory
			void						setPathUnowned(const char* path);
			
			void						setLogicalPath(const char* path);
			
			void						clearDepth() { fDepth = 0; }
			
			void						setBeingRemoved() { fBeingRemoved = true; }
			bool						isBeingRemoved() const { return fBeingRemoved; }
			
			void						setAddFuncNotified() { fAddFuncNotified = true; }
			bool						addFuncNotified() const { return fAddFuncNotified; }
	
protected:	
	struct DependentLibrary;
public:
	friend class iterator;
	
	class iterator
	{
	public:
		iterator&		operator++() { ++fLocation; return *this; }
		bool			operator!=(const iterator& it) const { return (it.fLocation != this->fLocation); }
		ImageLoader*	operator*() const { return fLocation->image; }
	private:
		friend class ImageLoader;
		iterator(DependentLibrary* loc) : fLocation(loc) {}
		DependentLibrary*	fLocation;
	};
	
	iterator beginDependents() { return iterator(fLibraries); }
	iterator endDependents() { return iterator(&fLibraries[fLibrariesCount]); }
	
	
		
	friend class Segment;
		
protected:
	// abstract base class so all constructors protected
					ImageLoader(const char* path, uint64_t offsetInFat, const struct stat& info); 
					ImageLoader(const char* moduleName); 
					ImageLoader(const ImageLoader&);
	void			operator=(const ImageLoader&);
	

	struct LibraryInfo {
		uint32_t		checksum;
		uint32_t		minVersion;
		uint32_t		maxVersion;
	};

	struct DependentLibrary {
		ImageLoader*	image;
		uint32_t		required : 1,
						checksumMatches : 1,
						isReExported : 1,
						isSubFramework : 1;
	};
	
	struct DependentLibraryInfo {
		const char*			name;
		LibraryInfo			info;
		bool				required;
		bool				reExported;
	};

	class SegmentIterator
	{
	public:
		SegmentIterator&	operator++(); // use inline later to work around circular reference
		bool				operator!=(const SegmentIterator& it) const { return (it.fLocation != this->fLocation); }
		class Segment*		operator*() const { return fLocation; }
		SegmentIterator(class Segment* loc) : fLocation(loc) {}
	private:
		class Segment*		fLocation;
	};

	typedef void (*Initializer)(int argc, const char* argv[], const char* envp[], const char* apple[], const ProgramVars* vars);
	typedef void (*Terminator)(void);
	
						// To link() an image, its dependent libraries are loaded, it is rebased, bound, and initialized.
						// These methods do the above, exactly once, and it the right order
	void				recursiveLoadLibraries(const LinkContext& context, const RPathChain& loaderRPaths);
	void				recursiveUnLoadMappedLibraries(const LinkContext& context);
	unsigned int		recursiveUpdateDepth(unsigned int maxDepth);
	void				recursiveValidate(const LinkContext& context);
	void				recursiveRebase(const LinkContext& context);
	void				recursiveBind(const LinkContext& context, bool forceLazysBound);
	void				recursiveGetDOFSections(const LinkContext& context, std::vector<DOFInfo>& dofs);
#if IMAGE_NOTIFY_SUPPORT
	void				recursiveImageAnnouncement(const LinkContext& context, ImageLoader**& newImages);
#endif
	void				recursiveInitialization(const LinkContext& context, mach_port_t this_thread);

								// return how many libraries this image depends on
	virtual uint32_t			doGetDependentLibraryCount() = 0;
	
								// fill in information about dependent libraries (array length is doGetDependentLibraryCount())
	virtual void				doGetDependentLibraries(DependentLibraryInfo libs[]) = 0;
	
								// called on images that are libraries, returns info about itself
	virtual LibraryInfo			doGetLibraryInfo() = 0;
	
								// do any fix ups in this image that depend only on the load address of the image
	virtual void				doRebase(const LinkContext& context) = 0;
	
								// do any symbolic fix ups in this image
	virtual void				doBind(const LinkContext& context, bool forceLazysBound) = 0;
	
								// called later via API to force all lazy pointer to be bound
	virtual void				doBindJustLazies(const LinkContext& context) = 0;
	
								// update any segment permissions
	virtual void				doUpdateMappingPermissions(const LinkContext& context) = 0;
	
								// if image has any dtrace DOF sections, append them to list to be registered
	virtual void				doGetDOFSections(const LinkContext& context, std::vector<DOFInfo>& dofs) = 0;
	
								// run any initialization routines in this image
	virtual void				doInitialization(const LinkContext& context) = 0;
	
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
	
								// set fState to dyld_image_state_memory_mapped
	void						setMapped(const LinkContext& context);
	
								// mark that target should not be unloaded unless this is also unloaded
	void						addDynamicReference(const ImageLoader* target);
				
								// used to start iterating Segments
	virtual SegmentIterator		beginSegments() const = 0;
	
								// used to end iterating Segments
	virtual SegmentIterator		endSegments() const = 0;
	

	static uint32_t				fgImagesWithUsedPrebinding;
	static uint32_t				fgImagesUsedFromSharedCache;
	static uint32_t				fgImagesRequiringNoFixups;
	static uint32_t				fgTotalRebaseFixups;
	static uint32_t				fgTotalBindFixups;
	static uint32_t				fgTotalBindSymbolsResolved;
	static uint32_t				fgTotalBindImageSearches;
	static uint32_t				fgTotalLazyBindFixups;
	static uint32_t				fgTotalPossibleLazyBindFixups;
	static uint32_t				fgTotalSegmentsMapped;
	static uint64_t				fgTotalBytesMapped;
	static uint64_t				fgTotalBytesPreFetched;
	static uint64_t				fgTotalLoadLibrariesTime;
	static uint64_t				fgTotalRebaseTime;
	static uint64_t				fgTotalBindTime;
	static uint64_t				fgTotalInitTime;
	static uintptr_t			fgNextSplitSegAddress;
	const char*					fPath;
	const char*					fLogicalPath; // for ZeroLink - the image which this bundle is part of
	dev_t						fDevice;
	ino_t						fInode;
	time_t						fLastModified;
	uint64_t					fOffsetInFatFile;
	DependentLibrary*			fLibraries;
	uint32_t					fLibrariesCount;
	uint32_t					fPathHash;
	uint32_t					fDlopenReferenceCount;	// count of how many dlopens have been done on this image
	uint32_t					fStaticReferenceCount;	// count of images that have a fLibraries entry pointing to this image
	uint32_t					fDynamicReferenceCount;	// count of images that have a fDynamicReferences entry pointer to this image
	std::set<const ImageLoader*>* fDynamicReferences;	// list of all images this image used because of a flat/coalesced lookup

private:
#if RECURSIVE_INITIALIZER_LOCK
	struct recursive_lock {
						recursive_lock(mach_port_t t) : thread(t), count(0) {}
		mach_port_t		thread;
		int				count;
	};
	void						recursiveSpinLock(recursive_lock&);
	void						recursiveSpinUnLock();
#endif 

	void						init(const char* path, uint64_t offsetInFat, dev_t device, ino_t inode, time_t modDate);
	intptr_t					assignSegmentAddresses(const LinkContext& context);
	const ImageLoader::Symbol*	findExportedSymbolInDependentImagesExcept(const char* name, const ImageLoader** dsiStart, 
										const ImageLoader**& dsiCur, const ImageLoader** dsiEnd, const ImageLoader** foundIn) const;



	uint16_t					fDepth;
	uint16_t					fLoadOrder;
	uint32_t					fState : 8,
								fAllLibraryChecksumsAndLoadAddressesMatch : 1,
								fLeaveMapped : 1,		// when unloaded, leave image mapped in cause some other code may have pointers into it
								fNeverUnload : 1,		// image was statically loaded by main executable
								fHideSymbols : 1,		// ignore this image's exported symbols when linking other images
								fMatchByInstallName : 1,// look at image's install-path not its load path
								fRegisteredDOF : 1,
#if IMAGE_NOTIFY_SUPPORT
								fAnnounced : 1,
#endif
								fAllLazyPointersBound : 1,
								fBeingRemoved : 1,
								fAddFuncNotified : 1,
								fPathOwnedByImage : 1;

#if RECURSIVE_INITIALIZER_LOCK
	recursive_lock*				fInitializerRecursiveLock;
#else
	uint32_t					fInitializerLock;
#endif
	static uint16_t				fgLoadOrdinal;
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

	virtual const char*			getName() = 0;
	virtual uintptr_t			getSize() = 0;
	virtual uintptr_t			getFileSize() = 0;
	virtual bool				hasTrailingZeroFill();
	virtual uintptr_t			getFileOffset() = 0;
	virtual bool				readable() = 0;
	virtual bool				writeable() = 0;
	virtual bool				executable() = 0;
	virtual bool				unaccessible() = 0;
	virtual uintptr_t			getActualLoadAddress(const ImageLoader*) = 0;
	virtual uintptr_t			getPreferredLoadAddress() = 0;
	virtual void				unmap(const ImageLoader*) = 0;
	virtual Segment*			next(Segment*) = 0;
#if __i386__
	virtual bool				readOnlyImportStubs() = 0;
#endif
	
	
protected:
	// abstract base class so all constructors protected
								Segment() {}
								Segment(const Segment&);
	void						operator=(const Segment&);

	virtual bool				hasPreferredLoadAddress() = 0;
	//virtual void				setActualLoadAddress(uint64_t addr) = 0;
	virtual void				map(int fd, uint64_t offsetInFatWrapper, intptr_t slide, const ImageLoader* image, const ImageLoader::LinkContext& context);
	
	static bool					reserveAddressRange(uintptr_t start, size_t length);
	static uintptr_t			reserveAnAddressRange(size_t length, const class ImageLoader::LinkContext& context);
	static uintptr_t			fgNextPIEDylibAddress;

private:
	void						setPermissions(const ImageLoader::LinkContext& context, const ImageLoader* image);
	void						map(const void* memoryImage, intptr_t slide, const ImageLoader* image, const ImageLoader::LinkContext& context);
	void						tempWritable(const ImageLoader::LinkContext& context, const ImageLoader* image);
	
	friend class ImageLoader;
	friend class ImageLoaderMachO;
};

inline ImageLoader::SegmentIterator& ImageLoader::SegmentIterator::operator++() { fLocation = fLocation->next(fLocation); return *this; }




#endif

