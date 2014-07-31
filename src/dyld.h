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

#include <stdint.h>

#include "ImageLoader.h"
#include "mach-o/dyld_priv.h"



//
// dyld functions available when implementing dyld API's
//
//
namespace dyld {

	struct LoadContext
	{
		bool			useSearchPaths;
		bool			useLdLibraryPath;
		bool			implicitRPath;
		bool			matchByInstallName;
		bool			dontLoad;
		bool			mustBeBundle;
		bool			mustBeDylib;
		bool			findDLL;
		const char*						origin;			// path for expanding @loader_path
		const ImageLoader::RPathChain*	rpath;			// paths for expanding @rpath
	};



	typedef void		 (*ImageCallback)(const struct mach_header* mh, intptr_t slide);
	typedef void		 (*BundleNotificationCallBack)(const char* imageName, ImageLoader* image);
	typedef ImageLoader* (*BundleLocatorCallBack)(const char* symbolName);
	typedef void		 (*UndefinedHandler)(const char* symbolName);
	typedef const char*	 (*ImageLocator)(const char* dllName);


	extern ImageLoader::LinkContext			gLinkContext;
	extern bool								gLogAPIs;
	extern bool								gSharedCacheNotFound;
	extern bool								gSharedCacheNeedsUpdating;
	extern bool								gSharedCacheDontNotify;
	extern const struct LibSystemHelpers*	gLibSystemHelpers;
#if SUPPORT_OLD_CRT_INITIALIZATION
	extern bool								gRunInitializersOldWay;
#endif
	extern void					registerAddCallback(ImageCallback func);
	extern void					registerRemoveCallback(ImageCallback func);
	extern void					registerZeroLinkHandlers(BundleNotificationCallBack, BundleLocatorCallBack);
	extern void					registerUndefinedHandler(UndefinedHandler);
	extern void					initializeMainExecutable();
	extern void					preflight(ImageLoader* image, const ImageLoader::RPathChain& loaderRPaths);
	extern void					link(ImageLoader* image, bool forceLazysBound, const ImageLoader::RPathChain& loaderRPaths);
	extern void					runInitializers(ImageLoader* image);
	extern void					runTerminators(void*);
	extern const char*			getExecutablePath();
	extern bool					validImage(const ImageLoader*);
	extern ImageLoader*			getIndexedImage(uint32_t index);
	extern uint32_t				getImageCount();
	extern ImageLoader*			findImageByMachHeader(const struct mach_header* target);
	extern ImageLoader*			findImageContainingAddress(const void* addr);
	extern ImageLoader*			findImageByName(const char* path);
	extern ImageLoader*			findLoadedImageByInstallPath(const char* path);
	extern bool					flatFindExportedSymbol(const char* name, const ImageLoader::Symbol** sym, const ImageLoader** image);
	extern bool					flatFindExportedSymbolWithHint(const char* name, const char* librarySubstring, const ImageLoader::Symbol** sym, const ImageLoader** image);
	extern ImageLoader*			load(const char* path, const LoadContext& context);
	extern ImageLoader*			loadFromMemory(const uint8_t* mem, uint64_t len, const char* moduleName);
	extern void					removeImage(ImageLoader* image);
	extern ImageLoader*			cloneImage(ImageLoader* image);
	extern void					forEachImageDo( void (*)(ImageLoader*, void*), void*);
	extern uintptr_t			_main(const struct mach_header* mainExecutableMH, uintptr_t mainExecutableSlide, int argc, const char* argv[], const char* envp[], const char* apple[]);
	extern void					halt(const char* message)  __attribute__((noreturn));
	extern void					setErrorMessage(const char* msg);
	extern const char*			getErrorMessage();
	extern void					clearErrorMessage();
	extern bool					mainExecutablePrebound();
	extern ImageLoader*			mainExecutable();
	extern void					processDyldEnvironmentVarible(const char* key, const char* value);
	extern void					registerImageStateSingleChangeHandler(dyld_image_states state, dyld_image_state_change_handler handler);
	extern void					registerImageStateBatchChangeHandler(dyld_image_states state, dyld_image_state_change_handler handler);
	extern void					garbageCollectImages();
	extern void					registerWinImageLocator(ImageLocator);
	extern int					openSharedCacheFile();
	extern const void*			imMemorySharedCacheHeader();

};

