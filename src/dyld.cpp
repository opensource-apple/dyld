/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2013 Apple Inc. All rights reserved.
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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/param.h>
#include <mach/mach_time.h> // mach_absolute_time()
#include <mach/mach_init.h> 
#include <sys/types.h>
#include <sys/stat.h> 
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syslog.h>
#include <sys/uio.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h> 
#include <mach-o/ldsyms.h> 
#include <libkern/OSByteOrder.h> 
#include <libkern/OSAtomic.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <sys/dtrace.h>
#include <libkern/OSAtomic.h>
#include <Availability.h>
#include <System/sys/codesign.h>
#include <_simple.h>
#include <os/lock_private.h>


#ifndef CPU_SUBTYPE_ARM_V5TEJ
	#define CPU_SUBTYPE_ARM_V5TEJ		((cpu_subtype_t) 7)
#endif
#ifndef CPU_SUBTYPE_ARM_XSCALE
	#define CPU_SUBTYPE_ARM_XSCALE		((cpu_subtype_t) 8)
#endif
#ifndef CPU_SUBTYPE_ARM_V7
	#define CPU_SUBTYPE_ARM_V7			((cpu_subtype_t) 9)
#endif
#ifndef CPU_SUBTYPE_ARM_V7F
	#define CPU_SUBTYPE_ARM_V7F			((cpu_subtype_t) 10)
#endif
#ifndef CPU_SUBTYPE_ARM_V7S
	#define CPU_SUBTYPE_ARM_V7S			((cpu_subtype_t) 11)
#endif
#ifndef CPU_SUBTYPE_ARM_V7K
	#define CPU_SUBTYPE_ARM_V7K			((cpu_subtype_t) 12)
#endif
#ifndef LC_DYLD_ENVIRONMENT
	#define LC_DYLD_ENVIRONMENT			0x27
#endif

#ifndef CPU_SUBTYPE_X86_64_H
	#define CPU_SUBTYPE_X86_64_H		((cpu_subtype_t) 8) 
#endif	

#ifndef VM_PROT_SLIDE   
    #define VM_PROT_SLIDE 0x20
#endif

#include <vector>
#include <algorithm>

#include "mach-o/dyld_gdb.h"

#include "dyld.h"
#include "ImageLoader.h"
#include "ImageLoaderMachO.h"
#include "dyldLibSystemInterface.h"
#include "dyldSyscallInterface.h"
#if DYLD_SHARED_CACHE_SUPPORT
#include "dyld_cache_format.h"
#endif
#if TARGET_IPHONE_SIMULATOR
  void coresymbolication_load_image(void*, const ImageLoader*, uint64_t);
  void coresymbolication_unload_image(void*, const ImageLoader*);
#else
  #include "coreSymbolicationDyldSupport.hpp"
#endif

// not libc header for send() syscall interface
extern "C" ssize_t __sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);


// ARM and x86_64 are the only architecture that use cpu-sub-types
#define CPU_SUBTYPES_SUPPORTED  ((__arm__ || __x86_64__) && !TARGET_IPHONE_SIMULATOR)



#define CPU_TYPE_MASK 0x00FFFFFF	/* complement of CPU_ARCH_MASK */


/* implemented in dyld_gdb.cpp */
extern void addImagesToAllImages(uint32_t infoCount, const dyld_image_info info[]);
extern void removeImageFromAllImages(const mach_header* mh);
extern void setAlImageInfosHalt(const char* message, uintptr_t flags);
extern void addNonSharedCacheImageUUID(const dyld_uuid_info& info);
extern const char* notifyGDB(enum dyld_image_states state, uint32_t infoCount, const dyld_image_info info[]);

// magic so CrashReporter logs message
extern "C" {
	char error_string[1024];
}
// implemented in dyldStartup.s for CrashReporter
extern "C" void dyld_fatal_error(const char* errString) __attribute__((noreturn));

// magic linker symbol for start of dyld binary
extern "C" const macho_header __dso_handle;


//
// The file contains the core of dyld used to get a process to main().  
// The API's that dyld supports are implemented in dyldAPIs.cpp.
//
//
//
//
//
namespace dyld {
	struct RegisteredDOF { const mach_header* mh; int registrationID; };
	struct DylibOverride { const char* installName; const char* override; };
}


VECTOR_NEVER_DESTRUCTED(ImageLoader*);
VECTOR_NEVER_DESTRUCTED(dyld::RegisteredDOF);
VECTOR_NEVER_DESTRUCTED(dyld::ImageCallback);
VECTOR_NEVER_DESTRUCTED(dyld::DylibOverride);
VECTOR_NEVER_DESTRUCTED(ImageLoader::DynamicReference);

VECTOR_NEVER_DESTRUCTED(dyld_image_state_change_handler);

namespace dyld {


// 
// state of all environment variables dyld uses
//
struct EnvironmentVariables {
	const char* const *			DYLD_FRAMEWORK_PATH;
	const char* const *			DYLD_FALLBACK_FRAMEWORK_PATH;
	const char* const *			DYLD_LIBRARY_PATH;
	const char* const *			DYLD_FALLBACK_LIBRARY_PATH;
	const char* const *			DYLD_INSERT_LIBRARIES;
	const char* const *			LD_LIBRARY_PATH;			// for unix conformance
	const char* const *			DYLD_VERSIONED_LIBRARY_PATH;
	const char* const *			DYLD_VERSIONED_FRAMEWORK_PATH;
	bool						DYLD_PRINT_LIBRARIES;
	bool						DYLD_PRINT_LIBRARIES_POST_LAUNCH;
	bool						DYLD_BIND_AT_LAUNCH;
	bool						DYLD_PRINT_STATISTICS;
	bool						DYLD_PRINT_OPTS;
	bool						DYLD_PRINT_ENV;
	bool						DYLD_DISABLE_DOFS;
	bool						DYLD_PRINT_CS_NOTIFICATIONS;
                            //  DYLD_SHARED_CACHE_DONT_VALIDATE ==> sSharedCacheIgnoreInodeAndTimeStamp
                            //  DYLD_SHARED_CACHE_DIR           ==> sSharedCacheDir
							//	DYLD_ROOT_PATH					==> gLinkContext.rootPaths
							//	DYLD_IMAGE_SUFFIX				==> gLinkContext.imageSuffix
							//	DYLD_PRINT_OPTS					==> gLinkContext.verboseOpts
							//	DYLD_PRINT_ENV					==> gLinkContext.verboseEnv
							//	DYLD_FORCE_FLAT_NAMESPACE		==> gLinkContext.bindFlat
							//	DYLD_PRINT_INITIALIZERS			==> gLinkContext.verboseInit
							//	DYLD_PRINT_SEGMENTS				==> gLinkContext.verboseMapping
							//	DYLD_PRINT_BINDINGS				==> gLinkContext.verboseBind
							//  DYLD_PRINT_WEAK_BINDINGS		==> gLinkContext.verboseWeakBind
							//	DYLD_PRINT_REBASINGS			==> gLinkContext.verboseRebase
							//	DYLD_PRINT_DOFS					==> gLinkContext.verboseDOF
							//	DYLD_PRINT_APIS					==> gLogAPIs
							//	DYLD_IGNORE_PREBINDING			==> gLinkContext.prebindUsage
							//	DYLD_PREBIND_DEBUG				==> gLinkContext.verbosePrebinding
							//	DYLD_NEW_LOCAL_SHARED_REGIONS	==> gLinkContext.sharedRegionMode
							//	DYLD_SHARED_REGION				==> gLinkContext.sharedRegionMode
							//	DYLD_PRINT_WARNINGS				==> gLinkContext.verboseWarnings
							//	DYLD_PRINT_RPATHS				==> gLinkContext.verboseRPaths
							//	DYLD_PRINT_INTERPOSING			==> gLinkContext.verboseInterposing
};



typedef std::vector<dyld_image_state_change_handler> StateHandlers;


enum RestrictedReason { restrictedNot, restrictedBySetGUid, restrictedBySegment, restrictedByEntitlements };
	
// all global state
static const char*					sExecPath = NULL;
static const char*					sExecShortName = NULL;
static const macho_header*			sMainExecutableMachHeader = NULL;
#if CPU_SUBTYPES_SUPPORTED
static cpu_type_t					sHostCPU;
static cpu_subtype_t				sHostCPUsubtype;
#endif
static ImageLoader*					sMainExecutable = NULL;
static bool							sProcessIsRestricted = false;
static RestrictedReason				sRestrictedReason = restrictedNot;
static size_t						sInsertedDylibCount = 0;
static std::vector<ImageLoader*>	sAllImages;
static std::vector<ImageLoader*>	sImageRoots;
static std::vector<ImageLoader*>	sImageFilesNeedingTermination;
static std::vector<RegisteredDOF>	sImageFilesNeedingDOFUnregistration;
static std::vector<ImageCallback>   sAddImageCallbacks;
static std::vector<ImageCallback>   sRemoveImageCallbacks;
static bool							sRemoveImageCallbacksInUse = false;
static void*						sSingleHandlers[7][3];
static void*						sBatchHandlers[7][3];
static ImageLoader*					sLastImageByAddressCache;
static EnvironmentVariables			sEnv;
static const char*					sFrameworkFallbackPaths[] = { "$HOME/Library/Frameworks", "/Library/Frameworks", "/Network/Library/Frameworks", "/System/Library/Frameworks", NULL };
static const char*					sLibraryFallbackPaths[] = { "$HOME/lib", "/usr/local/lib", "/usr/lib", NULL };
static UndefinedHandler				sUndefinedHandler = NULL;
static ImageLoader*					sBundleBeingLoaded = NULL;	// hack until OFI is reworked
#if DYLD_SHARED_CACHE_SUPPORT
static const dyld_cache_header*		sSharedCache = NULL;
static long							sSharedCacheSlide = 0;
static bool							sSharedCacheIgnoreInodeAndTimeStamp = false;
	   bool							gSharedCacheOverridden = false;
#if __IPHONE_OS_VERSION_MIN_REQUIRED
	static const char*				sSharedCacheDir = IPHONE_DYLD_SHARED_CACHE_DIR;
	static bool						sDylibsOverrideCache = false;
	#define ENABLE_DYLIBS_TO_OVERRIDE_CACHE_SIZE 1024
#else
	static const char*				sSharedCacheDir = MACOSX_DYLD_SHARED_CACHE_DIR;
#endif
#endif
ImageLoader::LinkContext			gLinkContext;
bool								gLogAPIs = false;
const struct LibSystemHelpers*		gLibSystemHelpers = NULL;
#if SUPPORT_OLD_CRT_INITIALIZATION
bool								gRunInitializersOldWay = false;
#endif
static std::vector<DylibOverride>	sDylibOverrides;
#if !TARGET_IPHONE_SIMULATOR	
static int							sLogSocket = -1;
#endif
static bool							sFrameworksFoundAsDylibs = false;
#if __x86_64__
static bool							sHaswell = false;
#endif
static std::vector<ImageLoader::DynamicReference> sDynamicReferences;
static bool							sLogToFile = false;
static char							sLoadingCrashMessage[1024] = "dyld: launch, loading dependent libraries";

//
// The MappedRanges structure is used for fast address->image lookups.
// The table is only updated when the dyld lock is held, so we don't
// need to worry about multiple writers.  But readers may look at this
// data without holding the lock. Therefore, all updates must be done
// in an order that will never cause readers to see inconsistent data.
// The general rule is that if the image field is non-NULL then
// the other fields are valid.
//
struct MappedRanges
{
	enum { count=400 };
	struct {
		ImageLoader*	image;
		uintptr_t		start;
		uintptr_t		end;
	} array[count];
	MappedRanges*		next;
};

static MappedRanges			sMappedRangesStart;

void addMappedRange(ImageLoader* image, uintptr_t start, uintptr_t end)
{
	//dyld::log("addMappedRange(0x%lX->0x%lX) for %s\n", start, end, image->getShortName());
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		for (int i=0; i < MappedRanges::count; ++i) {
			if ( p->array[i].image == NULL ) {
				p->array[i].start = start;
				p->array[i].end = end;
				// add image field last with a barrier so that any reader will see consistent records
				OSMemoryBarrier();
				p->array[i].image = image;
				return;
			}
		}
	}
	// table must be full, chain another
	MappedRanges* newRanges = (MappedRanges*)malloc(sizeof(MappedRanges));
	bzero(newRanges, sizeof(MappedRanges));
	newRanges->array[0].start = start;
	newRanges->array[0].end = end;
	newRanges->array[0].image = image;
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		if ( p->next == NULL ) {
			OSMemoryBarrier();
			p->next = newRanges;
			break;
		}
	}
}

void removedMappedRanges(ImageLoader* image)
{
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		for (int i=0; i < MappedRanges::count; ++i) {
			if ( p->array[i].image == image ) {
				// clear with a barrier so that any reader will see consistent records
				OSMemoryBarrier();
				p->array[i].image = NULL;
			}
		}
	}
}

ImageLoader* findMappedRange(uintptr_t target)
{
	for (MappedRanges* p = &sMappedRangesStart; p != NULL; p = p->next) {
		for (int i=0; i < MappedRanges::count; ++i) {
			if ( p->array[i].image != NULL ) {
				if ( (p->array[i].start <= target) && (target < p->array[i].end) )
					return p->array[i].image;
			}
		}
	}
	return NULL;
}



const char* mkstringf(const char* format, ...)
{
	_SIMPLE_STRING buf = _simple_salloc();
	if ( buf != NULL ) {
		va_list	list;
		va_start(list, format);
		_simple_vsprintf(buf, format, list);
		va_end(list);
		const char*	t = strdup(_simple_string(buf));
		_simple_sfree(buf);
		if ( t != NULL )
			return t;
	}
	return "mkstringf, out of memory error";
}


void throwf(const char* format, ...) 
{
	_SIMPLE_STRING buf = _simple_salloc();
	if ( buf != NULL ) {
		va_list	list;
		va_start(list, format);
		_simple_vsprintf(buf, format, list);
		va_end(list);
		const char*	t = strdup(_simple_string(buf));
		_simple_sfree(buf);
		if ( t != NULL )
			throw t;
	}
	throw "throwf, out of memory error";
}


#if !TARGET_IPHONE_SIMULATOR
static int sLogfile = STDERR_FILENO;
#endif

#if LOG_BINDINGS
static int sBindingsLogfile = -1;
static void mysprintf(char* dst, const char* format, ...)
{
	_SIMPLE_STRING buf = _simple_salloc();
	if ( buf != NULL ) {
		va_list	list;
		va_start(list, format);
		_simple_vsprintf(buf, format, list);
		va_end(list);
		strcpy(dst, _simple_string(buf));
		_simple_sfree(buf);
	}
	else {
		strcpy(dst, "out of memory");
	}
}
void logBindings(const char* format, ...) 
{
	if ( sBindingsLogfile != -1 ) {
		va_list	list;
		va_start(list, format);
		_simple_vdprintf(sBindingsLogfile, format, list);
		va_end(list);
	}
}
#endif

#if !TARGET_IPHONE_SIMULATOR	
// based on CFUtilities.c: also_do_stderr()
static bool useSyslog()
{
	// Use syslog() for processes managed by launchd
	if ( (gLibSystemHelpers != NULL) && (gLibSystemHelpers->version >= 11) ) {
		if ( (*gLibSystemHelpers->isLaunchdOwned)() ) {
			return true;
		}
	}

	// If stderr is not available, use syslog()
	struct stat sb;
	int result = fstat(STDERR_FILENO, &sb);
	if ( result < 0 )
		return true; // file descriptor 2 is closed

	return false;
}

	
static void socket_syslogv(int priority, const char* format, va_list list)
{
	// lazily create socket and connection to syslogd
	if ( sLogSocket == -1 ) {
		sLogSocket = ::socket(AF_UNIX, SOCK_DGRAM, 0);
		if (sLogSocket == -1)
			return;  // cannot log
		::fcntl(sLogSocket, F_SETFD, 1);
	
		struct sockaddr_un addr;
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, _PATH_LOG, sizeof(addr.sun_path));
		if ( ::connect(sLogSocket, (struct sockaddr *)&addr, sizeof(addr)) == -1 ) {
			::close(sLogSocket);
			sLogSocket = -1;
			return;
		}
	}
	
	// format message to syslogd like: "<priority>Process[pid]: message"
	_SIMPLE_STRING buf = _simple_salloc();
	if ( buf == NULL )
		return;
	if ( _simple_sprintf(buf, "<%d>%s[%d]: ", LOG_USER|LOG_NOTICE, sExecShortName, getpid()) == 0 ) {
		if ( _simple_vsprintf(buf, format, list) == 0 ) {
			const char* p = _simple_string(buf);
			::__sendto(sLogSocket, p, strlen(p), 0, NULL, 0);
		}
	}
	_simple_sfree(buf);
}

void vlog(const char* format, va_list list)
{
	if ( !sLogToFile && useSyslog() ) 
		socket_syslogv(LOG_ERR, format, list);
	else {
		_simple_vdprintf(sLogfile, format, list);
	}
}

void log(const char* format, ...)
{
	va_list	list;
	va_start(list, format);
	vlog(format, list);
	va_end(list);
}


void vwarn(const char* format, va_list list) 
{
	_simple_dprintf(sLogfile, "dyld: warning, ");
	_simple_vdprintf(sLogfile, format, list);
}

void warn(const char* format, ...) 
{
	va_list	list;
	va_start(list, format);
	vwarn(format, list);
	va_end(list);
}


#endif // !TARGET_IPHONE_SIMULATOR	


// <rdar://problem/8867781> control access to sAllImages through a lock 
// because global dyld lock is not held during initialization phase of dlopen()
// <rdar://problem/16145518> Use OSSpinLockLock to allow yielding
static OSSpinLock sAllImagesLock = 0;

static void allImagesLock()
{
    //dyld::log("allImagesLock()\n");
#if TARGET_IPHONE_SIMULATOR	
	// <rdar://problem/16154256> can't use OSSpinLockLock in simulator until thread_switch is provided by host dyld
	while ( ! OSAtomicCompareAndSwapPtrBarrier((void*)0, (void*)1, (void**)&sAllImagesLock) ) {
        // spin
    }
#else
	OSSpinLockLock(&sAllImagesLock);
#endif
}

static void allImagesUnlock()
{
    //dyld::log("allImagesUnlock()\n");
#if TARGET_IPHONE_SIMULATOR	
	while ( ! OSAtomicCompareAndSwapPtrBarrier((void*)1, (void*)0, (void**)&sAllImagesLock) ) {
		// spin
	}
#else
	OSSpinLockUnlock(&sAllImagesLock);
#endif
}




// utility class to assure files are closed when an exception is thrown
class FileOpener {
public:
	FileOpener(const char* path);
	~FileOpener();
	int getFileDescriptor() { return fd; }
private:
	int fd;
};

FileOpener::FileOpener(const char* path)
 : fd(-1)
{
	fd = my_open(path, O_RDONLY, 0);
}

FileOpener::~FileOpener()
{
	if ( fd != -1 )
		close(fd);
}


static void	registerDOFs(const std::vector<ImageLoader::DOFInfo>& dofs)
{
	const size_t dofSectionCount = dofs.size();
	if ( !sEnv.DYLD_DISABLE_DOFS && (dofSectionCount != 0) ) {
		int fd = open("/dev/" DTRACEMNR_HELPER, O_RDWR);
		if ( fd < 0 ) {
			//dyld::warn("can't open /dev/" DTRACEMNR_HELPER " to register dtrace DOF sections\n");
		}
		else {
			// allocate a buffer on the stack for the variable length dof_ioctl_data_t type
			uint8_t buffer[sizeof(dof_ioctl_data_t) + dofSectionCount*sizeof(dof_helper_t)];
			dof_ioctl_data_t* ioctlData = (dof_ioctl_data_t*)buffer;
			
			// fill in buffer with one dof_helper_t per DOF section
			ioctlData->dofiod_count = dofSectionCount;
			for (unsigned int i=0; i < dofSectionCount; ++i) {
				strlcpy(ioctlData->dofiod_helpers[i].dofhp_mod, dofs[i].imageShortName, DTRACE_MODNAMELEN);
				ioctlData->dofiod_helpers[i].dofhp_dof = (uintptr_t)(dofs[i].dof);
				ioctlData->dofiod_helpers[i].dofhp_addr = (uintptr_t)(dofs[i].dof);
			}
			
			// tell kernel about all DOF sections en mas
			// pass pointer to ioctlData because ioctl() only copies a fixed size amount of data into kernel
			user_addr_t val = (user_addr_t)(unsigned long)ioctlData;
			if ( ioctl(fd, DTRACEHIOC_ADDDOF, &val) != -1 ) {
				// kernel returns a unique identifier for each section in the dofiod_helpers[].dofhp_dof field.
				for (unsigned int i=0; i < dofSectionCount; ++i) {
					RegisteredDOF info;
					info.mh = dofs[i].imageHeader;
					info.registrationID = (int)(ioctlData->dofiod_helpers[i].dofhp_dof);
					sImageFilesNeedingDOFUnregistration.push_back(info);
					if ( gLinkContext.verboseDOF ) {
						dyld::log("dyld: registering DOF section %p in %s with dtrace, ID=0x%08X\n", 
							dofs[i].dof, dofs[i].imageShortName, info.registrationID);
					}
				}
			}
			else {
				//dyld::log( "dyld: ioctl to register dtrace DOF section failed\n");
			}
			close(fd);
		}
	}
}

static void	unregisterDOF(int registrationID)
{
	int fd = open("/dev/" DTRACEMNR_HELPER, O_RDWR);
	if ( fd < 0 ) {
		dyld::warn("can't open /dev/" DTRACEMNR_HELPER " to unregister dtrace DOF section\n");
	}
	else {
		ioctl(fd, DTRACEHIOC_REMOVE, registrationID);
		close(fd);
		if ( gLinkContext.verboseInit )
			dyld::warn("unregistering DOF section ID=0x%08X with dtrace\n", registrationID);
	}
}


//
// _dyld_register_func_for_add_image() is implemented as part of the general image state change notification
//
static void notifyAddImageCallbacks(ImageLoader* image)
{
	// use guard so that we cannot notify about the same image twice
	if ( ! image->addFuncNotified() ) {
		for (std::vector<ImageCallback>::iterator it=sAddImageCallbacks.begin(); it != sAddImageCallbacks.end(); it++)
			(*it)(image->machHeader(), image->getSlide());
		image->setAddFuncNotified();
	}
}



// notify gdb about these new images
static const char* updateAllImages(enum dyld_image_states state, uint32_t infoCount, const struct dyld_image_info info[])
{
	// <rdar://problem/8812589> don't add images without paths to all-image-info-list
	if ( info[0].imageFilePath != NULL )
		addImagesToAllImages(infoCount, info);
	return NULL;
}


static StateHandlers* stateToHandlers(dyld_image_states state, void* handlersArray[7][3])
{
	switch ( state ) {
		case dyld_image_state_mapped:
			return reinterpret_cast<StateHandlers*>(&handlersArray[0]);
			
		case dyld_image_state_dependents_mapped:
			return reinterpret_cast<StateHandlers*>(&handlersArray[1]);
			
		case dyld_image_state_rebased:
			return reinterpret_cast<StateHandlers*>(&handlersArray[2]);
			
		case dyld_image_state_bound:
			return reinterpret_cast<StateHandlers*>(&handlersArray[3]);
			
		case dyld_image_state_dependents_initialized:
			return reinterpret_cast<StateHandlers*>(&handlersArray[4]);

		case dyld_image_state_initialized:
			return reinterpret_cast<StateHandlers*>(&handlersArray[5]);
			
		case dyld_image_state_terminated:
			return reinterpret_cast<StateHandlers*>(&handlersArray[6]);
	}
	return NULL;
}

static void notifySingle(dyld_image_states state, const ImageLoader* image)
{
	//dyld::log("notifySingle(state=%d, image=%s)\n", state, image->getPath());
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sSingleHandlers);
	if ( handlers != NULL ) {
		dyld_image_info info;
		info.imageLoadAddress	= image->machHeader();
		info.imageFilePath		= image->getRealPath();
		info.imageFileModDate	= image->lastModified();
		for (std::vector<dyld_image_state_change_handler>::iterator it = handlers->begin(); it != handlers->end(); ++it) {
			const char* result = (*it)(state, 1, &info);
			if ( (result != NULL) && (state == dyld_image_state_mapped) ) {
				//fprintf(stderr, "  image rejected by handler=%p\n", *it);
				// make copy of thrown string so that later catch clauses can free it
				const char* str = strdup(result);
				throw str;
			}
		}
	}
	if ( state == dyld_image_state_mapped ) {
		// <rdar://problem/7008875> Save load addr + UUID for images from outside the shared cache
		if ( !image->inSharedCache() ) {
			dyld_uuid_info info;
			if ( image->getUUID(info.imageUUID) ) {
				info.imageLoadAddress = image->machHeader();
				addNonSharedCacheImageUUID(info);
			}
		}
	}
    // mach message csdlc about dynamically loaded images 
	if ( image->addFuncNotified() && (state == dyld_image_state_terminated) ) {
		if ( sEnv.DYLD_PRINT_CS_NOTIFICATIONS ) {
			dyld::log("dyld core symbolication unload notification: %p %s\n", image->machHeader(), image->getPath());
		}
		if ( dyld::gProcessInfo->coreSymbolicationShmPage != NULL) {
#if TARGET_IPHONE_SIMULATOR
			void* connection = dyld::gProcessInfo->coreSymbolicationShmPage;
			if ( *((uint32_t*)connection) == 2 ) {
#else
			CSCppDyldSharedMemoryPage* connection = (CSCppDyldSharedMemoryPage*)dyld::gProcessInfo->coreSymbolicationShmPage;
			if ( connection->is_valid_version() ) {
#endif
				coresymbolication_unload_image(connection, image);
			}
		}
	}
}




//
// Normally, dyld_all_image_infos is only updated in batches after an entire
// graph is loaded.  But if there is an error loading the initial set of
// dylibs needed by the main executable, dyld_all_image_infos is not yet set 
// up, leading to usually brief crash logs.
//
// This function manually adds the images loaded so far to dyld::gProcessInfo.
// It should only be called before terminating.
//
void syncAllImages()
{
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); ++it) {
		dyld_image_info info;
		ImageLoader* image = *it;
		info.imageLoadAddress = image->machHeader();
		info.imageFilePath = image->getRealPath();
		info.imageFileModDate = image->lastModified();
		// add to all_image_infos if not already there
		bool found = false;
		int existingCount = dyld::gProcessInfo->infoArrayCount;
		const dyld_image_info* existing = dyld::gProcessInfo->infoArray;
		if ( existing != NULL ) {
			for (int i=0; i < existingCount; ++i) {
				if ( existing[i].imageLoadAddress == info.imageLoadAddress ) {
					//dyld::log("not adding %s\n", info.imageFilePath);
					found = true;
					break;
				}
			}
		}
		if ( ! found ) {
			//dyld::log("adding %s\n", info.imageFilePath);
			addImagesToAllImages(1, &info);
		}
	}
}


static int imageSorter(const void* l, const void* r)
{
	const ImageLoader* left = *((ImageLoader**)l);
	const ImageLoader* right= *((ImageLoader**)r);
	return left->compare(right);
}

static void notifyBatchPartial(dyld_image_states state, bool orLater, dyld_image_state_change_handler onlyHandler)
{
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sBatchHandlers);
	if ( handlers != NULL ) {
		// don't use a vector because it will use malloc/free and we want notifcation to be low cost
        allImagesLock();
        ImageLoader* images[sAllImages.size()+1];
        ImageLoader** end = images;
        for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
            dyld_image_states imageState = (*it)->getState();
            if ( (imageState == state) || (orLater && (imageState > state)) )
                *end++ = *it;
        }
		if ( sBundleBeingLoaded != NULL ) {
			dyld_image_states imageState = sBundleBeingLoaded->getState();
			if ( (imageState == state) || (orLater && (imageState > state)) )
				*end++ = sBundleBeingLoaded;
		}
        const char* dontLoadReason = NULL;
		uint32_t count = (uint32_t)(end-images);
		if ( end != images ) {
			// sort bottom up
			qsort(images, count, sizeof(ImageLoader*), &imageSorter);
			// build info array
			dyld_image_info	infos[count];
			for (unsigned int i=0; i < count; ++i) {
				dyld_image_info* p = &infos[i];
				ImageLoader* image = images[i];
				//dyld::log("  state=%d, name=%s\n", state, image->getPath());
				p->imageLoadAddress = image->machHeader();
				p->imageFilePath = image->getRealPath();
				p->imageFileModDate = image->lastModified();
				// special case for add_image hook
				if ( state == dyld_image_state_bound )
					notifyAddImageCallbacks(image);
			}
			
			if ( onlyHandler != NULL ) {
				const char* result = (*onlyHandler)(state, count, infos);
				if ( (result != NULL) && (state == dyld_image_state_dependents_mapped) ) {
					//fprintf(stderr, "  images rejected by handler=%p\n", onlyHandler);
					// make copy of thrown string so that later catch clauses can free it
					dontLoadReason = strdup(result);
				}
			}
			else {
				// call each handler with whole array 
				for (std::vector<dyld_image_state_change_handler>::iterator it = handlers->begin(); it != handlers->end(); ++it) {
					const char* result = (*it)(state, count, infos);
					if ( (result != NULL) && (state == dyld_image_state_dependents_mapped) ) {
						//fprintf(stderr, "  images rejected by handler=%p\n", *it);
						// make copy of thrown string so that later catch clauses can free it
						dontLoadReason = strdup(result);
						break;
					}
				}
			}
		}
        allImagesUnlock();
        if ( dontLoadReason != NULL )
            throw dontLoadReason;
	}
	if ( state == dyld_image_state_rebased ) {
		if ( sEnv.DYLD_PRINT_CS_NOTIFICATIONS ) {
			for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
				dyld_image_states imageState = (*it)->getState();
				if ( (imageState == dyld_image_state_rebased) || (orLater && (imageState > dyld_image_state_rebased)) )
					dyld::log("dyld core symbolication load notification: %p %s\n", (*it)->machHeader(), (*it)->getPath());
			}
		}
		if ( dyld::gProcessInfo->coreSymbolicationShmPage != NULL) {
#if TARGET_IPHONE_SIMULATOR
			void* connection = dyld::gProcessInfo->coreSymbolicationShmPage;
			if ( *((uint32_t*)connection) == 2 ) {
#else
			CSCppDyldSharedMemoryPage* connection = (CSCppDyldSharedMemoryPage*)dyld::gProcessInfo->coreSymbolicationShmPage;
			if ( connection->is_valid_version() ) {
#endif
				// This needs to be captured now
				uint64_t load_timestamp = mach_absolute_time();
				for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
					dyld_image_states imageState = (*it)->getState();
					if ( (imageState == state) || (orLater && (imageState > state)) )
						coresymbolication_load_image(connection, *it, load_timestamp);
				}
			}
		}
	}
}



static void notifyBatch(dyld_image_states state)
{
	notifyBatchPartial(state, false, NULL);
}

// In order for register_func_for_add_image() callbacks to to be called bottom up,
// we need to maintain a list of root images. The main executable is usally the
// first root. Any images dynamically added are also roots (unless already loaded).
// If DYLD_INSERT_LIBRARIES is used, those libraries are first.
static void addRootImage(ImageLoader* image)
{
	//dyld::log("addRootImage(%p, %s)\n", image, image->getPath());
	// add to list of roots
	sImageRoots.push_back(image);
}


static void clearAllDepths()
{
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++)
		(*it)->clearDepth();
}

static void printAllDepths()
{
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++)
		dyld::log("%03d %s\n",  (*it)->getDepth(), (*it)->getShortName());
}


static unsigned int imageCount()
{
	return (unsigned int)sAllImages.size();
}


static void setNewProgramVars(const ProgramVars& newVars)
{
	// make a copy of the pointers to program variables
	gLinkContext.programVars = newVars;
	
	// now set each program global to their initial value
	*gLinkContext.programVars.NXArgcPtr = gLinkContext.argc;
	*gLinkContext.programVars.NXArgvPtr = gLinkContext.argv;
	*gLinkContext.programVars.environPtr = gLinkContext.envp;
	*gLinkContext.programVars.__prognamePtr = gLinkContext.progname;
}

#if SUPPORT_OLD_CRT_INITIALIZATION
static void setRunInitialzersOldWay()
{
	gRunInitializersOldWay = true;		
}
#endif

static void addDynamicReference(ImageLoader* from, ImageLoader* to) {
	// don't add dynamic reference if either are in the shared cache
	if( from->inSharedCache() )
		return;
	if( to->inSharedCache() )
		return;

	// don't add dynamic reference if there already is a static one
	if ( from->dependsOn(to) )
		return;
	
	// don't add if this combination already exists
	for (std::vector<ImageLoader::DynamicReference>::iterator it=sDynamicReferences.begin(); it != sDynamicReferences.end(); ++it) {
		if ( (it->from == from) && (it->to == to) )
			return;
	}
	//dyld::log("addDynamicReference(%s, %s\n", from->getShortName(), to->getShortName());
	ImageLoader::DynamicReference t;
	t.from = from;
	t.to = to;
	sDynamicReferences.push_back(t);
}

static void addImage(ImageLoader* image)
{
	// add to master list
    allImagesLock();
        sAllImages.push_back(image);
    allImagesUnlock();
	
	// update mapped ranges
	uintptr_t lastSegStart = 0;
	uintptr_t lastSegEnd = 0;
	for(unsigned int i=0, e=image->segmentCount(); i < e; ++i) {
		if ( image->segUnaccessible(i) ) 
			continue;
		uintptr_t start = image->segActualLoadAddress(i);
		uintptr_t end = image->segActualEndAddress(i);
		if ( start == lastSegEnd ) {
			// two segments are contiguous, just record combined segments
			lastSegEnd = end;
		}
		else {
			// non-contiguous segments, record last (if any)
			if ( lastSegEnd != 0 )
				addMappedRange(image, lastSegStart, lastSegEnd);
			lastSegStart = start;
			lastSegEnd = end;
		}		
	}
	if ( lastSegEnd != 0 )
		addMappedRange(image, lastSegStart, lastSegEnd);

	
	if ( sEnv.DYLD_PRINT_LIBRARIES || (sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH && (sMainExecutable!=NULL) && sMainExecutable->isLinked()) ) {
		dyld::log("dyld: loaded: %s\n", image->getPath());
	}
	
}

//
// Helper for std::remove_if
//
class RefUsesImage {
public:
	RefUsesImage(ImageLoader* image) : _image(image) {}
	bool operator()(const ImageLoader::DynamicReference& ref) const {
		return ( (ref.from == _image) || (ref.to == _image) );
	}
private:
	ImageLoader* _image;
};



void removeImage(ImageLoader* image)
{
	// if has dtrace DOF section, tell dtrace it is going away, then remove from sImageFilesNeedingDOFUnregistration
	for (std::vector<RegisteredDOF>::iterator it=sImageFilesNeedingDOFUnregistration.begin(); it != sImageFilesNeedingDOFUnregistration.end(); ) {
		if ( it->mh == image->machHeader() ) {
			unregisterDOF(it->registrationID);
			sImageFilesNeedingDOFUnregistration.erase(it);
			// don't increment iterator, the erase caused next element to be copied to where this iterator points
		}
		else {
			++it;
		}
	}
	
	// tell all registered remove image handlers about this
	// do this before removing image from internal data structures so that the callback can query dyld about the image
	if ( image->getState() >= dyld_image_state_bound ) {
		sRemoveImageCallbacksInUse = true; // This only runs inside dyld's global lock, so ok to use a global for the in-use flag.
		for (std::vector<ImageCallback>::iterator it=sRemoveImageCallbacks.begin(); it != sRemoveImageCallbacks.end(); it++) {
			(*it)(image->machHeader(), image->getSlide());
		}
		sRemoveImageCallbacksInUse = false;
	}
	
	// notify 
	notifySingle(dyld_image_state_terminated, image);
	
	// remove from mapped images table
	removedMappedRanges(image);

	// remove from master list
    allImagesLock();
        for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
            if ( *it == image ) {
                sAllImages.erase(it);
                break;
            }
        }
    allImagesUnlock();
	
	// remove from sDynamicReferences
	sDynamicReferences.erase(std::remove_if(sDynamicReferences.begin(), sDynamicReferences.end(), RefUsesImage(image)), sDynamicReferences.end());

	// flush find-by-address cache (do this after removed from master list, so there is no chance it can come back)
	if ( sLastImageByAddressCache == image )
		sLastImageByAddressCache = NULL;

	// if in root list, pull it out 
	for (std::vector<ImageLoader*>::iterator it=sImageRoots.begin(); it != sImageRoots.end(); it++) {
		if ( *it == image ) {
			sImageRoots.erase(it);
			break;
		}
	}

	// log if requested
	if ( sEnv.DYLD_PRINT_LIBRARIES || (sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH && (sMainExecutable!=NULL) && sMainExecutable->isLinked()) ) {
		dyld::log("dyld: unloaded: %s\n", image->getPath());
	}

	// tell gdb, new way
	removeImageFromAllImages(image->machHeader());
}


void runImageStaticTerminators(ImageLoader* image)
{
	// if in termination list, pull it out and run terminator
	bool mightBeMore;
	do {
		mightBeMore = false;
		for (std::vector<ImageLoader*>::iterator it=sImageFilesNeedingTermination.begin(); it != sImageFilesNeedingTermination.end(); it++) {
			if ( *it == image ) {
				sImageFilesNeedingTermination.erase(it);
				if (gLogAPIs) dyld::log("dlclose(), running static terminators for %p %s\n", image, image->getShortName());
				image->doTermination(gLinkContext);
				mightBeMore = true;
				break;
			}
		}
	} while ( mightBeMore );
}

static void terminationRecorder(ImageLoader* image)
{
	sImageFilesNeedingTermination.push_back(image);
}

const char* getExecutablePath()
{
	return sExecPath;
}

static void runAllStaticTerminators(void* extra)
{
	try {
		const size_t imageCount = sImageFilesNeedingTermination.size();
		for(size_t i=imageCount; i > 0; --i){
			ImageLoader* image = sImageFilesNeedingTermination[i-1];
			image->doTermination(gLinkContext);
		}
		sImageFilesNeedingTermination.clear();
		notifyBatch(dyld_image_state_terminated);
	}
	catch (const char* msg) {
		halt(msg);
	}
}

void initializeMainExecutable()
{
	// record that we've reached this step
	gLinkContext.startedInitializingMainExecutable = true;

	// run initialzers for any inserted dylibs
	ImageLoader::InitializerTimingList initializerTimes[sAllImages.size()];
	initializerTimes[0].count = 0;
	const size_t rootCount = sImageRoots.size();
	if ( rootCount > 1 ) {
		for(size_t i=1; i < rootCount; ++i) {
			sImageRoots[i]->runInitializers(gLinkContext, initializerTimes[0]);
		}
	}
	
	// run initializers for main executable and everything it brings up 
	sMainExecutable->runInitializers(gLinkContext, initializerTimes[0]);
	
	// register cxa_atexit() handler to run static terminators in all loaded images when this process exits
	if ( gLibSystemHelpers != NULL ) 
		(*gLibSystemHelpers->cxa_atexit)(&runAllStaticTerminators, NULL, NULL);

	// dump info if requested
	if ( sEnv.DYLD_PRINT_STATISTICS )
		ImageLoaderMachO::printStatistics((unsigned int)sAllImages.size(), initializerTimes[0]);
}

bool mainExecutablePrebound()
{
	return sMainExecutable->usablePrebinding(gLinkContext);
}

ImageLoader* mainExecutable()
{
	return sMainExecutable;
}




#if SUPPORT_VERSIONED_PATHS

// forward reference
static bool getDylibVersionAndInstallname(const char* dylibPath, uint32_t* version, char* installName);


//
// Examines a dylib file and if its current_version is newer than the installed
// dylib at its install_name, then add the dylib file to sDylibOverrides.
//
static void checkDylibOverride(const char* dylibFile)
{
	//dyld::log("checkDylibOverride('%s')\n", dylibFile);
	uint32_t altVersion;
 	char sysInstallName[PATH_MAX];
	if ( getDylibVersionAndInstallname(dylibFile, &altVersion, sysInstallName) ) {
		//dyld::log("%s has version 0x%08X and install name %s\n", dylibFile, altVersion, sysInstallName);
		uint32_t sysVersion;
		if ( getDylibVersionAndInstallname(sysInstallName, &sysVersion, NULL) ) {
			//dyld::log("%s has version 0x%08X\n", sysInstallName, sysVersion);
			if ( altVersion > sysVersion ) {
				//dyld::log("override found: %s -> %s\n", sysInstallName, dylibFile);
				// see if there already is an override for this dylib
				bool entryExists = false;
				for (std::vector<DylibOverride>::iterator it = sDylibOverrides.begin(); it != sDylibOverrides.end(); ++it) {
					if ( strcmp(it->installName, sysInstallName) == 0 ) {
						entryExists = true;
						uint32_t prevVersion;
						if ( getDylibVersionAndInstallname(it->override, &prevVersion, NULL) ) {
							if ( altVersion > prevVersion ) {
								// found an even newer override
								free((void*)(it->override));
								char resolvedPath[PATH_MAX];
								if ( realpath(dylibFile, resolvedPath) != NULL )
									it->override = strdup(resolvedPath);
								else
									it->override = strdup(dylibFile);
								break;
							}
						}
					}
				}
				if ( ! entryExists ) {
					DylibOverride entry;
					entry.installName = strdup(sysInstallName);
					char resolvedPath[PATH_MAX];
					if ( realpath(dylibFile, resolvedPath) != NULL )
						entry.override = strdup(resolvedPath);
					else
						entry.override = strdup(dylibFile);
					sDylibOverrides.push_back(entry);
					//dyld::log("added override: %s -> %s\n", entry.installName, entry.override);
				}
			}
		}
	}
	
}

static void checkDylibOverridesInDir(const char* dirPath)
{
	//dyld::log("checkDylibOverridesInDir('%s')\n", dirPath);
	char dylibPath[PATH_MAX];
	int dirPathLen = strlen(dirPath);
	strlcpy(dylibPath, dirPath, PATH_MAX); 
	DIR* dirp = opendir(dirPath);
	if ( dirp != NULL) {
		dirent entry;
		dirent* entp = NULL;
		while ( readdir_r(dirp, &entry, &entp) == 0 ) {
			if ( entp == NULL )
				break;
			if ( entp->d_type != DT_REG ) 
				continue;
			dylibPath[dirPathLen] = '/';     
			dylibPath[dirPathLen+1] = '\0';     
			if ( strlcat(dylibPath, entp->d_name, PATH_MAX) > PATH_MAX ) 
				continue;
			checkDylibOverride(dylibPath);
		}
		closedir(dirp);
	}
}


static void checkFrameworkOverridesInDir(const char* dirPath)
{
	//dyld::log("checkFrameworkOverridesInDir('%s')\n", dirPath);
	char frameworkPath[PATH_MAX];
	int dirPathLen = strlen(dirPath);
	strlcpy(frameworkPath, dirPath, PATH_MAX); 
	DIR* dirp = opendir(dirPath);
	if ( dirp != NULL) {
		dirent entry;
		dirent* entp = NULL;
		while ( readdir_r(dirp, &entry, &entp) == 0 ) {
			if ( entp == NULL )
				break;
			if ( entp->d_type != DT_DIR ) 
				continue;
			frameworkPath[dirPathLen] = '/';     
			frameworkPath[dirPathLen+1] = '\0';
			int dirNameLen = strlen(entp->d_name);
			if ( dirNameLen < 11 )
				continue;
			if ( strcmp(&entp->d_name[dirNameLen-10], ".framework") != 0 )
				continue;
			if ( strlcat(frameworkPath, entp->d_name, PATH_MAX) > PATH_MAX ) 
				continue;
			if ( strlcat(frameworkPath, "/", PATH_MAX) > PATH_MAX ) 
				continue;
			if ( strlcat(frameworkPath, entp->d_name, PATH_MAX) > PATH_MAX ) 
				continue;
			frameworkPath[strlen(frameworkPath)-10] = '\0';
			checkDylibOverride(frameworkPath);
		}
		closedir(dirp);
	}
}
#endif // SUPPORT_VERSIONED_PATHS


//
// Turns a colon separated list of strings into a NULL terminated array 
// of string pointers. If mainExecutableDir param is not NULL,
// substitutes @loader_path with main executable's dir.
//
static const char** parseColonList(const char* list, const char* mainExecutableDir)
{
	static const char* sEmptyList[] = { NULL };

	if ( list[0] == '\0' ) 
		return sEmptyList;
	
	int colonCount = 0;
	for(const char* s=list; *s != '\0'; ++s) {
		if (*s == ':') 
			++colonCount;
	}
	
	int index = 0;
	const char* start = list;
	char** result = new char*[colonCount+2];
	for(const char* s=list; *s != '\0'; ++s) {
		if (*s == ':') {
			size_t len = s-start;
			if ( (mainExecutableDir != NULL) && (strncmp(start, "@loader_path/", 13) == 0) ) {
				size_t mainExecDirLen = strlen(mainExecutableDir);
				char* str = new char[mainExecDirLen+len+1];
				strcpy(str, mainExecutableDir);
				strlcat(str, &start[13], mainExecDirLen+len+1);
				str[mainExecDirLen+len-13] = '\0';
				start = &s[1];
				result[index++] = str;
			}
			else if ( (mainExecutableDir != NULL) && (strncmp(start, "@executable_path/", 17) == 0) ) {
				size_t mainExecDirLen = strlen(mainExecutableDir);
				char* str = new char[mainExecDirLen+len+1];
				strcpy(str, mainExecutableDir);
				strlcat(str, &start[17], mainExecDirLen+len+1);
				str[mainExecDirLen+len-17] = '\0';
				start = &s[1];
				result[index++] = str;
			}
			else {
				char* str = new char[len+1];
				strncpy(str, start, len);
				str[len] = '\0';
				start = &s[1];
				result[index++] = str;
			}
		}
	}
	size_t len = strlen(start);
	if ( (mainExecutableDir != NULL) && (strncmp(start, "@loader_path/", 13) == 0) ) {
		size_t mainExecDirLen = strlen(mainExecutableDir);
		char* str = new char[mainExecDirLen+len+1];
		strcpy(str, mainExecutableDir);
		strlcat(str, &start[13], mainExecDirLen+len+1);
		str[mainExecDirLen+len-13] = '\0';
		result[index++] = str;
	}
	else if ( (mainExecutableDir != NULL) && (strncmp(start, "@executable_path/", 17) == 0) ) {
		size_t mainExecDirLen = strlen(mainExecutableDir);
		char* str = new char[mainExecDirLen+len+1];
		strcpy(str, mainExecutableDir);
		strlcat(str, &start[17], mainExecDirLen+len+1);
		str[mainExecDirLen+len-17] = '\0';
		result[index++] = str;
	}
	else {
		char* str = new char[len+1];
		strcpy(str, start);
		result[index++] = str;
	}
	result[index] = NULL;
	
	//dyld::log("parseColonList(%s)\n", list);
	//for(int i=0; result[i] != NULL; ++i)
	//	dyld::log("  %s\n", result[i]);
	return (const char**)result;
}

static void	appendParsedColonList(const char* list, const char* mainExecutableDir, const char* const ** storage)
{
	const char** newlist = parseColonList(list, mainExecutableDir);
	if ( *storage == NULL ) {
		// first time, just set
		*storage = newlist;
	}
	else {
		// need to append to existing list
		const char* const* existing = *storage;
		int count = 0;
		for(int i=0; existing[i] != NULL; ++i)
			++count;
		for(int i=0; newlist[i] != NULL; ++i)
			++count;
		const char** combinedList = new const char*[count+2];
		int index = 0;
		for(int i=0; existing[i] != NULL; ++i)
			combinedList[index++] = existing[i];
		for(int i=0; newlist[i] != NULL; ++i)
			combinedList[index++] = newlist[i];
		combinedList[index] = NULL;
		// leak old arrays
		*storage = combinedList;
	}
}

 
static void paths_expand_roots(const char **paths, const char *key, const char *val)
{
// 	assert(val != NULL);
// 	assert(paths != NULL);
	if(NULL != key) {
		size_t keyLen = strlen(key);
		for(int i=0; paths[i] != NULL; ++i) {
			if ( strncmp(paths[i], key, keyLen) == 0 ) {
				char* newPath = new char[strlen(val) + (strlen(paths[i]) - keyLen) + 1];
				strcpy(newPath, val);
				strcat(newPath, &paths[i][keyLen]);
				paths[i] = newPath;
			}
		}
	}
	return;
}

static void removePathWithPrefix(const char* paths[], const char* prefix)
{
    size_t prefixLen = strlen(prefix);
    int skip = 0;
    int i;
    for(i = 0; paths[i] != NULL; ++i) {
        if ( strncmp(paths[i], prefix, prefixLen) == 0 )
            ++skip;
        else
            paths[i-skip] = paths[i];
    }
    paths[i-skip] = NULL;
}


#if 0
static void paths_dump(const char **paths)
{
//   assert(paths != NULL);
  const char **strs = paths;
  while(*strs != NULL)
  {
    dyld::log("\"%s\"\n", *strs);
    strs++;
  }
  return;
}
#endif

static void printOptions(const char* argv[])
{
	uint32_t i = 0;
	while ( NULL != argv[i] ) {
		dyld::log("opt[%i] = \"%s\"\n", i, argv[i]);
		i++;
	}
}

static void printEnvironmentVariables(const char* envp[])
{
	while ( NULL != *envp ) {
		dyld::log("%s\n", *envp);
		envp++;
	}
}

void processDyldEnvironmentVariable(const char* key, const char* value, const char* mainExecutableDir)
{
	if ( strcmp(key, "DYLD_FRAMEWORK_PATH") == 0 ) {
		appendParsedColonList(value, mainExecutableDir, &sEnv.DYLD_FRAMEWORK_PATH);
	}
	else if ( strcmp(key, "DYLD_FALLBACK_FRAMEWORK_PATH") == 0 ) {
		appendParsedColonList(value, mainExecutableDir, &sEnv.DYLD_FALLBACK_FRAMEWORK_PATH);
	}
	else if ( strcmp(key, "DYLD_LIBRARY_PATH") == 0 ) {
		appendParsedColonList(value, mainExecutableDir, &sEnv.DYLD_LIBRARY_PATH);
	}
	else if ( strcmp(key, "DYLD_FALLBACK_LIBRARY_PATH") == 0 ) {
		appendParsedColonList(value, mainExecutableDir, &sEnv.DYLD_FALLBACK_LIBRARY_PATH);
	}
	else if ( (strcmp(key, "DYLD_ROOT_PATH") == 0) || (strcmp(key, "DYLD_PATHS_ROOT") == 0) ) {
		if ( strcmp(value, "/") != 0 ) {
			gLinkContext.rootPaths = parseColonList(value, mainExecutableDir);
			for (int i=0; gLinkContext.rootPaths[i] != NULL; ++i) {
				if ( gLinkContext.rootPaths[i][0] != '/' ) {
					dyld::warn("DYLD_ROOT_PATH not used because it contains a non-absolute path\n");
					gLinkContext.rootPaths = NULL;
					break;
				}
			}
		}
	} 
	else if ( strcmp(key, "DYLD_IMAGE_SUFFIX") == 0 ) {
		gLinkContext.imageSuffix = value;
	}
	else if ( strcmp(key, "DYLD_INSERT_LIBRARIES") == 0 ) {
		sEnv.DYLD_INSERT_LIBRARIES = parseColonList(value, NULL);
	}
	else if ( strcmp(key, "DYLD_PRINT_OPTS") == 0 ) {
		sEnv.DYLD_PRINT_OPTS = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_ENV") == 0 ) {
		sEnv.DYLD_PRINT_ENV = true;
	}
	else if ( strcmp(key, "DYLD_DISABLE_DOFS") == 0 ) {
		sEnv.DYLD_DISABLE_DOFS = true;
	}
	else if ( strcmp(key, "DYLD_DISABLE_PREFETCH") == 0 ) {
		gLinkContext.preFetchDisabled = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_LIBRARIES") == 0 ) {
		sEnv.DYLD_PRINT_LIBRARIES = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_LIBRARIES_POST_LAUNCH") == 0 ) {
		sEnv.DYLD_PRINT_LIBRARIES_POST_LAUNCH = true;
	}
	else if ( strcmp(key, "DYLD_BIND_AT_LAUNCH") == 0 ) {
		sEnv.DYLD_BIND_AT_LAUNCH = true;
	}
	else if ( strcmp(key, "DYLD_FORCE_FLAT_NAMESPACE") == 0 ) {
		gLinkContext.bindFlat = true;
	}
	else if ( strcmp(key, "DYLD_NEW_LOCAL_SHARED_REGIONS") == 0 ) {
		// ignore, no longer relevant but some scripts still set it
	}
	else if ( strcmp(key, "DYLD_NO_FIX_PREBINDING") == 0 ) {
	}
	else if ( strcmp(key, "DYLD_PREBIND_DEBUG") == 0 ) {
		gLinkContext.verbosePrebinding = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_INITIALIZERS") == 0 ) {
		gLinkContext.verboseInit = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_DOFS") == 0 ) {
		gLinkContext.verboseDOF = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_STATISTICS") == 0 ) {
		sEnv.DYLD_PRINT_STATISTICS = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_SEGMENTS") == 0 ) {
		gLinkContext.verboseMapping = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_BINDINGS") == 0 ) {
		gLinkContext.verboseBind = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_WEAK_BINDINGS") == 0 ) {
		gLinkContext.verboseWeakBind = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_REBASINGS") == 0 ) {
		gLinkContext.verboseRebase = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_APIS") == 0 ) {
		gLogAPIs = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_WARNINGS") == 0 ) {
		gLinkContext.verboseWarnings = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_RPATHS") == 0 ) {
		gLinkContext.verboseRPaths = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_CS_NOTIFICATIONS") == 0 ) {
		sEnv.DYLD_PRINT_CS_NOTIFICATIONS = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_INTERPOSING") == 0 ) {
		gLinkContext.verboseInterposing = true;
	}
	else if ( strcmp(key, "DYLD_PRINT_CODE_SIGNATURES") == 0 ) {
		gLinkContext.verboseCodeSignatures = true;
	}
	else if ( strcmp(key, "DYLD_SHARED_REGION") == 0 ) {
		if ( strcmp(value, "private") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUsePrivateSharedRegion;
		}
		else if ( strcmp(value, "avoid") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		}
		else if ( strcmp(value, "use") == 0 ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUseSharedRegion;
		}
		else if ( value[0] == '\0' ) {
			gLinkContext.sharedRegionMode = ImageLoader::kUseSharedRegion;
		}
		else {
			dyld::warn("unknown option to DYLD_SHARED_REGION.  Valid options are: use, private, avoid\n");
		}
	}
#if DYLD_SHARED_CACHE_SUPPORT
	else if ( strcmp(key, "DYLD_SHARED_CACHE_DIR") == 0 ) {
        sSharedCacheDir = value;
	}
	else if ( strcmp(key, "DYLD_SHARED_CACHE_DONT_VALIDATE") == 0 ) {
		sSharedCacheIgnoreInodeAndTimeStamp = true;
	}
#endif
	else if ( strcmp(key, "DYLD_IGNORE_PREBINDING") == 0 ) {
		if ( strcmp(value, "all") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseNoPrebinding;
		}
		else if ( strcmp(value, "app") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseAllButAppPredbinding;
		}
		else if ( strcmp(value, "nonsplit") == 0 ) {
			gLinkContext.prebindUsage = ImageLoader::kUseSplitSegPrebinding;
		}
		else if ( value[0] == '\0' ) {
			gLinkContext.prebindUsage = ImageLoader::kUseSplitSegPrebinding;
		}
		else {
			dyld::warn("unknown option to DYLD_IGNORE_PREBINDING.  Valid options are: all, app, nonsplit\n");
		}
	}
#if SUPPORT_VERSIONED_PATHS
	else if ( strcmp(key, "DYLD_VERSIONED_LIBRARY_PATH") == 0 ) {
		appendParsedColonList(value, mainExecutableDir, &sEnv.DYLD_VERSIONED_LIBRARY_PATH);
	}
	else if ( strcmp(key, "DYLD_VERSIONED_FRAMEWORK_PATH") == 0 ) {
		appendParsedColonList(value, mainExecutableDir, &sEnv.DYLD_VERSIONED_FRAMEWORK_PATH);
	}
#endif
	else if ( strcmp(key, "DYLD_PRINT_TO_FILE") == 0 ) {
		// handled in _main()
	}
	else {
		dyld::warn("unknown environment variable: %s\n", key);
	}
}


#if SUPPORT_LC_DYLD_ENVIRONMENT
static void checkLoadCommandEnvironmentVariables()
{
	// <rdar://problem/8440934> Support augmenting dyld environment variables in load commands
	const uint32_t cmd_count = sMainExecutableMachHeader->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)sMainExecutableMachHeader)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_DYLD_ENVIRONMENT:
			{
				const struct dylinker_command* envcmd = (struct dylinker_command*)cmd;
				const char* keyEqualsValue = (char*)envcmd + envcmd->name.offset;
				char mainExecutableDir[strlen(sExecPath)+2];
				strcpy(mainExecutableDir, sExecPath);
				char* lastSlash = strrchr(mainExecutableDir, '/');
				if ( lastSlash != NULL)
					lastSlash[1] = '\0';
				// only process variables that start with DYLD_ and end in _PATH
				if ( (strncmp(keyEqualsValue, "DYLD_", 5) == 0) ) {
					const char* equals = strchr(keyEqualsValue, '=');
					if ( equals != NULL ) {
						if ( strncmp(&equals[-5], "_PATH", 5) == 0 ) {
							const char* value = &equals[1];
							const size_t keyLen = equals-keyEqualsValue;
							char key[keyLen+1];
							strncpy(key, keyEqualsValue, keyLen);
							key[keyLen] = '\0';
							//dyld::log("processing: %s\n", keyEqualsValue);
							//dyld::log("mainExecutableDir: %s\n", mainExecutableDir);
							processDyldEnvironmentVariable(key, value, mainExecutableDir);
						}
					}
				}
			}
			break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}
#endif // SUPPORT_LC_DYLD_ENVIRONMENT	

	
static bool hasCodeSignatureLoadCommand(const macho_header* mh)
{
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		if (cmd->cmd == LC_CODE_SIGNATURE) 
			return true;
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	return false;
}
	

#if SUPPORT_VERSIONED_PATHS
static void checkVersionedPaths()
{
	// search DYLD_VERSIONED_LIBRARY_PATH directories for dylibs and check if they are newer
	if ( sEnv.DYLD_VERSIONED_LIBRARY_PATH != NULL ) {
		for(const char* const* lp = sEnv.DYLD_VERSIONED_LIBRARY_PATH; *lp != NULL; ++lp) {
			checkDylibOverridesInDir(*lp);
		}
	}
	
	// search DYLD_VERSIONED_FRAMEWORK_PATH directories for dylibs and check if they are newer
	if ( sEnv.DYLD_VERSIONED_FRAMEWORK_PATH != NULL ) {
		for(const char* const* fp = sEnv.DYLD_VERSIONED_FRAMEWORK_PATH; *fp != NULL; ++fp) {
			checkFrameworkOverridesInDir(*fp);
		}
	}
}
#endif	


//
// For security, setuid programs ignore DYLD_* environment variables.
// Additionally, the DYLD_* enviroment variables are removed
// from the environment, so that any child processes don't see them.
//
static void pruneEnvironmentVariables(const char* envp[], const char*** applep)
{
	// delete all DYLD_* and LD_LIBRARY_PATH environment variables
	int removedCount = 0;
	const char** d = envp;
	for(const char** s = envp; *s != NULL; s++) {
	    if ( (strncmp(*s, "DYLD_", 5) != 0) && (strncmp(*s, "LD_LIBRARY_PATH=", 16) != 0) ) {
			*d++ = *s;
		}
		else {
			++removedCount;
		}
	}
	*d++ = NULL;
// <rdar://11894054> Disable warnings about DYLD_ env vars being ignored.  The warnings are causing too much confusion.
#if 0
	if ( removedCount != 0 ) {
		dyld::log("dyld: DYLD_ environment variables being ignored because ");
		switch (sRestrictedReason) {
			case restrictedNot:
				break;
			case restrictedBySetGUid:
				dyld::log("main executable (%s) is setuid or setgid\n", sExecPath);
				break;
			case restrictedBySegment:
				dyld::log("main executable (%s) has __RESTRICT/__restrict section\n", sExecPath);
				break;
			case restrictedByEntitlements:
				dyld::log("main executable (%s) is code signed with entitlements\n", sExecPath);
				break;
		}
	}
#endif
	// slide apple parameters
	if ( removedCount > 0 ) {
		*applep = d;
		do {
			*d = d[removedCount];
		} while ( *d++ != NULL );
		for(int i=0; i < removedCount; ++i)
			*d++ = NULL;
	}
	
	// disable framework and library fallback paths for setuid binaries rdar://problem/4589305
	sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = NULL;
	sEnv.DYLD_FALLBACK_LIBRARY_PATH = NULL;

	if ( removedCount > 0 )
		strlcat(sLoadingCrashMessage, ", ignoring DYLD_* env vars", sizeof(sLoadingCrashMessage));
}


static void checkEnvironmentVariables(const char* envp[], bool ignoreEnviron)
{
	const char* home = NULL;
	const char** p;
	for(p = envp; *p != NULL; p++) {
		const char* keyEqualsValue = *p;
	    if ( strncmp(keyEqualsValue, "DYLD_", 5) == 0 ) {
			const char* equals = strchr(keyEqualsValue, '=');
			if ( (equals != NULL) && !ignoreEnviron ) {
				strlcat(sLoadingCrashMessage, "\n", sizeof(sLoadingCrashMessage));
				strlcat(sLoadingCrashMessage, keyEqualsValue, sizeof(sLoadingCrashMessage));
				const char* value = &equals[1];
				const size_t keyLen = equals-keyEqualsValue;
				char key[keyLen+1];
				strncpy(key, keyEqualsValue, keyLen);
				key[keyLen] = '\0';
				processDyldEnvironmentVariable(key, value, NULL);
			}
		}
	    else if ( strncmp(keyEqualsValue, "HOME=", 5) == 0 ) {
			home = &keyEqualsValue[5];
		}
		else if ( strncmp(keyEqualsValue, "LD_LIBRARY_PATH=", 16) == 0 ) {
			const char* path = &keyEqualsValue[16];
			sEnv.LD_LIBRARY_PATH = parseColonList(path, NULL);
		}
	}

#if SUPPORT_LC_DYLD_ENVIRONMENT
	checkLoadCommandEnvironmentVariables();
#endif // SUPPORT_LC_DYLD_ENVIRONMENT	
	
	// default value for DYLD_FALLBACK_FRAMEWORK_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_FRAMEWORK_PATH == NULL ) {
		const char** paths = sFrameworkFallbackPaths;
		if ( home == NULL )
			removePathWithPrefix(paths, "$HOME");
		else
			paths_expand_roots(paths, "$HOME", home);
		sEnv.DYLD_FALLBACK_FRAMEWORK_PATH = paths;
	}

	// default value for DYLD_FALLBACK_LIBRARY_PATH, if not set in environment
	if ( sEnv.DYLD_FALLBACK_LIBRARY_PATH == NULL ) {
		const char** paths = sLibraryFallbackPaths;
		if ( home == NULL ) 
			removePathWithPrefix(paths, "$HOME");
		else
			paths_expand_roots(paths, "$HOME", home);
		sEnv.DYLD_FALLBACK_LIBRARY_PATH = paths;
	}
	
	// <rdar://problem/11281064> DYLD_IMAGE_SUFFIX and DYLD_ROOT_PATH cannot be used together
	if ( (gLinkContext.imageSuffix != NULL) && (gLinkContext.rootPaths != NULL) ) {
		dyld::warn("Ignoring DYLD_IMAGE_SUFFIX because DYLD_ROOT_PATH is used.\n");
		gLinkContext.imageSuffix = NULL;
	}
	
#if SUPPORT_VERSIONED_PATHS
	checkVersionedPaths();
#endif	
}


static void getHostInfo()
{
#if CPU_SUBTYPES_SUPPORTED
#if __ARM_ARCH_7K__
	sHostCPU		= CPU_TYPE_ARM;
	sHostCPUsubtype = CPU_SUBTYPE_ARM_V7K;
#elif __ARM_ARCH_7A__
	sHostCPU		= CPU_TYPE_ARM;
	sHostCPUsubtype = CPU_SUBTYPE_ARM_V7;
#elif __ARM_ARCH_6K__
	sHostCPU		= CPU_TYPE_ARM;
	sHostCPUsubtype = CPU_SUBTYPE_ARM_V6;
#elif __ARM_ARCH_7F__
	sHostCPU		= CPU_TYPE_ARM;
	sHostCPUsubtype = CPU_SUBTYPE_ARM_V7F;
#elif __ARM_ARCH_7S__
	sHostCPU		= CPU_TYPE_ARM;
	sHostCPUsubtype = CPU_SUBTYPE_ARM_V7S;
#else
	struct host_basic_info info;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	mach_port_t hostPort = mach_host_self();
	kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
	if ( result != KERN_SUCCESS )
		throw "host_info() failed";
	sHostCPU		= info.cpu_type;
	sHostCPUsubtype = info.cpu_subtype;
	mach_port_deallocate(mach_task_self(), hostPort);
#endif
#endif
}

static void checkSharedRegionDisable()
{
#if __MAC_OS_X_VERSION_MIN_REQUIRED
	// if main executable has segments that overlap the shared region, 
	// then disable using the shared region
	if ( sMainExecutable->overlapsWithAddressRange((void*)(uintptr_t)SHARED_REGION_BASE, (void*)(uintptr_t)(SHARED_REGION_BASE + SHARED_REGION_SIZE)) ) {
		gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
		if ( gLinkContext.verboseMapping )
			dyld::warn("disabling shared region because main executable overlaps\n");
	}
#if __i386__
	if ( sProcessIsRestricted ) {
		// <rdar://problem/15280847> use private or no shared region for suid processes
		gLinkContext.sharedRegionMode = ImageLoader::kUsePrivateSharedRegion;
	}
#endif
#endif
	// iPhoneOS cannot run without shared region
}

bool validImage(const ImageLoader* possibleImage)
{
    const size_t imageCount = sAllImages.size();
    for(size_t i=0; i < imageCount; ++i) {
        if ( possibleImage == sAllImages[i] ) {
            return true;
        }
    }
	return false;
}

uint32_t getImageCount()
{
	return (uint32_t)sAllImages.size();
}

ImageLoader* getIndexedImage(unsigned int index)
{
	if ( index < sAllImages.size() )
		return sAllImages[index];
	return NULL;
}

ImageLoader* findImageByMachHeader(const struct mach_header* target)
{
	return findMappedRange((uintptr_t)target);
}


ImageLoader* findImageContainingAddress(const void* addr)
{
	return findMappedRange((uintptr_t)addr);
}


ImageLoader* findImageContainingSymbol(const void* symbol)
{
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* anImage = *it;
		if ( anImage->containsSymbol(symbol) )
			return anImage;
	}
	return NULL;
}



void forEachImageDo( void (*callback)(ImageLoader*, void* userData), void* userData)
{
	const size_t imageCount = sAllImages.size();
	for(size_t i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		(*callback)(anImage, userData);
	}
}

ImageLoader* findLoadedImage(const struct stat& stat_buf)
{
	const size_t imageCount = sAllImages.size();
	for(size_t i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		if ( anImage->statMatch(stat_buf) )
			return anImage;
	}
	return NULL;
}

// based on ANSI-C strstr()
static const char* strrstr(const char* str, const char* sub) 
{
	const size_t sublen = strlen(sub);
	for(const char* p = &str[strlen(str)]; p != str; --p) {
		if ( strncmp(p, sub, sublen) == 0 )
			return p;
	}
	return NULL;
}


//
// Find framework path
//
//  /path/foo.framework/foo								=>   foo.framework/foo	
//  /path/foo.framework/Versions/A/foo					=>   foo.framework/Versions/A/foo
//  /path/foo.framework/Frameworks/bar.framework/bar	=>   bar.framework/bar
//  /path/foo.framework/Libraries/bar.dylb				=>   NULL
//  /path/foo.framework/bar								=>   NULL
//
// Returns NULL if not a framework path
//
static const char* getFrameworkPartialPath(const char* path)
{
	const char* dirDot = strrstr(path, ".framework/");
	if ( dirDot != NULL ) {
		const char* dirStart = dirDot;
		for ( ; dirStart >= path; --dirStart) {
			if ( (*dirStart == '/') || (dirStart == path) ) {
				const char* frameworkStart = &dirStart[1];
				if ( dirStart == path )
					--frameworkStart;
				size_t len = dirDot - frameworkStart;
				char framework[len+1];
				strncpy(framework, frameworkStart, len);
				framework[len] = '\0';
				const char* leaf = strrchr(path, '/');
				if ( leaf != NULL ) {
					if ( strcmp(framework, &leaf[1]) == 0 ) {
						return frameworkStart;
					}
					if (  gLinkContext.imageSuffix != NULL ) {
						// some debug frameworks have install names that end in _debug
						if ( strncmp(framework, &leaf[1], len) == 0 ) {
							if ( strcmp( gLinkContext.imageSuffix, &leaf[len+1]) == 0 )
								return frameworkStart;
						}
					}
				}
			}
		}
	}
	return NULL;
}


static const char* getLibraryLeafName(const char* path)
{
	const char* start = strrchr(path, '/');
	if ( start != NULL )
		return &start[1];
	else
		return path;
}


// only for architectures that use cpu-sub-types
#if CPU_SUBTYPES_SUPPORTED 

const cpu_subtype_t CPU_SUBTYPE_END_OF_LIST = -1;


//
//	A fat file may contain multiple sub-images for the same CPU type.
//	In that case, dyld picks which sub-image to use by scanning a table
//	of preferred cpu-sub-types for the running cpu.  
//	
//	There is one row in the table for each cpu-sub-type on which dyld might run.
//  The first entry in a row is that cpu-sub-type.  It is followed by all
//	cpu-sub-types that can run on that cpu, if preferred order.  Each row ends with 
//	a "SUBTYPE_ALL" (to denote that images written to run on any cpu-sub-type are usable), 
//  followed by one or more CPU_SUBTYPE_END_OF_LIST to pad out this row.
//


#if __arm__
//      
//     ARM sub-type lists
//
const int kARM_RowCount = 8;
static const cpu_subtype_t kARM[kARM_RowCount][9] = { 

	// armv7f can run: v7f, v7, v6, v5, and v4
	{  CPU_SUBTYPE_ARM_V7F, CPU_SUBTYPE_ARM_V7, CPU_SUBTYPE_ARM_V6, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST },

	// armv7k can run: v7k
	{  CPU_SUBTYPE_ARM_V7K, CPU_SUBTYPE_END_OF_LIST },

	// armv7s can run: v7s, v7, v7f, v7k, v6, v5, and v4
	{  CPU_SUBTYPE_ARM_V7S, CPU_SUBTYPE_ARM_V7, CPU_SUBTYPE_ARM_V7F, CPU_SUBTYPE_ARM_V6, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST },

	// armv7 can run: v7, v6, v5, and v4
	{  CPU_SUBTYPE_ARM_V7, CPU_SUBTYPE_ARM_V6, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST },
	
	// armv6 can run: v6, v5, and v4
	{  CPU_SUBTYPE_ARM_V6, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
	
	// xscale can run: xscale, v5, and v4
	{  CPU_SUBTYPE_ARM_XSCALE, CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
	
	// armv5 can run: v5 and v4
	{  CPU_SUBTYPE_ARM_V5TEJ, CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },

	// armv4 can run: v4
	{  CPU_SUBTYPE_ARM_V4T, CPU_SUBTYPE_ARM_ALL, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST, CPU_SUBTYPE_END_OF_LIST },
};
#endif

#if __x86_64__
//      
//     x86_64 sub-type lists
//
const int kX86_64_RowCount = 2;
static const cpu_subtype_t kX86_64[kX86_64_RowCount][5] = {

	// x86_64h can run: x86_64h, x86_64h(lib), x86_64(lib), and x86_64
	{ CPU_SUBTYPE_X86_64_H, CPU_SUBTYPE_LIB64|CPU_SUBTYPE_X86_64_H, CPU_SUBTYPE_LIB64|CPU_SUBTYPE_X86_64_ALL, CPU_SUBTYPE_X86_64_ALL,  CPU_SUBTYPE_END_OF_LIST },

	// x86_64 can run: x86_64(lib) and x86_64
	{ CPU_SUBTYPE_X86_64_ALL, CPU_SUBTYPE_LIB64|CPU_SUBTYPE_X86_64_ALL, CPU_SUBTYPE_END_OF_LIST },

};
#endif


// scan the tables above to find the cpu-sub-type-list for this machine
static const cpu_subtype_t* findCPUSubtypeList(cpu_type_t cpu, cpu_subtype_t subtype)
{
	switch (cpu) {
#if __arm__
		case CPU_TYPE_ARM:
			for (int i=0; i < kARM_RowCount ; ++i) {
				if ( kARM[i][0] == subtype )
					return kARM[i];
			}
			break;
#endif
#if __x86_64__
		case CPU_TYPE_X86_64:
			for (int i=0; i < kX86_64_RowCount ; ++i) {
				if ( kX86_64[i][0] == subtype )
					return kX86_64[i];
			}
			break;
#endif
	}
	return NULL;
}




// scan fat table-of-contents for best most preferred subtype
static bool fatFindBestFromOrderedList(cpu_type_t cpu, const cpu_subtype_t list[], const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* const archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for (uint32_t subTypeIndex=0; list[subTypeIndex] != CPU_SUBTYPE_END_OF_LIST; ++subTypeIndex) {
		for(uint32_t fatIndex=0; fatIndex < OSSwapBigToHostInt32(fh->nfat_arch); ++fatIndex) {
			if ( ((cpu_type_t)OSSwapBigToHostInt32(archs[fatIndex].cputype) == cpu) 
				&& (list[subTypeIndex] == (cpu_subtype_t)OSSwapBigToHostInt32(archs[fatIndex].cpusubtype)) ) {
				*offset = OSSwapBigToHostInt32(archs[fatIndex].offset);
				*len = OSSwapBigToHostInt32(archs[fatIndex].size);
				return true;
			}
		}
	}
	return false;
}

// scan fat table-of-contents for exact match of cpu and cpu-sub-type
static bool fatFindExactMatch(cpu_type_t cpu, cpu_subtype_t subtype, const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( ((cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == cpu)
			&& ((cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == subtype) ) {
			*offset = OSSwapBigToHostInt32(archs[i].offset);
			*len = OSSwapBigToHostInt32(archs[i].size);
			return true;
		}
	}
	return false;
}

// scan fat table-of-contents for image with matching cpu-type and runs-on-all-sub-types
static bool fatFindRunsOnAllCPUs(cpu_type_t cpu, const fat_header* fh, uint64_t* offset, uint64_t* len)
{
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( (cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == cpu) {
			switch (cpu) {
#if __arm__
				case CPU_TYPE_ARM:
					if ( (cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == CPU_SUBTYPE_ARM_ALL ) {
						*offset = OSSwapBigToHostInt32(archs[i].offset);
						*len = OSSwapBigToHostInt32(archs[i].size);
						return true;
					}
					break;
#endif
#if __x86_64__
				case CPU_TYPE_X86_64:
					if ( (cpu_subtype_t)OSSwapBigToHostInt32(archs[i].cpusubtype) == CPU_SUBTYPE_X86_64_ALL ) {
						*offset = OSSwapBigToHostInt32(archs[i].offset);
						*len = OSSwapBigToHostInt32(archs[i].size);
						return true;
					}
					break;
#endif
			}
		}
	}
	return false;
}

#endif // CPU_SUBTYPES_SUPPORTED

//
// A fat file may contain multiple sub-images for the same cpu-type,
// each optimized for a different cpu-sub-type (e.g G3 or G5).
// This routine picks the optimal sub-image.
//
static bool fatFindBest(const fat_header* fh, uint64_t* offset, uint64_t* len)
{
#if CPU_SUBTYPES_SUPPORTED
	// assume all dylibs loaded must have same cpu type as main executable
	const cpu_type_t cpu = sMainExecutableMachHeader->cputype;

	// We only know the subtype to use if the main executable cpu type matches the host
	if ( (cpu & CPU_TYPE_MASK) == sHostCPU ) {
		// get preference ordered list of subtypes
		const cpu_subtype_t* subTypePreferenceList = findCPUSubtypeList(cpu, sHostCPUsubtype);
	
		// use ordered list to find best sub-image in fat file
		if ( subTypePreferenceList != NULL ) 
			return fatFindBestFromOrderedList(cpu, subTypePreferenceList, fh, offset, len);
		
		// if running cpu is not in list, try for an exact match
		if ( fatFindExactMatch(cpu, sHostCPUsubtype, fh, offset, len) )
			return true;
	}
	
	// running on an uknown cpu, can only load generic code
	return fatFindRunsOnAllCPUs(cpu, fh, offset, len);
#else
	// just find first slice with matching architecture
	const fat_arch* archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
	for(uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
		if ( (cpu_type_t)OSSwapBigToHostInt32(archs[i].cputype) == sMainExecutableMachHeader->cputype) {
			*offset = OSSwapBigToHostInt32(archs[i].offset);
			*len = OSSwapBigToHostInt32(archs[i].size);
			return true;
		}
	}
	return false;
#endif
}



//
// This is used to validate if a non-fat (aka thin or raw) mach-o file can be used
// on the current processor. //
bool isCompatibleMachO(const uint8_t* firstPage, const char* path)
{
#if CPU_SUBTYPES_SUPPORTED
	// It is deemed compatible if any of the following are true:
	//  1) mach_header subtype is in list of compatible subtypes for running processor
	//  2) mach_header subtype is same as running processor subtype
	//  3) mach_header subtype runs on all processor variants
	const mach_header* mh = (mach_header*)firstPage;
	if ( mh->magic == sMainExecutableMachHeader->magic ) {
		if ( mh->cputype == sMainExecutableMachHeader->cputype ) {
			if ( (mh->cputype & CPU_TYPE_MASK) == sHostCPU ) {
				// get preference ordered list of subtypes that this machine can use
				const cpu_subtype_t* subTypePreferenceList = findCPUSubtypeList(mh->cputype, sHostCPUsubtype);
				if ( subTypePreferenceList != NULL ) {
					// if image's subtype is in the list, it is compatible
					for (const cpu_subtype_t* p = subTypePreferenceList; *p != CPU_SUBTYPE_END_OF_LIST; ++p) {
						if ( *p == mh->cpusubtype )
							return true;
					}
					// have list and not in list, so not compatible
					throwf("incompatible cpu-subtype: 0x%08X in %s", mh->cpusubtype, path);
				}
				// unknown cpu sub-type, but if exact match for current subtype then ok to use
				if ( mh->cpusubtype == sHostCPUsubtype ) 
					return true;
			}
			
			// cpu type has no ordered list of subtypes
			switch (mh->cputype) {
				case CPU_TYPE_I386:
				case CPU_TYPE_X86_64:
					// subtypes are not used or these architectures
					return true;
			}
		}
	}
#else
	// For architectures that don't support cpu-sub-types
	// this just check the cpu type.
	const mach_header* mh = (mach_header*)firstPage;
	if ( mh->magic == sMainExecutableMachHeader->magic ) {
		if ( mh->cputype == sMainExecutableMachHeader->cputype ) {
			return true;
		}
	}
#endif
	return false;
}




// The kernel maps in main executable before dyld gets control.  We need to 
// make an ImageLoader* for the already mapped in main executable.
static ImageLoader* instantiateFromLoadedImage(const macho_header* mh, uintptr_t slide, const char* path)
{
	// try mach-o loader
	if ( isCompatibleMachO((const uint8_t*)mh, path) ) {
		ImageLoader* image = ImageLoaderMachO::instantiateMainExecutable(mh, slide, path, gLinkContext);
		addImage(image);
		return image;
	}
	
	throw "main executable not a known format";
}


#if DYLD_SHARED_CACHE_SUPPORT
static bool findInSharedCacheImage(const char* path, bool searchByPath, const struct stat* stat_buf, const macho_header** mh, const char** pathInCache, long* slide)
{
	if ( sSharedCache != NULL ) {
#if __MAC_OS_X_VERSION_MIN_REQUIRED	
		// Mac OS X always requires inode/mtime to valid cache
		// if stat() not done yet, do it now
		struct stat statb;
		if ( stat_buf == NULL ) {
			if ( my_stat(path, &statb) == -1 )
				return false;
			stat_buf = &statb;
		}
#endif
#if __IPHONE_OS_VERSION_MIN_REQUIRED	
		uint64_t hash = 0;
		for (const char* s=path; *s != '\0'; ++s)
			hash += hash*4 + *s;
#endif

		// walk shared cache to see if there is a cached image that matches the inode/mtime/path desired
		const dyld_cache_image_info* const start = (dyld_cache_image_info*)((uint8_t*)sSharedCache + sSharedCache->imagesOffset);
		const dyld_cache_image_info* const end = &start[sSharedCache->imagesCount];
#if __IPHONE_OS_VERSION_MIN_REQUIRED	
		const bool cacheHasHashInfo = (start->modTime == 0);
#endif
		for( const dyld_cache_image_info* p = start; p != end; ++p) {
#if __IPHONE_OS_VERSION_MIN_REQUIRED	
			// just check path
			const char* aPath = (char*)sSharedCache + p->pathFileOffset;
			if ( cacheHasHashInfo && (p->inode != hash) )
				continue;
			if ( strcmp(path, aPath) == 0 ) {
				// found image in cache
				*mh = (macho_header*)(p->address+sSharedCacheSlide);
				*pathInCache = aPath;
				*slide = sSharedCacheSlide;
				return true;
			}
#elif __MAC_OS_X_VERSION_MIN_REQUIRED
			// check mtime and inode first because it is fast
			bool inodeMatch = ( ((time_t)p->modTime == stat_buf->st_mtime) && ((ino_t)p->inode == stat_buf->st_ino) );
			if ( searchByPath || sSharedCacheIgnoreInodeAndTimeStamp || inodeMatch ) {
				// mod-time and inode match an image in the shared cache, now check path
				const char* aPath = (char*)sSharedCache + p->pathFileOffset;
				bool cacheHit = (strcmp(path, aPath) == 0);
				if ( inodeMatch && !cacheHit ) {
					// path does not match install name of dylib in cache, but inode and mtime does match
					// perhaps path is a symlink to the cached dylib
					struct stat pathInCacheStatBuf;
					if ( my_stat(aPath, &pathInCacheStatBuf) != -1 )
						cacheHit = ( (pathInCacheStatBuf.st_dev == stat_buf->st_dev) && (pathInCacheStatBuf.st_ino == stat_buf->st_ino) );	
				}
				if ( cacheHit ) {
					// found image in cache, return info
					*mh = (macho_header*)(p->address+sSharedCacheSlide);
					//dyld::log("findInSharedCacheImage(), mh=%p, p->address=0x%0llX, slid=0x%0lX, path=%p\n", 
					//	*mh, p->address, sSharedCacheSlide, aPath);
					*pathInCache = aPath;
					*slide = sSharedCacheSlide;
					return true;
				}
			}
#endif
		}	
	}
	return false;
}

bool inSharedCache(const char* path)
{
	const macho_header* mhInCache;
	const char*			pathInCache;
	long				slide;
	return findInSharedCacheImage(path, true, NULL, &mhInCache, &pathInCache, &slide);
}

#endif

static ImageLoader* checkandAddImage(ImageLoader* image, const LoadContext& context)
{
	// now sanity check that this loaded image does not have the same install path as any existing image
	const char* loadedImageInstallPath = image->getInstallPath();
	if ( image->isDylib() && (loadedImageInstallPath != NULL) && (loadedImageInstallPath[0] == '/') ) {
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* anImage = *it;
			const char* installPath = anImage->getInstallPath();
			if ( installPath != NULL) {
				if ( strcmp(loadedImageInstallPath, installPath) == 0 ) {
					//dyld::log("duplicate(%s) => %p\n", installPath, anImage);
					removeImage(image);
					ImageLoader::deleteImage(image);
					return anImage;
				}
			}
		}
	}

	// some API's restrict what they can load
	if ( context.mustBeBundle && !image->isBundle() )
		throw "not a bundle";
	if ( context.mustBeDylib && !image->isDylib() )
		throw "not a dylib";

	// regular main executables cannot be loaded 
	if ( image->isExecutable() ) {
		if ( !context.canBePIE || !image->isPositionIndependentExecutable() )
			throw "can't load a main executable";
	}
	
	// don't add bundles to global list, they can be loaded but not linked.  When linked it will be added to list
	if ( ! image->isBundle() ) 
		addImage(image);
	
	return image;
}

#if TARGET_IPHONE_SIMULATOR	
static bool isSimulatorBinary(const uint8_t* firstPage, const char* path)
{
	const macho_header* mh = (macho_header*)firstPage;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* const cmdsReadEnd = (struct load_command*)(((char*)mh)+4096);
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_VERSION_MIN_IPHONEOS:
				return true;
			case LC_VERSION_MIN_MACOSX:
				// grandfather in a few libSystem dylibs
				if ( strncmp(path, "/usr/lib/system/libsystem_", 26) == 0 )
					return true;
				return false;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		if ( cmd > cmdsReadEnd )
			return true;
	}
	return false;
}
#endif

// map in file and instantiate an ImageLoader
static ImageLoader* loadPhase6(int fd, const struct stat& stat_buf, const char* path, const LoadContext& context)
{
	//dyld::log("%s(%s)\n", __func__ , path);
	uint64_t fileOffset = 0;
	uint64_t fileLength = stat_buf.st_size;

	// validate it is a file (not directory)
	if ( (stat_buf.st_mode & S_IFMT) != S_IFREG ) 
		throw "not a file";

	uint8_t firstPage[4096];
	bool shortPage = false;
	
	// min mach-o file is 4K
	if ( fileLength < 4096 ) {
		if ( pread(fd, firstPage, fileLength, 0) != (ssize_t)fileLength )
			throwf("pread of short file failed: %d", errno);
		shortPage = true;
	} 
	else {
		if ( pread(fd, firstPage, 4096,0) != 4096 )
			throwf("pread of first 4K failed: %d", errno);
	}
	
	// if fat wrapper, find usable sub-file
	const fat_header* fileStartAsFat = (fat_header*)firstPage;
	if ( fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		if ( fatFindBest(fileStartAsFat, &fileOffset, &fileLength) ) {
			if ( (fileOffset+fileLength) > (uint64_t)(stat_buf.st_size) )
				throwf("truncated fat file.  file length=%llu, but needed slice goes to %llu", stat_buf.st_size, fileOffset+fileLength);
			if (pread(fd, firstPage, 4096, fileOffset) != 4096)
				throwf("pread of fat file failed: %d", errno);
		}
		else {
			throw "no matching architecture in universal wrapper";
		}
	}
	
	// try mach-o loader
	if ( shortPage ) 
		throw "file too short";
	if ( isCompatibleMachO(firstPage, path) ) {

		// only MH_BUNDLE, MH_DYLIB, and some MH_EXECUTE can be dynamically loaded
		switch ( ((mach_header*)firstPage)->filetype ) {
			case MH_EXECUTE:
			case MH_DYLIB:
			case MH_BUNDLE:
				break;
			default:
				throw "mach-o, but wrong filetype";
		}
		
#if TARGET_IPHONE_SIMULATOR	
		// <rdar://problem/14168872> dyld_sim should restrict loading osx binaries
		if ( !isSimulatorBinary(firstPage, path) ) {
			throw "mach-o, but not built for iOS simulator";
		}
#endif

		// instantiate an image
		ImageLoader* image = ImageLoaderMachO::instantiateFromFile(path, fd, firstPage, fileOffset, fileLength, stat_buf, gLinkContext);
		
		// validate
		return checkandAddImage(image, context);
	}
	
	// try other file formats here...
	
	
	// throw error about what was found
	switch (*(uint32_t*)firstPage) {
		case MH_MAGIC:
		case MH_CIGAM:
		case MH_MAGIC_64:
		case MH_CIGAM_64:
			throw "mach-o, but wrong architecture";
		default:
		throwf("unknown file type, first eight bytes: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X", 
			firstPage[0], firstPage[1], firstPage[2], firstPage[3], firstPage[4], firstPage[5], firstPage[6],firstPage[7]);
	}
}


static ImageLoader* loadPhase5open(const char* path, const LoadContext& context, const struct stat& stat_buf, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);

	// open file (automagically closed when this function exits)
	FileOpener file(path);
		
	// just return NULL if file not found, but record any other errors
	if ( file.getFileDescriptor() == -1 ) {
		int err = errno;
		if ( err != ENOENT ) {
			const char* newMsg = dyld::mkstringf("%s: open() failed with errno=%d", path, err);
			exceptions->push_back(newMsg);
		}
		return NULL;
	}

	try {
		return loadPhase6(file.getFileDescriptor(), stat_buf, path, context);
	}
	catch (const char* msg) {
		const char* newMsg = dyld::mkstringf("%s: %s", path, msg);
		exceptions->push_back(newMsg);
		free((void*)msg);
		return NULL;
	}
}


#if __MAC_OS_X_VERSION_MIN_REQUIRED	
static ImageLoader* loadPhase5load(const char* path, const char* orgPath, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;

	// just return NULL if file not found, but record any other errors
	struct stat stat_buf;
	if ( my_stat(path, &stat_buf) == -1 ) {
		int err = errno;
		if ( err != ENOENT ) {
			exceptions->push_back(dyld::mkstringf("%s: stat() failed with errno=%d", path, err));
		}
		return NULL;
	}
	
	// in case image was renamed or found via symlinks, check for inode match
	image = findLoadedImage(stat_buf);
	if ( image != NULL )
		return image;
	
	// do nothing if not already loaded and if RTLD_NOLOAD or NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED
	if ( context.dontLoad )
		return NULL;

#if DYLD_SHARED_CACHE_SUPPORT
	// see if this image is in shared cache
	const macho_header* mhInCache;
	const char*			pathInCache;
	long				slideInCache;
	if ( findInSharedCacheImage(path, false, &stat_buf, &mhInCache, &pathInCache, &slideInCache) ) {
		image = ImageLoaderMachO::instantiateFromCache(mhInCache, pathInCache, slideInCache, stat_buf, gLinkContext);
		return checkandAddImage(image, context);
	}
#endif
	// file exists and is not in dyld shared cache, so open it
	return loadPhase5open(path, context, stat_buf, exceptions);
}
#endif // __MAC_OS_X_VERSION_MIN_REQUIRED



#if __IPHONE_OS_VERSION_MIN_REQUIRED 
static ImageLoader* loadPhase5stat(const char* path, const LoadContext& context, struct stat* stat_buf, 
									int* statErrNo, bool* imageFound, std::vector<const char*>* exceptions)
{
	ImageLoader* image = NULL;
	*imageFound = false;
	*statErrNo = 0;
	if ( my_stat(path, stat_buf) == 0 ) {
		// in case image was renamed or found via symlinks, check for inode match
		image = findLoadedImage(*stat_buf);
		if ( image != NULL ) {
			*imageFound = true;
			return image;
		}
		// do nothing if not already loaded and if RTLD_NOLOAD 
		if ( context.dontLoad ) {
			*imageFound = true;
			return NULL;
		}
		image = loadPhase5open(path, context, *stat_buf, exceptions);
		if ( image != NULL ) {
			*imageFound = true;
			return image;
		}
	}
	else {
		*statErrNo = errno;
	}
	return NULL;
}

// try to open file
static ImageLoader* loadPhase5load(const char* path, const char* orgPath, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	struct stat stat_buf;
	bool imageFound;
	int statErrNo;
	ImageLoader* image;
#if DYLD_SHARED_CACHE_SUPPORT
	if ( sDylibsOverrideCache ) {
		// flag is set that allows installed framework roots to override dyld shared cache
		image = loadPhase5stat(path, context, &stat_buf, &statErrNo, &imageFound, exceptions);
		if ( imageFound )
			return image;
	}
	// see if this image is in shared cache
	const macho_header* mhInCache;
	const char*			pathInCache;
	long				slideInCache;
	if ( findInSharedCacheImage(path, true, NULL, &mhInCache, &pathInCache, &slideInCache) ) {
		// see if this image in the cache was already loaded via a different path
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); ++it) {
			ImageLoader* anImage = *it;
			if ( (const macho_header*)anImage->machHeader() == mhInCache )
				return anImage;
		}
		// do nothing if not already loaded and if RTLD_NOLOAD 
		if ( context.dontLoad )
			return NULL;
		// nope, so instantiate a new image from dyld shared cache
		// <rdar://problem/7014995> zero out stat buffer so mtime, etc are zero for items from the shared cache
		bzero(&stat_buf, sizeof(stat_buf));
		image = ImageLoaderMachO::instantiateFromCache(mhInCache, pathInCache, slideInCache, stat_buf, gLinkContext);
		return checkandAddImage(image, context);
	}
	
	if ( !sDylibsOverrideCache ) {
		// flag is not set, and not in cache to try opening it
		image = loadPhase5stat(path, context, &stat_buf, &statErrNo, &imageFound, exceptions);
		if ( imageFound )
			return image;
	}
#else
	image = loadPhase5stat(path, context, &stat_buf, &statErrNo, &imageFound, exceptions);
	if ( imageFound )
		return image;
#endif
	// just return NULL if file not found, but record any other errors
	if ( (statErrNo != ENOENT) && (statErrNo != 0) ) {
		exceptions->push_back(dyld::mkstringf("%s: stat() failed with errno=%d", path, statErrNo));
	}
	return NULL;
}
#endif // __IPHONE_OS_VERSION_MIN_REQUIRED


// look for path match with existing loaded images
static ImageLoader* loadPhase5check(const char* path, const char* orgPath, const LoadContext& context)
{
	//dyld::log("%s(%s, %s)\n", __func__ , path, orgPath);
	// search path against load-path and install-path of all already loaded images
	uint32_t hash = ImageLoader::hash(path);
	//dyld::log("check() hash=%d, path=%s\n", hash, path);
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* anImage = *it;
		// check hash first to cut down on strcmp calls
		//dyld::log("    check() hash=%d, path=%s\n", anImage->getPathHash(), anImage->getPath());
		if ( anImage->getPathHash() == hash ) {
			if ( strcmp(path, anImage->getPath()) == 0 ) {
				// if we are looking for a dylib don't return something else
				if ( !context.mustBeDylib || anImage->isDylib() )
					return anImage;
			}
		}
		if ( context.matchByInstallName || anImage->matchInstallPath() ) {
			const char* installPath = anImage->getInstallPath();
			if ( installPath != NULL) {
				if ( strcmp(path, installPath) == 0 ) {
					// if we are looking for a dylib don't return something else
					if ( !context.mustBeDylib || anImage->isDylib() )
						return anImage;
				}
			}
		}
		// an install name starting with @rpath should match by install name, not just real path
		if ( (orgPath[0] == '@') && (strncmp(orgPath, "@rpath/", 7) == 0) ) {
			const char* installPath = anImage->getInstallPath();
			if ( installPath != NULL) {
				if ( !context.mustBeDylib || anImage->isDylib() ) {
					if ( strcmp(orgPath, installPath) == 0 )
						return anImage;
				}
			}
		}
	}
	
	//dyld::log("%s(%s) => NULL\n", __func__,   path);
	return NULL;
}


// open or check existing
static ImageLoader* loadPhase5(const char* path, const char* orgPath, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	
	// check for specific dylib overrides
	for (std::vector<DylibOverride>::iterator it = sDylibOverrides.begin(); it != sDylibOverrides.end(); ++it) {
		if ( strcmp(it->installName, path) == 0 ) {
			path = it->override;
			break;
		}
	}
	
	if ( exceptions != NULL ) 
		return loadPhase5load(path, orgPath, context, exceptions);
	else
		return loadPhase5check(path, orgPath, context);
}

// try with and without image suffix
static ImageLoader* loadPhase4(const char* path, const char* orgPath, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;
	if (  gLinkContext.imageSuffix != NULL ) {
		char pathWithSuffix[strlen(path)+strlen( gLinkContext.imageSuffix)+2];
		ImageLoader::addSuffix(path,  gLinkContext.imageSuffix, pathWithSuffix);
		image = loadPhase5(pathWithSuffix, orgPath, context, exceptions);
	}
	if ( image == NULL )
		image = loadPhase5(path, orgPath, context, exceptions);
	return image;
}

static ImageLoader* loadPhase2(const char* path, const char* orgPath, const LoadContext& context, 
							   const char* const frameworkPaths[], const char* const libraryPaths[], 
							   std::vector<const char*>* exceptions); // forward reference


// expand @ variables
static ImageLoader* loadPhase3(const char* path, const char* orgPath, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;
	if ( strncmp(path, "@executable_path/", 17) == 0 ) {
		// executable_path cannot be in used in any binary in a setuid process rdar://problem/4589305
		if ( sProcessIsRestricted ) 
			throwf("unsafe use of @executable_path in %s with restricted binary", context.origin);
		// handle @executable_path path prefix
		const char* executablePath = sExecPath;
		char newPath[strlen(executablePath) + strlen(path)];
		strcpy(newPath, executablePath);
		char* addPoint = strrchr(newPath,'/');
		if ( addPoint != NULL )
			strcpy(&addPoint[1], &path[17]);
		else
			strcpy(newPath, &path[17]);
		image = loadPhase4(newPath, orgPath, context, exceptions);
		if ( image != NULL ) 
			return image;

		// perhaps main executable path is a sym link, find realpath and retry
		char resolvedPath[PATH_MAX];
		if ( realpath(sExecPath, resolvedPath) != NULL ) {
			char newRealPath[strlen(resolvedPath) + strlen(path)];
			strcpy(newRealPath, resolvedPath);
			char* addPoint = strrchr(newRealPath,'/');
			if ( addPoint != NULL )
				strcpy(&addPoint[1], &path[17]);
			else
				strcpy(newRealPath, &path[17]);
			image = loadPhase4(newRealPath, orgPath, context, exceptions);
			if ( image != NULL ) 
				return image;
		}
	}
	else if ( (strncmp(path, "@loader_path/", 13) == 0) && (context.origin != NULL) ) {
		// @loader_path cannot be used from the main executable of a setuid process rdar://problem/4589305
		if ( sProcessIsRestricted && (strcmp(context.origin, sExecPath) == 0) )
			throwf("unsafe use of @loader_path in %s with restricted binary", context.origin);
		// handle @loader_path path prefix
		char newPath[strlen(context.origin) + strlen(path)];
		strcpy(newPath, context.origin);
		char* addPoint = strrchr(newPath,'/');
		if ( addPoint != NULL )
			strcpy(&addPoint[1], &path[13]);
		else
			strcpy(newPath, &path[13]);
		image = loadPhase4(newPath, orgPath, context, exceptions);
		if ( image != NULL ) 
			return image;
		
		// perhaps loader path is a sym link, find realpath and retry
		char resolvedPath[PATH_MAX];
		if ( realpath(context.origin, resolvedPath) != NULL ) {
			char newRealPath[strlen(resolvedPath) + strlen(path)];
			strcpy(newRealPath, resolvedPath);
			char* addPoint = strrchr(newRealPath,'/');
			if ( addPoint != NULL )
				strcpy(&addPoint[1], &path[13]);
			else
				strcpy(newRealPath, &path[13]);
			image = loadPhase4(newRealPath, orgPath, context, exceptions);
			if ( image != NULL ) 
				return image;
		}
	}
	else if ( context.implicitRPath || (strncmp(path, "@rpath/", 7) == 0) ) {
		const char* trailingPath = (strncmp(path, "@rpath/", 7) == 0) ? &path[7] : path;
		// substitute @rpath with all -rpath paths up the load chain
		for(const ImageLoader::RPathChain* rp=context.rpath; rp != NULL; rp=rp->next) {
			if (rp->paths != NULL ) {
				for(std::vector<const char*>::iterator it=rp->paths->begin(); it != rp->paths->end(); ++it) {
					const char* anRPath = *it;
					char newPath[strlen(anRPath) + strlen(trailingPath)+2];
					strcpy(newPath, anRPath);
					strcat(newPath, "/"); 
					strcat(newPath, trailingPath); 
					image = loadPhase4(newPath, orgPath, context, exceptions);
					if ( gLinkContext.verboseRPaths && (exceptions != NULL) ) {
						if ( image != NULL ) 
							dyld::log("RPATH successful expansion of %s to: %s\n", orgPath, newPath);
						else
							dyld::log("RPATH failed to expanding     %s to: %s\n", orgPath, newPath);
					}
					if ( image != NULL ) 
						return image;
				}
			}
		}
		
		// substitute @rpath with LD_LIBRARY_PATH
		if ( sEnv.LD_LIBRARY_PATH != NULL ) {
			image = loadPhase2(trailingPath, orgPath, context, NULL, sEnv.LD_LIBRARY_PATH, exceptions);
			if ( image != NULL )
				return image;
		}
		
		// if this is the "open" pass, don't try to open @rpath/... as a relative path
		if ( (exceptions != NULL) && (trailingPath != path) )
			return NULL;
	}
	else if (sProcessIsRestricted && (path[0] != '/' )) {
		throwf("unsafe use of relative rpath %s in %s with restricted binary", path, context.origin);
	}
	
	return loadPhase4(path, orgPath, context, exceptions);
}


// try search paths
static ImageLoader* loadPhase2(const char* path, const char* orgPath, const LoadContext& context, 
							   const char* const frameworkPaths[], const char* const libraryPaths[], 
							   std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;
	const char* frameworkPartialPath = getFrameworkPartialPath(path);
	if ( frameworkPaths != NULL ) {
		if ( frameworkPartialPath != NULL ) {
			const size_t frameworkPartialPathLen = strlen(frameworkPartialPath);
			for(const char* const* fp = frameworkPaths; *fp != NULL; ++fp) {
				char npath[strlen(*fp)+frameworkPartialPathLen+8];
				strcpy(npath, *fp);
				strcat(npath, "/");
				strcat(npath, frameworkPartialPath);
				//dyld::log("dyld: fallback framework path used: %s() -> loadPhase4(\"%s\", ...)\n", __func__, npath);
				image = loadPhase4(npath, orgPath, context, exceptions);
				if ( image != NULL )
					return image;
			}
		}
	}
	// <rdar://problem/12649639> An executable with the same name as a framework & DYLD_LIBRARY_PATH pointing to it gets loaded twice
	// <rdar://problem/14160846> Some apps depend on frameworks being found via library paths
	if ( (libraryPaths != NULL) && ((frameworkPartialPath == NULL) || sFrameworksFoundAsDylibs) ) {
		const char* libraryLeafName = getLibraryLeafName(path);
		const size_t libraryLeafNameLen = strlen(libraryLeafName);
		for(const char* const* lp = libraryPaths; *lp != NULL; ++lp) {
			char libpath[strlen(*lp)+libraryLeafNameLen+8];
			strcpy(libpath, *lp);
			strcat(libpath, "/");
			strcat(libpath, libraryLeafName);
			//dyld::log("dyld: fallback library path used: %s() -> loadPhase4(\"%s\", ...)\n", __func__, libpath);
			image = loadPhase4(libpath, orgPath, context, exceptions);
			if ( image != NULL )
				return image;
		}
	}
	return NULL;
}

// try search overrides and fallbacks
static ImageLoader* loadPhase1(const char* path, const char* orgPath, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);
	ImageLoader* image = NULL;

	// handle LD_LIBRARY_PATH environment variables that force searching
	if ( context.useLdLibraryPath && (sEnv.LD_LIBRARY_PATH != NULL) ) {
		image = loadPhase2(path, orgPath, context, NULL, sEnv.LD_LIBRARY_PATH, exceptions);
		if ( image != NULL )
			return image;
	}

	// handle DYLD_ environment variables that force searching
	if ( context.useSearchPaths && ((sEnv.DYLD_FRAMEWORK_PATH != NULL) || (sEnv.DYLD_LIBRARY_PATH != NULL)) ) {
		image = loadPhase2(path, orgPath, context, sEnv.DYLD_FRAMEWORK_PATH, sEnv.DYLD_LIBRARY_PATH, exceptions);
		if ( image != NULL )
			return image;
	}
	
	// try raw path
	image = loadPhase3(path, orgPath, context, exceptions);
	if ( image != NULL )
		return image;
	
	// try fallback paths during second time (will open file)
	const char* const* fallbackLibraryPaths = sEnv.DYLD_FALLBACK_LIBRARY_PATH;
	if ( (fallbackLibraryPaths != NULL) && !context.useFallbackPaths )
		fallbackLibraryPaths = NULL;
	if ( !context.dontLoad  && (exceptions != NULL) && ((sEnv.DYLD_FALLBACK_FRAMEWORK_PATH != NULL) || (fallbackLibraryPaths != NULL)) ) {
		image = loadPhase2(path, orgPath, context, sEnv.DYLD_FALLBACK_FRAMEWORK_PATH, fallbackLibraryPaths, exceptions);
		if ( image != NULL )
			return image;
	}
		
	return NULL;
}

// try root substitutions
static ImageLoader* loadPhase0(const char* path, const char* orgPath, const LoadContext& context, std::vector<const char*>* exceptions)
{
	//dyld::log("%s(%s, %p)\n", __func__ , path, exceptions);

	// handle DYLD_ROOT_PATH which forces absolute paths to use a new root
	if ( (gLinkContext.rootPaths != NULL) && (path[0] == '/') ) {
		for(const char* const* rootPath = gLinkContext.rootPaths ; *rootPath != NULL; ++rootPath) {
			char newPath[strlen(*rootPath) + strlen(path)+2];
			strcpy(newPath, *rootPath);
			strcat(newPath, path);
			ImageLoader* image = loadPhase1(newPath, orgPath, context, exceptions);
			if ( image != NULL )
				return image;
		}
	}

	// try raw path
	return loadPhase1(path, orgPath, context, exceptions);
}

#if DYLD_SHARED_CACHE_SUPPORT
	static bool cacheablePath(const char* path) {
		if (strncmp(path, "/usr/lib/", 9) == 0)
			return true;
		if (strncmp(path, "/System/Library/", 16) == 0)
			return true;
		return false;
	}
#endif

//
// Given all the DYLD_ environment variables, the general case for loading libraries
// is that any given path expands into a list of possible locations to load.  We
// also must take care to ensure two copies of the "same" library are never loaded.
//
// The algorithm used here is that there is a separate function for each "phase" of the
// path expansion.  Each phase function calls the next phase with each possible expansion
// of that phase.  The result is the last phase is called with all possible paths.  
//
// To catch duplicates the algorithm is run twice.  The first time, the last phase checks
// the path against all loaded images.  The second time, the last phase calls open() on 
// the path.  Either time, if an image is found, the phases all unwind without checking
// for other paths.
//
ImageLoader* load(const char* path, const LoadContext& context)
{
	CRSetCrashLogMessage2(path);
	const char* orgPath = path;
	
	//dyld::log("%s(%s)\n", __func__ , path);
	char realPath[PATH_MAX];
	// when DYLD_IMAGE_SUFFIX is in used, do a realpath(), otherwise a load of "Foo.framework/Foo" will not match
	if ( context.useSearchPaths && ( gLinkContext.imageSuffix != NULL) ) {
		if ( realpath(path, realPath) != NULL )
			path = realPath;
	}
	
	// try all path permutations and check against existing loaded images
	ImageLoader* image = loadPhase0(path, orgPath, context, NULL);
	if ( image != NULL ) {
		CRSetCrashLogMessage2(NULL);
		return image;
	}

	// try all path permutations and try open() until first success
	std::vector<const char*> exceptions;
	image = loadPhase0(path, orgPath, context, &exceptions);
#if __IPHONE_OS_VERSION_MIN_REQUIRED && DYLD_SHARED_CACHE_SUPPORT && !TARGET_IPHONE_SIMULATOR
	// <rdar://problem/16704628> support symlinks on disk to a path in dyld shared cache
	if ( (image == NULL) && cacheablePath(path) && !context.dontLoad ) {
		char resolvedPath[PATH_MAX];
		realpath(path, resolvedPath);
		int myerr = errno;
		// If realpath() resolves to a path which does not exist on disk, errno is set to ENOENT
		if ( (myerr == ENOENT) || (myerr == 0) )
		{
			// see if this image is in shared cache
			const macho_header* mhInCache;
			const char*			pathInCache;
			long				slideInCache;
			if ( findInSharedCacheImage(resolvedPath, false, NULL, &mhInCache, &pathInCache, &slideInCache) ) {
				struct stat stat_buf;
				bzero(&stat_buf, sizeof(stat_buf));
				try {
					image = ImageLoaderMachO::instantiateFromCache(mhInCache, pathInCache, slideInCache, stat_buf, gLinkContext);
					image = checkandAddImage(image, context);
				}
				catch (...) {
					image = NULL;
				}
			}
		}
	}
#endif
    CRSetCrashLogMessage2(NULL);
	if ( image != NULL ) {
		// <rdar://problem/6916014> leak in dyld during dlopen when using DYLD_ variables
		for (std::vector<const char*>::iterator it = exceptions.begin(); it != exceptions.end(); ++it) {
			free((void*)(*it));
		}
#if DYLD_SHARED_CACHE_SUPPORT
		// if loaded image is not from cache, but original path is in cache
		// set gSharedCacheOverridden flag to disable some ObjC optimizations
		if ( !gSharedCacheOverridden && !image->inSharedCache() && image->isDylib() && cacheablePath(path) && inSharedCache(path) ) {
			gSharedCacheOverridden = true;
		}
#endif
		return image;
	}
	else if ( exceptions.size() == 0 ) {
		if ( context.dontLoad ) {
			return NULL;
		}
		else
			throw "image not found";
	}
	else {
		const char* msgStart = "no suitable image found.  Did find:";
		const char* delim = "\n\t";
		size_t allsizes = strlen(msgStart)+8;
		for (size_t i=0; i < exceptions.size(); ++i) 
			allsizes += (strlen(exceptions[i]) + strlen(delim));
		char* fullMsg = new char[allsizes];
		strcpy(fullMsg, msgStart);
		for (size_t i=0; i < exceptions.size(); ++i) {
			strcat(fullMsg, delim);
			strcat(fullMsg, exceptions[i]);
			free((void*)exceptions[i]);
		}
		throw (const char*)fullMsg;
	}
}



#if DYLD_SHARED_CACHE_SUPPORT



#if __i386__
	#define ARCH_NAME			"i386"
	#define ARCH_CACHE_MAGIC	"dyld_v1    i386"
#elif __x86_64__
	#define ARCH_NAME			"x86_64"
	#define ARCH_CACHE_MAGIC	"dyld_v1  x86_64"
	#define ARCH_NAME_H			"x86_64h"
	#define ARCH_CACHE_MAGIC_H	"dyld_v1 x86_64h"
#elif __ARM_ARCH_5TEJ__
	#define ARCH_NAME			"armv5"
	#define ARCH_CACHE_MAGIC	"dyld_v1   armv5"
#elif __ARM_ARCH_6K__
	#define ARCH_NAME			"armv6"
	#define ARCH_CACHE_MAGIC	"dyld_v1   armv6"
#elif __ARM_ARCH_7F__
	#define ARCH_NAME			"armv7f"
	#define ARCH_CACHE_MAGIC	"dyld_v1  armv7f"
#elif __ARM_ARCH_7K__
	#define ARCH_NAME			"armv7k"
	#define ARCH_CACHE_MAGIC	"dyld_v1  armv7k"
#elif __ARM_ARCH_7A__
	#define ARCH_NAME			"armv7"
	#define ARCH_CACHE_MAGIC	"dyld_v1   armv7"
#elif __ARM_ARCH_7S__
	#define ARCH_NAME			"armv7s"
	#define ARCH_CACHE_MAGIC	"dyld_v1  armv7s"
#elif __arm64__
	#define ARCH_NAME			"arm64"
	#define ARCH_CACHE_MAGIC	"dyld_v1   arm64"
#endif


static int __attribute__((noinline)) _shared_region_check_np(uint64_t* start_address)
{
	if ( gLinkContext.sharedRegionMode == ImageLoader::kUseSharedRegion ) 
		return syscall(294, start_address);
	return -1;
}


static int __attribute__((noinline)) _shared_region_map_and_slide_np(int fd, uint32_t count, const shared_file_mapping_np mappings[],
												int codeSignatureMappingIndex, long slide, void* slideInfo, unsigned long slideInfoSize)
{
	// register code signature blob for whole dyld cache
	if ( codeSignatureMappingIndex != -1 ) {
		fsignatures_t siginfo;
		siginfo.fs_file_start = 0;  // cache always starts at beginning of file
		siginfo.fs_blob_start = (void*)mappings[codeSignatureMappingIndex].sfm_file_offset;
		siginfo.fs_blob_size  = mappings[codeSignatureMappingIndex].sfm_size;
		int result = fcntl(fd, F_ADDFILESIGS, &siginfo);
		// <rdar://problem/12891874> don't warn in chrooted case because mapping syscall is about to fail too
		if ( (result == -1) && gLinkContext.verboseMapping )
			dyld::log("dyld: code signature registration for shared cache failed with errno=%d\n", errno);
	}

	if ( gLinkContext.sharedRegionMode == ImageLoader::kUseSharedRegion ) {
		return syscall(438, fd, count, mappings, slide, slideInfo, slideInfoSize);
	}

	// remove the shared region sub-map
	vm_deallocate(mach_task_self(), (vm_address_t)SHARED_REGION_BASE, SHARED_REGION_SIZE);
	
	// notify gdb or other lurkers that this process is no longer using the shared region
	dyld::gProcessInfo->processDetachedFromSharedRegion = true;

	// map cache just for this process with mmap()
	const shared_file_mapping_np* const start = mappings;
	const shared_file_mapping_np* const end = &mappings[count];
	for (const shared_file_mapping_np* p = start; p < end; ++p ) {
		void* mmapAddress = (void*)(uintptr_t)(p->sfm_address);
		size_t size = p->sfm_size;
		//dyld::log("dyld: mapping address %p with size 0x%08lX\n", mmapAddress, size);
		int protection = 0;
		if ( p->sfm_init_prot & VM_PROT_EXECUTE )
			protection   |= PROT_EXEC;
		if ( p->sfm_init_prot & VM_PROT_READ )
			protection   |= PROT_READ;
		if ( p->sfm_init_prot & VM_PROT_WRITE )
			protection   |= PROT_WRITE;
		off_t offset = p->sfm_file_offset;
		if ( mmap(mmapAddress, size, protection, MAP_FIXED | MAP_PRIVATE, fd, offset) != mmapAddress ) {
			// failed to map some chunk of this shared cache file
			// clear shared region
			vm_deallocate(mach_task_self(), (vm_address_t)SHARED_REGION_BASE, SHARED_REGION_SIZE);
			// go back to not using shared region at all
			gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
			if ( gLinkContext.verboseMapping ) {
				dyld::log("dyld: shared cached region cannot be mapped at address %p with size 0x%08lX\n",
							mmapAddress, size);
			}
			// return failure
			return -1;
		}
	}

	// update all __DATA pages with slide info
	if ( slide != 0 ) {
		const uintptr_t dataPagesStart = mappings[1].sfm_address;
		const dyld_cache_slide_info* slideInfoHeader = (dyld_cache_slide_info*)slideInfo;
		const uint16_t* toc = (uint16_t*)((long)(slideInfoHeader) + slideInfoHeader->toc_offset);
		const uint8_t* entries = (uint8_t*)((long)(slideInfoHeader) + slideInfoHeader->entries_offset);
		for(uint32_t i=0; i < slideInfoHeader->toc_count; ++i) {
			const uint8_t* entry = &entries[toc[i]*slideInfoHeader->entries_size];
			const uint8_t* page = (uint8_t*)(long)(dataPagesStart + (4096*i));
			//dyld::log("page=%p toc[%d]=%d entries=%p\n", page, i, toc[i], entry);
			for(int j=0; j < 128; ++j) {
				uint8_t b = entry[j];
				//dyld::log("    entry[%d] = 0x%02X\n", j, b);
				if ( b != 0 ) {
					for(int k=0; k < 8; ++k) {
						if ( b & (1<<k) ) {
							uintptr_t* p = (uintptr_t*)(page + j*8*4 + k*4);
							uintptr_t value = *p;
							//dyld::log("        *%p was 0x%lX will be 0x%lX\n", p, value, value+sSharedCacheSlide);
							*p = value + slide;
						}
					}
				}
			}
		}
	}

	// succesfully mapped shared cache for just this process
	gLinkContext.sharedRegionMode = ImageLoader::kUsePrivateSharedRegion;
	
	return 0;
}


const void*	imMemorySharedCacheHeader()
{
	return sSharedCache;
}

int openSharedCacheFile()
{
	char path[MAXPATHLEN];
	strlcpy(path, sSharedCacheDir, MAXPATHLEN);
	strlcat(path, "/", MAXPATHLEN);
#if __x86_64__
	if ( sHaswell ) {
		strlcat(path, DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME_H, MAXPATHLEN);
		int fd = my_open(path, O_RDONLY, 0);
		if ( fd != -1 ) {
			if ( gLinkContext.verboseMapping ) 
				dyld::log("dyld: Mapping%s shared cache from %s\n", (gLinkContext.sharedRegionMode == ImageLoader::kUsePrivateSharedRegion) ? " private": "", path);
			return fd;
		}
		strlcpy(path, sSharedCacheDir, MAXPATHLEN);
	}
#endif
	strlcat(path, DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME, MAXPATHLEN);
	if ( gLinkContext.verboseMapping ) 
		dyld::log("dyld: Mapping%s shared cache from %s\n", (gLinkContext.sharedRegionMode == ImageLoader::kUsePrivateSharedRegion) ? " private": "", path);
	return my_open(path, O_RDONLY, 0);
}


static void getCacheBounds(uint32_t mappingsCount, const shared_file_mapping_np mappings[], uint64_t& lowAddress, uint64_t& highAddress)
{
	lowAddress = 0;
	highAddress = 0;
	for(uint32_t i=0; i < mappingsCount; ++i) {
		if ( lowAddress == 0 ) {
			lowAddress = mappings[i].sfm_address;
			highAddress = mappings[i].sfm_address + mappings[i].sfm_size;
		}
		else {
			if ( mappings[i].sfm_address < lowAddress )
				lowAddress = mappings[i].sfm_address;
			if ( (mappings[i].sfm_address + mappings[i].sfm_size) > highAddress )
				highAddress = mappings[i].sfm_address + mappings[i].sfm_size;
		}
	}
}

static long pickCacheSlide(uint32_t mappingsCount, shared_file_mapping_np mappings[])
{
#if __x86_64__
	// x86_64 has a two memory regions:
	//       256MB at 0x00007FFF70000000 
	//      1024MB at 0x00007FFF80000000
	// Some old shared caches have r/w region after rx region, so all regions slide within 1GB range
	// Newer shared caches have r/w region based at 0x7FFF70000000 and r/o regions at 0x7FFF80000000, so each part has max slide
	if ( (mappingsCount >= 3) && (mappings[1].sfm_init_prot == (VM_PROT_READ|VM_PROT_WRITE)) && (mappings[1].sfm_address == 0x00007FFF70000000) ) {
		const uint64_t rwSize = mappings[1].sfm_size;
		const uint64_t rwSlop = 0x10000000ULL - rwSize;
		const uint64_t roSize = (mappings[2].sfm_address + mappings[2].sfm_size) - mappings[0].sfm_address;
		const uint64_t roSlop = 0x40000000ULL - roSize;
		const uint64_t space = (rwSlop < roSlop) ? rwSlop : roSlop;
		
		// choose new random slide
		long slide = (arc4random() % space) & (-4096);
		//dyld::log("rwSlop=0x%0llX, roSlop=0x%0llX\n", rwSlop, roSlop);
		//dyld::log("space=0x%0llX, slide=0x%0lX\n", space, slide);
		
		// update mappings
		for(uint32_t i=0; i < mappingsCount; ++i) {
			mappings[i].sfm_address += slide;
		}
		
		return slide;
	}
	// else fall through to handle old style cache
#endif
	// get bounds of cache
	uint64_t lowAddress;
	uint64_t highAddress;
	getCacheBounds(mappingsCount, mappings, lowAddress, highAddress);
	
	// find slop space
	const uint64_t space = (SHARED_REGION_BASE + SHARED_REGION_SIZE) - highAddress;
	
	// choose new random slide
	long slide = dyld_page_trunc(arc4random() % space);
	//dyld::log("slideSpace=0x%0llX\n", space);
	//dyld::log("slide=0x%0lX\n", slide);
	
	// update mappings
	for(uint32_t i=0; i < mappingsCount; ++i) {
		mappings[i].sfm_address += slide;
	}
	
	return slide;
}

static void mapSharedCache()
{
	uint64_t cacheBaseAddress = 0;
	// quick check if a cache is already mapped into shared region
	if ( _shared_region_check_np(&cacheBaseAddress) == 0 ) {
		sSharedCache = (dyld_cache_header*)cacheBaseAddress;
		// if we don't understand the currently mapped shared cache, then ignore
#if __x86_64__
		const char* magic = (sHaswell ? ARCH_CACHE_MAGIC_H : ARCH_CACHE_MAGIC);
#else
		const char* magic = ARCH_CACHE_MAGIC;
#endif
		if ( strcmp(sSharedCache->magic, magic) != 0 ) {
			sSharedCache = NULL;
			if ( gLinkContext.verboseMapping ) {
				dyld::log("dyld: existing shared cached in memory is not compatible\n");
				return;
			}
		}
		// check if cache file is slidable
		const dyld_cache_header* header = sSharedCache;
		if ( (header->mappingOffset >= 0x48) && (header->slideInfoSize != 0) ) {
			// solve for slide by comparing loaded address to address of first region
			const uint8_t* loadedAddress = (uint8_t*)sSharedCache;
			const dyld_cache_mapping_info* const mappings = (dyld_cache_mapping_info*)(loadedAddress+header->mappingOffset);
			const uint8_t* preferedLoadAddress = (uint8_t*)(long)(mappings[0].address);
			sSharedCacheSlide = loadedAddress - preferedLoadAddress;
			dyld::gProcessInfo->sharedCacheSlide = sSharedCacheSlide;
			//dyld::log("sSharedCacheSlide=0x%08lX, loadedAddress=%p, preferedLoadAddress=%p\n", sSharedCacheSlide, loadedAddress, preferedLoadAddress);
		}
		// if cache has a uuid, copy it 
		if ( header->mappingOffset >= 0x68 ) {
			memcpy(dyld::gProcessInfo->sharedCacheUUID, header->uuid, 16);
		}
		// verbose logging
		if ( gLinkContext.verboseMapping ) {
			dyld::log("dyld: re-using existing shared cache mapping\n");
		}
	}
	else {
#if __i386__ || __x86_64__
		// <rdar://problem/5925940> Safe Boot should disable dyld shared cache
		// if we are in safe-boot mode and the cache was not made during this boot cycle,
		// delete the cache file
		uint32_t	safeBootValue = 0;
		size_t		safeBootValueSize = sizeof(safeBootValue);
		if ( (sysctlbyname("kern.safeboot", &safeBootValue, &safeBootValueSize, NULL, 0) == 0) && (safeBootValue != 0) ) {
			// user booted machine in safe-boot mode
			struct stat dyldCacheStatInfo;
			//  Don't use custom DYLD_SHARED_CACHE_DIR if provided, use standard path
			if ( my_stat(MACOSX_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME, &dyldCacheStatInfo) == 0 ) {
				struct timeval bootTimeValue;
				size_t bootTimeValueSize = sizeof(bootTimeValue);
				if ( (sysctlbyname("kern.boottime", &bootTimeValue, &bootTimeValueSize, NULL, 0) == 0) && (bootTimeValue.tv_sec != 0) ) {
					// if the cache file was created before this boot, then throw it away and let it rebuild itself
					if ( dyldCacheStatInfo.st_mtime < bootTimeValue.tv_sec ) {
						::unlink(MACOSX_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME);
						gLinkContext.sharedRegionMode = ImageLoader::kDontUseSharedRegion;
						return;
					}
				}
			}
		}
#endif
		// map in shared cache to shared region
		int fd = openSharedCacheFile();
		if ( fd != -1 ) {
			uint8_t firstPages[8192];
			if ( ::read(fd, firstPages, 8192) == 8192 ) {
				dyld_cache_header* header = (dyld_cache_header*)firstPages;
		#if __x86_64__
				const char* magic = (sHaswell ? ARCH_CACHE_MAGIC_H : ARCH_CACHE_MAGIC);
		#else
				const char* magic = ARCH_CACHE_MAGIC;
		#endif
				if ( strcmp(header->magic, magic) == 0 ) {
					const dyld_cache_mapping_info* const fileMappingsStart = (dyld_cache_mapping_info*)&firstPages[header->mappingOffset];
					const dyld_cache_mapping_info* const fileMappingsEnd = &fileMappingsStart[header->mappingCount];
					shared_file_mapping_np	mappings[header->mappingCount+1]; // add room for code-sig 
					unsigned int mappingCount = header->mappingCount;
					int codeSignatureMappingIndex = -1;
					int readWriteMappingIndex = -1;
					int readOnlyMappingIndex = -1;
					// validate that the cache file has not been truncated
					bool goodCache = false;
					struct stat stat_buf;
					if ( fstat(fd, &stat_buf) == 0 ) {
						goodCache = true;
						int i=0;
						for (const dyld_cache_mapping_info* p = fileMappingsStart; p < fileMappingsEnd; ++p, ++i) {
							mappings[i].sfm_address		= p->address;
							mappings[i].sfm_size		= p->size;
							mappings[i].sfm_file_offset	= p->fileOffset;
							mappings[i].sfm_max_prot	= p->maxProt;
							mappings[i].sfm_init_prot	= p->initProt;
							// rdar://problem/5694507 old update_dyld_shared_cache tool could make a cache file
							// that is not page aligned, but otherwise ok.
							if ( p->fileOffset+p->size > (uint64_t)(stat_buf.st_size+4095 & (-4096)) ) {
								dyld::log("dyld: shared cached file is corrupt: %s" DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME "\n", sSharedCacheDir);
								goodCache = false;
							}
							if ( (mappings[i].sfm_init_prot & (VM_PROT_READ|VM_PROT_WRITE)) == (VM_PROT_READ|VM_PROT_WRITE) ) {
								readWriteMappingIndex = i;
							}
							if ( mappings[i].sfm_init_prot == VM_PROT_READ ) {
								readOnlyMappingIndex = i;
							}
						}
						// if shared cache is code signed, add a mapping for the code signature
						uint64_t signatureSize = header->codeSignatureSize;
						// zero size in header means signature runs to end-of-file
						if ( signatureSize == 0 )
							signatureSize = stat_buf.st_size - header->codeSignatureOffset;
						if ( signatureSize != 0 ) {
                            int linkeditMapping = mappingCount-1;
							codeSignatureMappingIndex = mappingCount++;
							mappings[codeSignatureMappingIndex].sfm_address		= mappings[linkeditMapping].sfm_address + mappings[linkeditMapping].sfm_size;
#if __arm__ || __arm64__
							mappings[codeSignatureMappingIndex].sfm_size		= (signatureSize+16383) & (-16384);
#else
							mappings[codeSignatureMappingIndex].sfm_size		= (signatureSize+4095) & (-4096);
#endif
							mappings[codeSignatureMappingIndex].sfm_file_offset	= header->codeSignatureOffset;
							mappings[codeSignatureMappingIndex].sfm_max_prot	= VM_PROT_READ;
							mappings[codeSignatureMappingIndex].sfm_init_prot	= VM_PROT_READ;
						}
					}
#if __MAC_OS_X_VERSION_MIN_REQUIRED	
					// sanity check that /usr/lib/libSystem.B.dylib stat() info matches cache
					if ( header->imagesCount * sizeof(dyld_cache_image_info) + header->imagesOffset < 8192 ) {
						bool foundLibSystem = false;
						if ( my_stat("/usr/lib/libSystem.B.dylib", &stat_buf) == 0 ) {
							const dyld_cache_image_info* images = (dyld_cache_image_info*)&firstPages[header->imagesOffset];
							const dyld_cache_image_info* const imagesEnd = &images[header->imagesCount];
							for (const dyld_cache_image_info* p = images; p < imagesEnd; ++p) {
 								if ( ((time_t)p->modTime == stat_buf.st_mtime) && ((ino_t)p->inode == stat_buf.st_ino) ) {
									foundLibSystem = true;
									break;
								}
							}					
						}
						if ( !sSharedCacheIgnoreInodeAndTimeStamp && !foundLibSystem ) {
							dyld::log("dyld: shared cached file was built against a different libSystem.dylib, ignoring cache.\n"
									"to update dyld shared cache run: 'sudo update_dyld_shared_cache' then reboot.\n");
							goodCache = false;
						}
					}
#endif
#if __IPHONE_OS_VERSION_MIN_REQUIRED
					{
						uint64_t lowAddress;
						uint64_t highAddress;
						getCacheBounds(mappingCount, mappings, lowAddress, highAddress);
						if ( (highAddress-lowAddress) > SHARED_REGION_SIZE ) 
							throw "dyld shared cache is too big to fit in shared region";
					}
#endif

					if ( goodCache && (readWriteMappingIndex == -1) ) {
						dyld::log("dyld: shared cached file is missing read/write mapping: %s" DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME "\n", sSharedCacheDir);
						goodCache = false;
					}
					if ( goodCache && (readOnlyMappingIndex == -1) ) {
						dyld::log("dyld: shared cached file is missing read-only mapping: %s" DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME "\n", sSharedCacheDir);
						goodCache = false;
					}
					if ( goodCache ) {
						long cacheSlide = 0;
						void* slideInfo = NULL;
						uint64_t slideInfoSize = 0;
						// check if shared cache contains slid info
						if ( header->slideInfoSize != 0 ) {
							// <rdar://problem/8611968> don't slide shared cache if ASLR disabled (main executable didn't slide)
							if ( sMainExecutable->isPositionIndependentExecutable() && (sMainExecutable->getSlide() == 0) )
								cacheSlide = 0;
							else {
								// generate random slide amount
								cacheSlide = pickCacheSlide(mappingCount, mappings);
								slideInfo = (void*)(long)(mappings[readOnlyMappingIndex].sfm_address + (header->slideInfoOffset - mappings[readOnlyMappingIndex].sfm_file_offset));
								slideInfoSize = header->slideInfoSize;
								// add VM_PROT_SLIDE bit to __DATA area of cache
								mappings[readWriteMappingIndex].sfm_max_prot  |= VM_PROT_SLIDE;
								mappings[readWriteMappingIndex].sfm_init_prot |= VM_PROT_SLIDE;
							}
						}
						if ( gLinkContext.verboseMapping ) {
							dyld::log("dyld: calling _shared_region_map_and_slide_np() with regions:\n");
							for (int i=0; i < mappingCount; ++i) {
								dyld::log("   address=0x%08llX, size=0x%08llX, fileOffset=0x%08llX\n", mappings[i].sfm_address, mappings[i].sfm_size, mappings[i].sfm_file_offset);
							}
						}
						if (_shared_region_map_and_slide_np(fd, mappingCount, mappings, codeSignatureMappingIndex, cacheSlide, slideInfo, slideInfoSize) == 0) {
							// successfully mapped cache into shared region
							sSharedCache = (dyld_cache_header*)mappings[0].sfm_address;
							sSharedCacheSlide = cacheSlide;
							dyld::gProcessInfo->sharedCacheSlide = cacheSlide;
							//dyld::log("sSharedCache=%p sSharedCacheSlide=0x%08lX\n", sSharedCache, sSharedCacheSlide);
							// if cache has a uuid, copy it
							if ( header->mappingOffset >= 0x68 ) {
								memcpy(dyld::gProcessInfo->sharedCacheUUID, header->uuid, 16);
							}
						}
						else {
#if __IPHONE_OS_VERSION_MIN_REQUIRED
							throw "dyld shared cache could not be mapped";
#endif
							if ( gLinkContext.verboseMapping ) 
								dyld::log("dyld: shared cached file could not be mapped\n");
						}
					}
				}
				else {
					if ( gLinkContext.verboseMapping ) 
						dyld::log("dyld: shared cached file is invalid\n");
				}
			}
			else {
				if ( gLinkContext.verboseMapping ) 
					dyld::log("dyld: shared cached file cannot be read\n");
			}
			close(fd);
		}
		else {
			if ( gLinkContext.verboseMapping ) 
				dyld::log("dyld: shared cached file cannot be opened\n");
		}
	}
	
	// remember if dyld loaded at same address as when cache built
	if ( sSharedCache != NULL ) {
		gLinkContext.dyldLoadedAtSameAddressNeededBySharedCache = ((uintptr_t)(sSharedCache->dyldBaseAddress) == (uintptr_t)&_mh_dylinker_header);
	}
	
	// tell gdb where the shared cache is
	if ( sSharedCache != NULL ) {
		const dyld_cache_mapping_info* const start = (dyld_cache_mapping_info*)((uint8_t*)sSharedCache + sSharedCache->mappingOffset);
		dyld_shared_cache_ranges.sharedRegionsCount = sSharedCache->mappingCount;
		// only room to tell gdb about first four regions
		if ( dyld_shared_cache_ranges.sharedRegionsCount > 4 )
			dyld_shared_cache_ranges.sharedRegionsCount = 4;
		const dyld_cache_mapping_info* const end = &start[dyld_shared_cache_ranges.sharedRegionsCount];
		int index = 0;
		for (const dyld_cache_mapping_info* p = start; p < end; ++p, ++index ) {
			dyld_shared_cache_ranges.ranges[index].start = p->address+sSharedCacheSlide;
			dyld_shared_cache_ranges.ranges[index].length = p->size;
			if ( gLinkContext.verboseMapping ) {
				dyld::log("        0x%08llX->0x%08llX %s%s%s init=%x, max=%x\n", 
					p->address+sSharedCacheSlide, p->address+sSharedCacheSlide+p->size-1,
					((p->initProt & VM_PROT_READ) ? "read " : ""),
					((p->initProt & VM_PROT_WRITE) ? "write " : ""),
					((p->initProt & VM_PROT_EXECUTE) ? "execute " : ""),  p->initProt, p->maxProt);
			}
		#if __i386__
			// If a non-writable and executable region is found in the R/W shared region, then this is __IMPORT segments
			// This is an old cache.  Make writable.  dyld no longer supports turn W on and off as it binds
			if ( (p->initProt == (VM_PROT_READ|VM_PROT_EXECUTE)) && ((p->address & 0xF0000000) == 0xA0000000) ) {
				if ( p->size != 0 ) {
					vm_prot_t prot = VM_PROT_EXECUTE | PROT_READ | VM_PROT_WRITE;
					vm_protect(mach_task_self(), p->address, p->size, false, prot);
					if ( gLinkContext.verboseMapping ) {
						dyld::log("%18s at 0x%08llX->0x%08llX altered permissions to %c%c%c\n", "", p->address, 
							p->address+p->size-1,
							(prot & PROT_READ) ? 'r' : '.',  (prot & PROT_WRITE) ? 'w' : '.',  (prot & PROT_EXEC) ? 'x' : '.' );
					}
				}
			}
		#endif
		}
		if ( gLinkContext.verboseMapping ) {
			// list the code blob
			dyld_cache_header* header = (dyld_cache_header*)sSharedCache;
			uint64_t signatureSize = header->codeSignatureSize;
			// zero size in header means signature runs to end-of-file
			if ( signatureSize == 0 ) {
				struct stat stat_buf;
				if ( my_stat(IPHONE_DYLD_SHARED_CACHE_DIR DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME, &stat_buf) == 0 ) 
					signatureSize = stat_buf.st_size - header->codeSignatureOffset;
			}
			if ( signatureSize != 0 ) {
				const dyld_cache_mapping_info* const last = &start[dyld_shared_cache_ranges.sharedRegionsCount-1];
				uint64_t codeBlobStart = last->address + last->size;
				dyld::log("        0x%08llX->0x%08llX (code signature)\n", codeBlobStart, codeBlobStart+signatureSize);
			}
		}
#if __IPHONE_OS_VERSION_MIN_REQUIRED
		// check for file that enables dyld shared cache dylibs to be overridden
		struct stat enableStatBuf;
		// check file size to determine if correct file is in place. 
		// See <rdar://problem/13591370> Need a way to disable roots without removing /S/L/C/com.apple.dyld/enable...
		sDylibsOverrideCache = ( (my_stat(IPHONE_DYLD_SHARED_CACHE_DIR "enable-dylibs-to-override-cache", &enableStatBuf) == 0)
									&& (enableStatBuf.st_size < ENABLE_DYLIBS_TO_OVERRIDE_CACHE_SIZE) );
#endif	
	}
}
#endif // #if DYLD_SHARED_CACHE_SUPPORT



// create when NSLinkModule is called for a second time on a bundle
ImageLoader* cloneImage(ImageLoader* image)
{
	// open file (automagically closed when this function exits)
	FileOpener file(image->getPath());
	
	struct stat stat_buf;
	if ( fstat(file.getFileDescriptor(), &stat_buf) == -1)
		throw "stat error";
	
	dyld::LoadContext context;
	context.useSearchPaths		= false;
	context.useFallbackPaths	= false;
	context.useLdLibraryPath	= false;
	context.implicitRPath		= false;
	context.matchByInstallName	= false;
	context.dontLoad			= false;
	context.mustBeBundle		= true;
	context.mustBeDylib			= false;
	context.canBePIE			= false;
	context.origin				= NULL;
	context.rpath				= NULL;
	return loadPhase6(file.getFileDescriptor(), stat_buf, image->getPath(), context);
}


ImageLoader* loadFromMemory(const uint8_t* mem, uint64_t len, const char* moduleName)
{
	// if fat wrapper, find usable sub-file
	const fat_header* memStartAsFat = (fat_header*)mem;
	uint64_t fileOffset = 0;
	uint64_t fileLength = len;
	if ( memStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		if ( fatFindBest(memStartAsFat, &fileOffset, &fileLength) ) {
			mem = &mem[fileOffset];
			len = fileLength;
		}
		else {
			throw "no matching architecture in universal wrapper";
		}
	}

	// try each loader
	if ( isCompatibleMachO(mem, moduleName) ) {
		ImageLoader* image = ImageLoaderMachO::instantiateFromMemory(moduleName, (macho_header*)mem, len, gLinkContext);
		// don't add bundles to global list, they can be loaded but not linked.  When linked it will be added to list
		if ( ! image->isBundle() ) 
			addImage(image);
		return image;
	}
	
	// try other file formats here...
	
	// throw error about what was found
	switch (*(uint32_t*)mem) {
		case MH_MAGIC:
		case MH_CIGAM:
		case MH_MAGIC_64:
		case MH_CIGAM_64:
			throw "mach-o, but wrong architecture";
		default:
		throwf("unknown file type, first eight bytes: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X", 
			mem[0], mem[1], mem[2], mem[3], mem[4], mem[5], mem[6],mem[7]);
	}
}


void registerAddCallback(ImageCallback func)
{
	// now add to list to get notified when any more images are added
	sAddImageCallbacks.push_back(func);
	
	// call callback with all existing images
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* image = *it;
		if ( image->getState() >= dyld_image_state_bound && image->getState() < dyld_image_state_terminated )
			(*func)(image->machHeader(), image->getSlide());
	}
}

void registerRemoveCallback(ImageCallback func)
{
	// <rdar://problem/15025198> ignore calls to register a notification during a notification
	if ( sRemoveImageCallbacksInUse )
		return;
	sRemoveImageCallbacks.push_back(func);
}

void clearErrorMessage()
{
	error_string[0] = '\0';
}

void setErrorMessage(const char* message)
{
	// save off error message in global buffer for CrashReporter to find
	strlcpy(error_string, message, sizeof(error_string));
}

const char* getErrorMessage()
{
	return error_string;
}


void halt(const char* message)
{
	dyld::log("dyld: %s\n", message);
	setErrorMessage(message);
	uintptr_t terminationFlags = 0;
	if ( !gLinkContext.startedInitializingMainExecutable ) 
		terminationFlags = 1;
	setAlImageInfosHalt(error_string, terminationFlags);
	dyld_fatal_error(error_string);
}

static void setErrorStrings(unsigned errorCode, const char* errorClientOfDylibPath,
								const char* errorTargetDylibPath, const char* errorSymbol)
{
	dyld::gProcessInfo->errorKind = errorCode;
	dyld::gProcessInfo->errorClientOfDylibPath = errorClientOfDylibPath;
	dyld::gProcessInfo->errorTargetDylibPath = errorTargetDylibPath;
	dyld::gProcessInfo->errorSymbol = errorSymbol;
}


uintptr_t bindLazySymbol(const mach_header* mh, uintptr_t* lazyPointer)
{
	uintptr_t result = 0;
	// acquire read-lock on dyld's data structures
#if 0 // rdar://problem/3811777 turn off locking until deadlock is resolved
	if ( gLibSystemHelpers != NULL ) 
		(*gLibSystemHelpers->lockForReading)();
#endif
	// lookup and bind lazy pointer and get target address
	try {
		ImageLoader* target;
	#if __i386__
		// fast stubs pass NULL for mh and image is instead found via the location of stub (aka lazyPointer)
		if ( mh == NULL )
			target = dyld::findImageContainingAddress(lazyPointer);
		else
			target = dyld::findImageByMachHeader(mh);
	#else
		// note, target should always be mach-o, because only mach-o lazy handler wired up to this
		target = dyld::findImageByMachHeader(mh);
	#endif
		if ( target == NULL )
			throwf("image not found for lazy pointer at %p", lazyPointer);
		result = target->doBindLazySymbol(lazyPointer, gLinkContext);
	}
	catch (const char* message) {
		dyld::log("dyld: lazy symbol binding failed: %s\n", message);
		halt(message);
	}
	// release read-lock on dyld's data structures
#if 0
	if ( gLibSystemHelpers != NULL ) 
		(*gLibSystemHelpers->unlockForReading)();
#endif
	// return target address to glue which jumps to it with real parameters restored
	return result;
}


uintptr_t fastBindLazySymbol(ImageLoader** imageLoaderCache, uintptr_t lazyBindingInfoOffset)
{
	uintptr_t result = 0;
	// get image 
	if ( *imageLoaderCache == NULL ) {
		// save in cache
		*imageLoaderCache = dyld::findMappedRange((uintptr_t)imageLoaderCache);
		if ( *imageLoaderCache == NULL ) {
			const char* message = "fast lazy binding from unknown image";
			dyld::log("dyld: %s\n", message);
			halt(message);
		}
	}
	
	// bind lazy pointer and return it
	try {
		result = (*imageLoaderCache)->doBindFastLazySymbol((uint32_t)lazyBindingInfoOffset, gLinkContext, 
								(dyld::gLibSystemHelpers != NULL) ? dyld::gLibSystemHelpers->acquireGlobalDyldLock : NULL,
								(dyld::gLibSystemHelpers != NULL) ? dyld::gLibSystemHelpers->releaseGlobalDyldLock : NULL);
	}
	catch (const char* message) {
		dyld::log("dyld: lazy symbol binding failed: %s\n", message);
		halt(message);
	}

	// return target address to glue which jumps to it with real parameters restored
	return result;
}



void registerUndefinedHandler(UndefinedHandler handler)
{
	sUndefinedHandler = handler;
}

static void undefinedHandler(const char* symboName)
{
	if ( sUndefinedHandler != NULL ) {
		(*sUndefinedHandler)(symboName);
	}
}

static bool findExportedSymbol(const char* name, bool onlyInCoalesced, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	// search all images in order
	const ImageLoader* firstWeakImage = NULL;
	const ImageLoader::Symbol* firstWeakSym = NULL;
	const size_t imageCount = sAllImages.size();
	for(size_t i=0; i < imageCount; ++i) {
		ImageLoader* anImage = sAllImages[i];
		// the use of inserted libraries alters search order
		// so that inserted libraries are found before the main executable
		if ( sInsertedDylibCount > 0 ) {
			if ( i < sInsertedDylibCount )
				anImage = sAllImages[i+1];
			else if ( i == sInsertedDylibCount )
				anImage = sAllImages[0];
		}
		if ( ! anImage->hasHiddenExports() && (!onlyInCoalesced || anImage->hasCoalescedExports()) ) {
			*sym = anImage->findExportedSymbol(name, false, image);
			if ( *sym != NULL ) {
				// if weak definition found, record first one found
				if ( ((*image)->getExportedSymbolInfo(*sym) & ImageLoader::kWeakDefinition) != 0 ) {
					if ( firstWeakImage == NULL ) {
						firstWeakImage = *image;
						firstWeakSym = *sym;
					}
				}
				else {
					// found non-weak, so immediately return with it
					return true;
				}
			}
		}
	}
	if ( firstWeakSym != NULL ) {
		// found a weak definition, but no non-weak, so return first weak found
		*sym = firstWeakSym;
		*image = firstWeakImage;
		return true;
	}
	
	return false;
}

bool flatFindExportedSymbol(const char* name, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	return findExportedSymbol(name, false, sym, image);
}

bool findCoalescedExportedSymbol(const char* name, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	return findExportedSymbol(name, true, sym, image);
}


bool flatFindExportedSymbolWithHint(const char* name, const char* librarySubstring, const ImageLoader::Symbol** sym, const ImageLoader** image)
{
	// search all images in order
	const size_t imageCount = sAllImages.size();
	for(size_t i=0; i < imageCount; ++i){
		ImageLoader* anImage = sAllImages[i];
		// only look at images whose paths contain the hint string (NULL hint string is wildcard)
		if ( ! anImage->isBundle() && ((librarySubstring==NULL) || (strstr(anImage->getPath(), librarySubstring) != NULL)) ) {
			*sym = anImage->findExportedSymbol(name, false, image);
			if ( *sym != NULL ) {
				return true;
			}
		}
	}
	return false;
}

unsigned int getCoalescedImages(ImageLoader* images[])
{
	unsigned int count = 0;
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* image = *it;
		if ( image->participatesInCoalescing() ) {
			*images++ = *it;
			++count;
		}
	}
	return count;
}


static ImageLoader::MappedRegion* getMappedRegions(ImageLoader::MappedRegion* regions)
{
	ImageLoader::MappedRegion* end = regions;
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		(*it)->getMappedRegions(end);
	}
	return end;
}

void registerImageStateSingleChangeHandler(dyld_image_states state, dyld_image_state_change_handler handler)
{
	// mark the image that the handler is in as never-unload because dyld has a reference into it
	ImageLoader* handlerImage = findImageContainingAddress((void*)handler);
	if ( handlerImage != NULL )
		handlerImage->setNeverUnload();

	// add to list of handlers
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sSingleHandlers);
	if ( handlers != NULL ) {
        // <rdar://problem/10332417> need updateAllImages() to be last in dyld_image_state_mapped list
        // so that if ObjC adds a handler that prevents a load, it happens before the gdb list is updated
        if ( state == dyld_image_state_mapped )
            handlers->insert(handlers->begin(), handler);
        else
            handlers->push_back(handler);

		// call callback with all existing images
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* image = *it;
			dyld_image_info	 info;
			info.imageLoadAddress	= image->machHeader();
			info.imageFilePath		= image->getRealPath();
			info.imageFileModDate	= image->lastModified();
			// should only call handler if state == image->state
			if ( image->getState() == state )
				(*handler)(state, 1, &info);
			// ignore returned string, too late to do anything
		}
	}
}

void registerImageStateBatchChangeHandler(dyld_image_states state, dyld_image_state_change_handler handler)
{
	// mark the image that the handler is in as never-unload because dyld has a reference into it
	ImageLoader* handlerImage = findImageContainingAddress((void*)handler);
	if ( handlerImage != NULL )
		handlerImage->setNeverUnload();

	// add to list of handlers
	std::vector<dyld_image_state_change_handler>* handlers = stateToHandlers(state, sBatchHandlers);
	if ( handlers != NULL ) {
		// insert at front, so that gdb handler is always last
		handlers->insert(handlers->begin(), handler);
		
		// call callback with all existing images
		try {
			notifyBatchPartial(state, true, handler);
		}
		catch (const char* msg) {
			// ignore request to abort during registration
		}
	}
}

static ImageLoader* libraryLocator(const char* libraryName, bool search, const char* origin, const ImageLoader::RPathChain* rpaths)
{
	dyld::LoadContext context;
	context.useSearchPaths		= search;
	context.useFallbackPaths	= search;
	context.useLdLibraryPath	= false;
	context.implicitRPath		= false;
	context.matchByInstallName	= false;
	context.dontLoad			= false;
	context.mustBeBundle		= false;
	context.mustBeDylib			= true;
	context.canBePIE			= false;
	context.origin				= origin;
	context.rpath				= rpaths;
	return load(libraryName, context);
}

static const char* basename(const char* path)
{
    const char* last = path;
    for (const char* s = path; *s != '\0'; s++) {
        if (*s == '/') 
			last = s+1;
    }
    return last;
}

static void setContext(const macho_header* mainExecutableMH, int argc, const char* argv[], const char* envp[], const char* apple[])
{
	gLinkContext.loadLibrary			= &libraryLocator;
	gLinkContext.terminationRecorder	= &terminationRecorder;
	gLinkContext.flatExportFinder		= &flatFindExportedSymbol;
	gLinkContext.coalescedExportFinder	= &findCoalescedExportedSymbol;
	gLinkContext.getCoalescedImages		= &getCoalescedImages;
	gLinkContext.undefinedHandler		= &undefinedHandler;
	gLinkContext.getAllMappedRegions	= &getMappedRegions;
	gLinkContext.bindingHandler			= NULL;
	gLinkContext.notifySingle			= &notifySingle;
	gLinkContext.notifyBatch			= &notifyBatch;
	gLinkContext.removeImage			= &removeImage;
	gLinkContext.registerDOFs			= &registerDOFs;
	gLinkContext.clearAllDepths			= &clearAllDepths;
	gLinkContext.printAllDepths			= &printAllDepths;
	gLinkContext.imageCount				= &imageCount;
	gLinkContext.setNewProgramVars		= &setNewProgramVars;
#if DYLD_SHARED_CACHE_SUPPORT
	gLinkContext.inSharedCache			= &inSharedCache;
#endif
	gLinkContext.setErrorStrings		= &setErrorStrings;
#if SUPPORT_OLD_CRT_INITIALIZATION
	gLinkContext.setRunInitialzersOldWay= &setRunInitialzersOldWay;
#endif
	gLinkContext.findImageContainingAddress	= &findImageContainingAddress;
	gLinkContext.addDynamicReference	= &addDynamicReference;
	gLinkContext.bindingOptions			= ImageLoader::kBindingNone;
	gLinkContext.argc					= argc;
	gLinkContext.argv					= argv;
	gLinkContext.envp					= envp;
	gLinkContext.apple					= apple;
	gLinkContext.progname				= (argv[0] != NULL) ? basename(argv[0]) : "";
	gLinkContext.programVars.mh			= mainExecutableMH;
	gLinkContext.programVars.NXArgcPtr	= &gLinkContext.argc;
	gLinkContext.programVars.NXArgvPtr	= &gLinkContext.argv;
	gLinkContext.programVars.environPtr	= &gLinkContext.envp;
	gLinkContext.programVars.__prognamePtr=&gLinkContext.progname;
	gLinkContext.mainExecutable			= NULL;
	gLinkContext.imageSuffix			= NULL;
	gLinkContext.dynamicInterposeArray	= NULL;
	gLinkContext.dynamicInterposeCount	= 0;
	gLinkContext.prebindUsage			= ImageLoader::kUseAllPrebinding;
#if TARGET_IPHONE_SIMULATOR
	gLinkContext.sharedRegionMode		= ImageLoader::kDontUseSharedRegion;
#else
	gLinkContext.sharedRegionMode		= ImageLoader::kUseSharedRegion;
#endif
}


#if __LP64__
	#define LC_SEGMENT_COMMAND		LC_SEGMENT_64
	#define macho_segment_command	segment_command_64
	#define macho_section			section_64
#else
	#define LC_SEGMENT_COMMAND		LC_SEGMENT
	#define macho_segment_command	segment_command
	#define macho_section			section
#endif


//
// Look for a special segment in the mach header. 
// Its presences means that the binary wants to have DYLD ignore
// DYLD_ environment variables.
//
static bool hasRestrictedSegment(const macho_header* mh)
{
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
			{
				const struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
				
				//dyld::log("seg name: %s\n", seg->segname);
				if (strcmp(seg->segname, "__RESTRICT") == 0) {
					const struct macho_section* const sectionsStart = (struct macho_section*)((char*)seg + sizeof(struct macho_segment_command));
					const struct macho_section* const sectionsEnd = &sectionsStart[seg->nsects];
					for (const struct macho_section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
						if (strcmp(sect->sectname, "__restrict") == 0) 
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

#if SUPPORT_VERSIONED_PATHS
//
// Peeks at a dylib file and returns its current_version and install_name.
// Returns false on error.
//			
static bool getDylibVersionAndInstallname(const char* dylibPath, uint32_t* version, char* installName)
{
	// open file (automagically closed when this function exits)
	FileOpener file(dylibPath);
	
	if ( file.getFileDescriptor() == -1 ) 
		return false;
	
	uint8_t firstPage[4096];
	if ( pread(file.getFileDescriptor(), firstPage, 4096, 0) != 4096 )
		return false;

	// if fat wrapper, find usable sub-file
	const fat_header* fileStartAsFat = (fat_header*)firstPage;
	if ( fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		uint64_t fileOffset;
		uint64_t fileLength;
		if ( fatFindBest(fileStartAsFat, &fileOffset, &fileLength) ) {
			if ( pread(file.getFileDescriptor(), firstPage, 4096, fileOffset) != 4096 )
				return false;
		}
		else {
			return false;
		}
	}

	// check mach-o header
	const mach_header* mh = (mach_header*)firstPage;
	if ( mh->magic != sMainExecutableMachHeader->magic ) 
		return false;
	if ( mh->cputype != sMainExecutableMachHeader->cputype )
		return false;

	// scan load commands for LC_ID_DYLIB
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* const cmdsReadEnd = (struct load_command*)(((char*)mh)+4096);
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_ID_DYLIB:
			{
				const struct dylib_command* id = (struct dylib_command*)cmd;
				*version = id->dylib.current_version;
				if ( installName != NULL )
					strlcpy(installName, (char *)id + id->dylib.name.offset, PATH_MAX);
				return true;
			}
			break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
		if ( cmd > cmdsReadEnd )
			return false;
	}
	
	return false;
}
#endif // SUPPORT_VERSIONED_PATHS
						

#if 0
static void printAllImages()
{
	dyld::log("printAllImages()\n");
	for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
		ImageLoader* image = *it;
		dyld_image_states imageState = image->getState();
		dyld::log("  state=%d, dlopen-count=%d, never-unload=%d, in-use=%d, name=%s\n",
				  imageState, image->dlopenCount(), image->neverUnload(), image->isMarkedInUse(), image->getShortName());
	}
}
#endif

void link(ImageLoader* image, bool forceLazysBound, bool neverUnload, const ImageLoader::RPathChain& loaderRPaths)
{
	// add to list of known images.  This did not happen at creation time for bundles
	if ( image->isBundle() && !image->isLinked() )
		addImage(image);

	// we detect root images as those not linked in yet 
	if ( !image->isLinked() )
		addRootImage(image);
	
	// process images
	try {
		image->link(gLinkContext, forceLazysBound, false, neverUnload, loaderRPaths);
	}
	catch (const char* msg) {
		garbageCollectImages();
		throw;
	}
}


void runInitializers(ImageLoader* image)
{
	// do bottom up initialization
	ImageLoader::InitializerTimingList initializerTimes[sAllImages.size()];
	initializerTimes[0].count = 0;
	image->runInitializers(gLinkContext, initializerTimes[0]);
}

// This function is called at the end of dlclose() when the reference count goes to zero.
// The dylib being unloaded may have brought in other dependent dylibs when it was loaded.
// Those dependent dylibs need to be unloaded, but only if they are not referenced by
// something else.  We use a standard mark and sweep garbage collection.
//
// The tricky part is that when a dylib is unloaded it may have a termination function that
// can run and itself call dlclose() on yet another dylib.  The problem is that this
// sort of gabage collection is not re-entrant.  Instead a terminator's call to dlclose()
// which calls garbageCollectImages() will just set a flag to re-do the garbage collection
// when the current pass is done.
//
// Also note that this is done within the dyld global lock, so it is always single threaded.
//
void garbageCollectImages()
{
	static bool sDoingGC = false;
	static bool sRedo = false;

	if ( sDoingGC ) {
		// GC is currently being run, just set a flag to have it run again.
		sRedo = true;
		return;
	}
	
	sDoingGC = true;
	do {
		sRedo = false;
		
		// mark phase: mark all images not-in-use
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* image = *it;
			//dyld::log("gc: neverUnload=%d name=%s\n", image->neverUnload(), image->getShortName());
			image->markNotUsed();
		}
		
		// sweep phase: mark as in-use, images reachable from never-unload or in-use image
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* image = *it;
			if ( (image->dlopenCount() != 0) || image->neverUnload() ) {
				image->markedUsedRecursive(sDynamicReferences);
			}
		}

		// collect phase: build array of images not marked in-use
		ImageLoader* deadImages[sAllImages.size()];
		unsigned deadCount = 0;
		unsigned i = 0;
		for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
			ImageLoader* image = *it;
			if ( ! image->isMarkedInUse() ) {
				deadImages[i++] = image;
				if (gLogAPIs) dyld::log("dlclose(), found unused image %p %s\n", image, image->getShortName());
				++deadCount;
			}
		}

		// collect phase: run termination routines for images not marked in-use
		const int maxRangeCount = deadCount*2;
		__cxa_range_t ranges[maxRangeCount];
		int rangeCount = 0;
		for (unsigned i=0; i < deadCount; ++i) {
			ImageLoader* image = deadImages[i];
			for (unsigned int j=0; j < image->segmentCount(); ++j) {
				if ( !image->segExecutable(j) )
					continue;
				if ( rangeCount < maxRangeCount ) {
					ranges[rangeCount].addr = (const void*)image->segActualLoadAddress(j);
					ranges[rangeCount].length = image->segSize(j);
					++rangeCount;
				}
			}
			try {
				runImageStaticTerminators(image);
			}
			catch (const char* msg) {
				dyld::warn("problem running terminators for image: %s\n", msg);
			}
		}
		
		// <rdar://problem/14718598> dyld should call __cxa_finalize_ranges()
		if ( (rangeCount > 0) && (gLibSystemHelpers != NULL) && (gLibSystemHelpers->version >= 13) )
			(*gLibSystemHelpers->cxa_finalize_ranges)(ranges, rangeCount);

		// collect phase: delete all images which are not marked in-use
		bool mightBeMore;
		do {
			mightBeMore = false;
			for (std::vector<ImageLoader*>::iterator it=sAllImages.begin(); it != sAllImages.end(); it++) {
				ImageLoader* image = *it;
				if ( ! image->isMarkedInUse() ) {
					try {
						if (gLogAPIs) dyld::log("dlclose(), deleting %p %s\n", image, image->getShortName());
						removeImage(image);
						ImageLoader::deleteImage(image);
						mightBeMore = true;
						break;  // interator in invalidated by this removal
					}
					catch (const char* msg) {
						dyld::warn("problem deleting image: %s\n", msg);
					}
				}
			}
		} while ( mightBeMore );
	} while (sRedo);
	sDoingGC = false;

	//printAllImages();

}


static void preflight_finally(ImageLoader* image)
{
	if ( image->isBundle() ) {
		removeImageFromAllImages(image->machHeader());
		ImageLoader::deleteImage(image);
	}
	sBundleBeingLoaded = NULL;
	dyld::garbageCollectImages();
}


void preflight(ImageLoader* image, const ImageLoader::RPathChain& loaderRPaths)
{
	try {
		if ( image->isBundle() ) 
			sBundleBeingLoaded = image;	// hack
		image->link(gLinkContext, false, true, false, loaderRPaths);
	}
	catch (const char* msg) {	
		preflight_finally(image);
		throw;
	}
	preflight_finally(image);
}

#if __x86_64__
static bool isHaswell()
{
#if TARGET_IPHONE_SIMULATOR
	return false;
#else
	// check system is capable of running x86_64h code
	struct host_basic_info info;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	mach_port_t hostPort = mach_host_self();
	kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
	mach_port_deallocate(mach_task_self(), hostPort);
	if ( result != KERN_SUCCESS )
		return false;
	return ( info.cpu_subtype == CPU_SUBTYPE_X86_64_H );
#endif
}
#endif

static void loadInsertedDylib(const char* path)
{
	ImageLoader* image = NULL;
	try {
		LoadContext context;
		context.useSearchPaths		= false;
		context.useFallbackPaths	= false;
		context.useLdLibraryPath	= false;
		context.implicitRPath		= false;
		context.matchByInstallName	= false;
		context.dontLoad			= false;
		context.mustBeBundle		= false;
		context.mustBeDylib			= true;
		context.canBePIE			= false;
		context.origin				= NULL;	// can't use @loader_path with DYLD_INSERT_LIBRARIES
		context.rpath				= NULL;
		image = load(path, context);
	}
	catch (const char* msg) {
#if TARGET_IPHONE_SIMULATOR
		dyld::log("dyld: warning: could not load inserted library '%s' because %s\n", path, msg);
#else
		halt(dyld::mkstringf("could not load inserted library '%s' because %s\n", path, msg));
#endif
	}
	catch (...) {
		halt(dyld::mkstringf("could not load inserted library '%s'\n", path));
	}
}

static bool processRestricted(const macho_header* mainExecutableMH)
{	
#if __MAC_OS_X_VERSION_MIN_REQUIRED
    // ask kernel if code signature of program makes it restricted
    uint32_t flags;
	if ( csops(0, CS_OPS_STATUS, &flags, sizeof(flags)) != -1 ) {
		if ( flags & CS_ENFORCEMENT ) {
			gLinkContext.codeSigningEnforced = true;
		}
	}
	if (flags & CS_RESTRICT) {
		sRestrictedReason = restrictedByEntitlements;
		return true;
	}
#else
	gLinkContext.codeSigningEnforced = true;
#endif
	
	// all processes with setuid or setgid bit set are restricted
    if ( issetugid() ) {
		sRestrictedReason = restrictedBySetGUid;
		return true;
	}
		
	// <rdar://problem/13158444&13245742> Respect __RESTRICT,__restrict section for root processes
	if ( hasRestrictedSegment(mainExecutableMH) ) {
		// existence of __RESTRICT/__restrict section make process restricted
		sRestrictedReason = restrictedBySegment;
		return true;
	}
    return false;
}


bool processIsRestricted()
{
	return sProcessIsRestricted;
}


// <rdar://problem/10583252> Add dyld to uuidArray to enable symbolication of stackshots
static void addDyldImageToUUIDList()
{
	const struct macho_header* mh = (macho_header*)&__dso_handle;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)((char*)mh + sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_UUID: {
				uuid_command* uc = (uuid_command*)cmd;
				dyld_uuid_info info;
				info.imageLoadAddress = (mach_header*)mh;
				memcpy(info.imageUUID, uc->uuid, 16);
				addNonSharedCacheImageUUID(info);
				return;
			}
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
}

#if __MAC_OS_X_VERSION_MIN_REQUIRED
typedef int (*open_proc_t)(const char*, int, int);
typedef int (*fcntl_proc_t)(int, int, void*);
typedef int (*ioctl_proc_t)(int, unsigned long, void*);
static void* getProcessInfo() { return dyld::gProcessInfo; }
static SyscallHelpers sSysCalls = {
		3,
		// added in version 1
		(open_proc_t)&open, 
		&close, 
		&pread, 
		&write, 
		&mmap, 
		&munmap, 
		&madvise,
		&stat, 
		(fcntl_proc_t)&fcntl, 
		(ioctl_proc_t)&ioctl, 
		&issetugid, 
		&getcwd, 
		&realpath, 
		&vm_allocate, 
		&vm_deallocate,
		&vm_protect,
		&vlog, 
		&vwarn, 
		&pthread_mutex_lock, 
		&pthread_mutex_unlock,
		&mach_thread_self, 
		&mach_port_deallocate, 
		&task_self_trap,
		&mach_timebase_info,
		&OSAtomicCompareAndSwapPtrBarrier, 
		&OSMemoryBarrier,
		&getProcessInfo,
		&__error,
		&mach_absolute_time,
		// added in version 2
		&thread_switch,
		// added in version 3
		&opendir,
		&readdir_r,
		&closedir
};

__attribute__((noinline))
static uintptr_t useSimulatorDyld(int fd, const macho_header* mainExecutableMH, const char* dyldPath, 
								int argc, const char* argv[], const char* envp[], const char* apple[], uintptr_t* startGlue)
{
	*startGlue = 0;
	
	// verify simulator dyld file is owned by root
	struct stat sb;
	if ( fstat(fd, &sb) == -1 )
		return 0;
	if ( sb.st_uid != 0 )
		return 0;

	// read first page of dyld file
	uint8_t firstPage[4096];
	if ( pread(fd, firstPage, 4096, 0) != 4096 )
		return 0;
	
	// if fat file, pick matching slice
	uint64_t fileOffset = 0;
	uint64_t fileLength = sb.st_size;
	const fat_header* fileStartAsFat = (fat_header*)firstPage;
	if ( fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
		if ( !fatFindBest(fileStartAsFat, &fileOffset, &fileLength) ) 
			return 0;
		// re-read buffer from start of mach-o slice in fat file
		if ( pread(fd, firstPage, 4096, fileOffset) != 4096 )
			return 0;
	}
	else if ( !isCompatibleMachO(firstPage, dyldPath) ) {
		return 0;
	}
	
	// calculate total size of dyld segments
	const macho_header* mh = (const macho_header*)firstPage;
	uintptr_t mappingSize = 0;
	uintptr_t preferredLoadAddress = 0;
	const uint32_t cmd_count = mh->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)mh)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					mappingSize += seg->vmsize;
					if ( seg->fileoff == 0 )
						preferredLoadAddress = seg->vmaddr;
				}
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}

	// reserve space, then mmap each segment
	vm_address_t loadAddress = 0;
	uintptr_t entry = 0;
	if ( ::vm_allocate(mach_task_self(), &loadAddress, mappingSize, VM_FLAGS_ANYWHERE) != 0 )
		return 0;
	cmd = cmds;
	struct linkedit_data_command* codeSigCmd = NULL;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					struct macho_segment_command* seg = (struct macho_segment_command*)cmd;
					uintptr_t requestedLoadAddress = seg->vmaddr - preferredLoadAddress + loadAddress;
					void* segAddress = ::mmap((void*)requestedLoadAddress, seg->filesize, seg->initprot, MAP_FIXED | MAP_PRIVATE, fd, fileOffset + seg->fileoff);
					//dyld::log("dyld_sim %s mapped at %p\n", seg->segname, segAddress);
					if ( segAddress == (void*)(-1) )
						return 0;
				}
				break;
			case LC_UNIXTHREAD:
				{
				#if __i386__
					const i386_thread_state_t* registers = (i386_thread_state_t*)(((char*)cmd) + 16);
					entry = (registers->__eip + loadAddress - preferredLoadAddress);
				#elif __x86_64__
					const x86_thread_state64_t* registers = (x86_thread_state64_t*)(((char*)cmd) + 16);
					entry = (registers->__rip + loadAddress - preferredLoadAddress);
				#endif
				}
				break;
			case LC_CODE_SIGNATURE:
				codeSigCmd = (struct linkedit_data_command*)cmd;
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	
	if ( codeSigCmd != NULL ) {
		fsignatures_t siginfo;
		siginfo.fs_file_start=fileOffset;							// start of mach-o slice in fat file 
		siginfo.fs_blob_start=(void*)(long)(codeSigCmd->dataoff);	// start of code-signature in mach-o file
		siginfo.fs_blob_size=codeSigCmd->datasize;					// size of code-signature
		int result = fcntl(fd, F_ADDFILESIGS, &siginfo);
		if ( result == -1 ) {
			if ( (errno == EPERM) || (errno == EBADEXEC) )
				return 0;
		}
	}
	close(fd);

	// notify debugger that dyld_sim is loaded
	dyld_image_info info;
	info.imageLoadAddress = (mach_header*)loadAddress;
	info.imageFilePath	  = strdup(dyldPath);
	info.imageFileModDate = sb.st_mtime;
	addImagesToAllImages(1, &info);
	dyld::gProcessInfo->notification(dyld_image_adding, 1, &info);
	
	// jump into new simulator dyld
	typedef uintptr_t (*sim_entry_proc_t)(int argc, const char* argv[], const char* envp[], const char* apple[],
								const macho_header* mainExecutableMH, const macho_header* dyldMH, uintptr_t dyldSlide,
								const dyld::SyscallHelpers* vtable, uintptr_t* startGlue);
	sim_entry_proc_t newDyld = (sim_entry_proc_t)entry;
	return (*newDyld)(argc, argv, envp, apple, mainExecutableMH, (macho_header*)loadAddress, 
					 loadAddress - preferredLoadAddress, 
					 &sSysCalls, startGlue);
}
#endif


//
// Entry point for dyld.  The kernel loads dyld and jumps to __dyld_start which
// sets up some registers and call this function.
//
// Returns address of main() in target program which __dyld_start jumps to
//
uintptr_t
_main(const macho_header* mainExecutableMH, uintptr_t mainExecutableSlide, 
		int argc, const char* argv[], const char* envp[], const char* apple[], 
		uintptr_t* startGlue)
{
	uintptr_t result = 0;
	sMainExecutableMachHeader = mainExecutableMH;
#if !TARGET_IPHONE_SIMULATOR
	const char* loggingPath = _simple_getenv(envp, "DYLD_PRINT_TO_FILE");
	if ( loggingPath != NULL ) {
		int fd = open(loggingPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if ( fd != -1 ) {
			sLogfile = fd;
			sLogToFile = true;
		}
		else {
			dyld::log("dyld: could not open DYLD_PRINT_TO_FILE='%s', errno=%d\n", loggingPath, errno);
		}
	}
#endif
#if __MAC_OS_X_VERSION_MIN_REQUIRED
	// if this is host dyld, check to see if iOS simulator is being run
	const char* rootPath = _simple_getenv(envp, "DYLD_ROOT_PATH");
	if ( rootPath != NULL ) {
		// look to see if simulator has its own dyld
		char simDyldPath[PATH_MAX]; 
		strlcpy(simDyldPath, rootPath, PATH_MAX);
		strlcat(simDyldPath, "/usr/lib/dyld_sim", PATH_MAX);
		int fd = my_open(simDyldPath, O_RDONLY, 0);
		if ( fd != -1 ) {
			result = useSimulatorDyld(fd, mainExecutableMH, simDyldPath, argc, argv, envp, apple, startGlue);
			if ( !result && (*startGlue == 0) )
				halt("problem loading iOS simulator dyld");
			return result;
		}
	}
#endif

	CRSetCrashLogMessage("dyld: launch started");

#if LOG_BINDINGS
	char bindingsLogPath[256];
	
	const char* shortProgName = "unknown";
	if ( argc > 0 ) {
		shortProgName = strrchr(argv[0], '/');
		if ( shortProgName == NULL )
			shortProgName = argv[0];
		else 
			++shortProgName;
	}
	mysprintf(bindingsLogPath, "/tmp/bindings/%d-%s", getpid(), shortProgName);
	sBindingsLogfile = open(bindingsLogPath, O_WRONLY | O_CREAT, 0666);
	if ( sBindingsLogfile == -1 ) {
		::mkdir("/tmp/bindings", 0777);
		sBindingsLogfile = open(bindingsLogPath, O_WRONLY | O_CREAT, 0666);
	}
	//dyld::log("open(%s) => %d, errno = %d\n", bindingsLogPath, sBindingsLogfile, errno);
#endif	
	setContext(mainExecutableMH, argc, argv, envp, apple);

	// Pickup the pointer to the exec path.
	sExecPath = _simple_getenv(apple, "executable_path");

	// <rdar://problem/13868260> Remove interim apple[0] transition code from dyld
	if (!sExecPath) sExecPath = apple[0];
	
	sExecPath = apple[0];
	bool ignoreEnvironmentVariables = false;
	if ( sExecPath[0] != '/' ) {
		// have relative path, use cwd to make absolute
		char cwdbuff[MAXPATHLEN];
	    if ( getcwd(cwdbuff, MAXPATHLEN) != NULL ) {
			// maybe use static buffer to avoid calling malloc so early...
			char* s = new char[strlen(cwdbuff) + strlen(sExecPath) + 2];
			strcpy(s, cwdbuff);
			strcat(s, "/");
			strcat(s, sExecPath);
			sExecPath = s;
		}
	}
	// Remember short name of process for later logging
	sExecShortName = ::strrchr(sExecPath, '/');
	if ( sExecShortName != NULL )
		++sExecShortName;
	else
		sExecShortName = sExecPath;
    sProcessIsRestricted = processRestricted(mainExecutableMH);
    if ( sProcessIsRestricted ) {
#if SUPPORT_LC_DYLD_ENVIRONMENT
		checkLoadCommandEnvironmentVariables();
#if SUPPORT_VERSIONED_PATHS
		checkVersionedPaths();
#endif	
#endif 	
		pruneEnvironmentVariables(envp, &apple);
		// set again because envp and apple may have changed or moved
		setContext(mainExecutableMH, argc, argv, envp, apple);
	}
	else
		checkEnvironmentVariables(envp, ignoreEnvironmentVariables);
	if ( sEnv.DYLD_PRINT_OPTS ) 
		printOptions(argv);
	if ( sEnv.DYLD_PRINT_ENV ) 
		printEnvironmentVariables(envp);
	getHostInfo();
	// install gdb notifier
	stateToHandlers(dyld_image_state_dependents_mapped, sBatchHandlers)->push_back(notifyGDB);
	stateToHandlers(dyld_image_state_mapped, sSingleHandlers)->push_back(updateAllImages);
	// make initial allocations large enough that it is unlikely to need to be re-alloced
	sAllImages.reserve(INITIAL_IMAGE_COUNT);
	sImageRoots.reserve(16);
	sAddImageCallbacks.reserve(4);
	sRemoveImageCallbacks.reserve(4);
	sImageFilesNeedingTermination.reserve(16);
	sImageFilesNeedingDOFUnregistration.reserve(8);
	
#ifdef WAIT_FOR_SYSTEM_ORDER_HANDSHAKE
	// <rdar://problem/6849505> Add gating mechanism to dyld support system order file generation process
	WAIT_FOR_SYSTEM_ORDER_HANDSHAKE(dyld::gProcessInfo->systemOrderFlag);
#endif
	

	try {
		// add dyld itself to UUID list
		addDyldImageToUUIDList();
		CRSetCrashLogMessage(sLoadingCrashMessage);
		// instantiate ImageLoader for main executable
		sMainExecutable = instantiateFromLoadedImage(mainExecutableMH, mainExecutableSlide, sExecPath);
		gLinkContext.mainExecutable = sMainExecutable;
		gLinkContext.processIsRestricted = sProcessIsRestricted;
		gLinkContext.mainExecutableCodeSigned = hasCodeSignatureLoadCommand(mainExecutableMH);

#if TARGET_IPHONE_SIMULATOR
		// check main executable is not too new for this OS
		{
			if ( ! isSimulatorBinary((uint8_t*)mainExecutableMH, sExecPath) ) {
				throwf("program was built for Mac OS X and cannot be run in simulator"); 
			}
			uint32_t mainMinOS = sMainExecutable->minOSVersion();
			// dyld is always built for the current OS, so we can get the current OS version
			// from the load command in dyld itself.
			uint32_t dyldMinOS = ImageLoaderMachO::minOSVersion((const mach_header*)&__dso_handle);
			if ( mainMinOS > dyldMinOS ) {
				throwf("app was built for iOS %d.%d which is newer than this simulator %d.%d", 
						mainMinOS >> 16, ((mainMinOS >> 8) & 0xFF),
						dyldMinOS >> 16, ((dyldMinOS >> 8) & 0xFF));
			}
		}
#endif

		// load shared cache
	#if __x86_64__
		sHaswell = isHaswell();
	#endif
		checkSharedRegionDisable();
	#if DYLD_SHARED_CACHE_SUPPORT
		if ( gLinkContext.sharedRegionMode != ImageLoader::kDontUseSharedRegion )
			mapSharedCache();
	#endif
		// load any inserted libraries
		if	( sEnv.DYLD_INSERT_LIBRARIES != NULL ) {
			for (const char* const* lib = sEnv.DYLD_INSERT_LIBRARIES; *lib != NULL; ++lib) 
				loadInsertedDylib(*lib);
		}
		// record count of inserted libraries so that a flat search will look at 
		// inserted libraries, then main, then others.
		sInsertedDylibCount = sAllImages.size()-1;

		// link main executable
		gLinkContext.linkingMainExecutable = true;
		link(sMainExecutable, sEnv.DYLD_BIND_AT_LAUNCH, true, ImageLoader::RPathChain(NULL, NULL));
		sMainExecutable->setNeverUnloadRecursive();
		if ( sMainExecutable->forceFlat() ) {
			gLinkContext.bindFlat = true;
			gLinkContext.prebindUsage = ImageLoader::kUseNoPrebinding;
		}

		// link any inserted libraries
		// do this after linking main executable so that any dylibs pulled in by inserted 
		// dylibs (e.g. libSystem) will not be in front of dylibs the program uses
		if ( sInsertedDylibCount > 0 ) {
			for(unsigned int i=0; i < sInsertedDylibCount; ++i) {
				ImageLoader* image = sAllImages[i+1];
				link(image, sEnv.DYLD_BIND_AT_LAUNCH, true, ImageLoader::RPathChain(NULL, NULL));
				image->setNeverUnloadRecursive();
			}
			// only INSERTED libraries can interpose
			// register interposing info after all inserted libraries are bound so chaining works
			for(unsigned int i=0; i < sInsertedDylibCount; ++i) {
				ImageLoader* image = sAllImages[i+1];
				image->registerInterposing();
			}
		}
		// apply interposing to initial set of images
		for(int i=0; i < sImageRoots.size(); ++i) {
			sImageRoots[i]->applyInterposing(gLinkContext);
		}
		gLinkContext.linkingMainExecutable = false;
		
		// <rdar://problem/12186933> do weak binding only after all inserted images linked
		sMainExecutable->weakBind(gLinkContext);
		
		CRSetCrashLogMessage("dyld: launch, running initializers");
	#if SUPPORT_OLD_CRT_INITIALIZATION
		// Old way is to run initializers via a callback from crt1.o
		if ( ! gRunInitializersOldWay ) 
			initializeMainExecutable(); 
	#else
		// run all initializers
		initializeMainExecutable(); 
	#endif
		// find entry point for main executable
		result = (uintptr_t)sMainExecutable->getThreadPC();
		if ( result != 0 ) {
			// main executable uses LC_MAIN, needs to return to glue in libdyld.dylib
			if ( (gLibSystemHelpers != NULL) && (gLibSystemHelpers->version >= 9) )
				*startGlue = (uintptr_t)gLibSystemHelpers->startGlueToCallExit;
			else
				halt("libdyld.dylib support not present for LC_MAIN");
		}
		else {
			// main executable uses LC_UNIXTHREAD, dyld needs to let "start" in program set up for main()
			result = (uintptr_t)sMainExecutable->getMain();
			*startGlue = 0;
		}
	}
	catch(const char* message) {
		syncAllImages();
		halt(message);
	}
	catch(...) {
		dyld::log("dyld: launch failed\n");
	}

	CRSetCrashLogMessage(NULL);
	
	return result;
}



} // namespace



