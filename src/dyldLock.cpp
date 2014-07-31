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

#include <pthread.h>

#include "dyldLock.h"

// until the reader/writer locks are fully tested, we just use a simple recursive mutex
#define REAL_READER_WRITER_LOCK 0



//
// This class implements a recursive reader/writer lock.
// Recursive means a thread that has already aquired the lock can re-acquire it.
// The lock allows either:
//      a) one "writer" thread to hold lock
//		b) multiple "reader" threads to hold
//
// A thread with the writer can acquire a reader lock.
// A thread with a reader lock can acquire a writer lock iff it is the only thread with a reader lock
//
class RecursiveReaderWriterLock 
{
public:
			RecursiveReaderWriterLock() __attribute__((visibility("hidden")));
	void	initIfNeeded();
	
	
	void	lockForSingleWritingThread() __attribute__((visibility("hidden")));
	void	unlockForSingleWritingThread() __attribute__((visibility("hidden")));
	
	void	lockForMultipleReadingThreads() __attribute__((visibility("hidden")));
	void	unlockForMultipleReadingThreads() __attribute__((visibility("hidden")));

private:
#if REAL_READER_WRITER_LOCK
	struct ThreadRecursionCount {
		pthread_t	fThread;
		uint32_t	fCount;
	};
	bool	writerThreadIsAnyThreadBut(pthread_t thread);
	void	writerThreadRetain(pthread_t thread);
	bool	writerThreadRelease(pthread_t thread);

	enum { kMaxReaderThreads = 4 };
	bool	readerThreadSetRetain(pthread_t thread);
	bool	readerThreadSetRelease(pthread_t thread);
	bool	readerThreadSetContainsAnotherThread(pthread_t thread);

	ThreadRecursionCount	fWriterThread;
	ThreadRecursionCount	fReaderThreads[kMaxReaderThreads];
	pthread_cond_t			fLockFree;
	pthread_mutex_t			fMutex;
#else
	pthread_mutex_t			fMutex;
#endif
	bool					fInitialized;	// assumes this is statically initialized to false because sLock is static
};


//
// initIfNeeded() is a hack so that when libSystem_debug.dylb is useable.
// The problem is that Objective-C +load methods are called before C++ initialziers are run
// If +load method makes a call into dyld, sLock is not initialized.  
//
// The long term solution is for objc and dyld to work more closely together so that instead
// of running all +load methods before all initializers, we run each image's +load then its
// initializers all in bottom up order.
//
// This lazy initialization is not thread safe, but as long as someone does not create a 
// new thread in a +load method, the C++ constructor for sLock will be called before main()
// so there will only be one thead.
//
void RecursiveReaderWriterLock::initIfNeeded()
{
	if ( ! fInitialized ) {
		pthread_mutexattr_t recursiveMutexAttr;
		pthread_mutexattr_init(&recursiveMutexAttr);
		pthread_mutexattr_settype(&recursiveMutexAttr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&fMutex, &recursiveMutexAttr);
	#if REAL_READER_WRITER_LOCK
		pthread_cond_init(&fLockFree, NULL);
		fWriterThread.fThread = NULL;
		fWriterThread.fCount = 0;
		for (int i=0; i < kMaxReaderThreads; ++i) {
			fReaderThreads[i].fThread = NULL;
			fReaderThreads[i].fCount = 0;
		}
	#endif
		fInitialized = true;
	}
}

RecursiveReaderWriterLock::RecursiveReaderWriterLock()
{
	initIfNeeded();
}

void RecursiveReaderWriterLock::lockForSingleWritingThread()
{
	this->initIfNeeded();
#if REAL_READER_WRITER_LOCK
	pthread_mutex_lock(&fMutex);
	pthread_t thisThread = pthread_self();
	// wait as long as there is another writer or any readers on a different thread
	while ( writerThreadIsAnyThreadBut(thisThread) || readerThreadSetContainsAnotherThread(thisThread) ) {
		pthread_cond_wait(&fLockFree, &fMutex);
	}
	writerThreadRetain(thisThread);
	pthread_mutex_unlock(&fMutex);
#else
	pthread_mutex_lock(&fMutex);
#endif
}

void RecursiveReaderWriterLock::unlockForSingleWritingThread()
{
	this->initIfNeeded();
#if REAL_READER_WRITER_LOCK
	pthread_mutex_lock(&fMutex);
	if ( writerThreadRelease(pthread_self()) ) {
		pthread_cond_broadcast(&fLockFree);
	}
	pthread_mutex_unlock(&fMutex);
#else
	pthread_mutex_unlock(&fMutex);
#endif
}


void RecursiveReaderWriterLock::lockForMultipleReadingThreads()
{
	this->initIfNeeded();
#if REAL_READER_WRITER_LOCK
	pthread_mutex_lock(&fMutex);
	pthread_t thisThread = pthread_self();
	// wait as long as there is a writer on another thread or too many readers already
	while ( writerThreadIsAnyThreadBut(thisThread) || !readerThreadSetRetain(thisThread) ) {
		pthread_cond_wait(&fLockFree, &fMutex);
	}
	pthread_mutex_unlock(&fMutex);
#else
	pthread_mutex_lock(&fMutex);
#endif
}


void RecursiveReaderWriterLock::unlockForMultipleReadingThreads()
{
	this->initIfNeeded();
#if REAL_READER_WRITER_LOCK
	pthread_mutex_lock(&fMutex);
	if ( readerThreadSetRelease(pthread_self()) ) {
		pthread_cond_broadcast(&fLockFree);
	}
	pthread_mutex_unlock(&fMutex);
#else
	pthread_mutex_unlock(&fMutex);
#endif
}

#if REAL_READER_WRITER_LOCK
bool RecursiveReaderWriterLock::writerThreadIsAnyThreadBut(pthread_t thread)
{
	return ( (fWriterThread.fThread != NULL) && (fWriterThread.fThread != thread) );
}

void RecursiveReaderWriterLock::writerThreadRetain(pthread_t thread)
{
	++fWriterThread.fCount;
}

bool RecursiveReaderWriterLock::writerThreadRelease(pthread_t thread)
{
	return ( --fWriterThread.fCount == 0 );
}


bool RecursiveReaderWriterLock::readerThreadSetRetain(pthread_t thread)
{
	// if thread is already in set, bump its count
	for (int i=0; i < kMaxReaderThreads; ++i) {
		if ( fReaderThreads[i].fThread == thread ) {
			++fReaderThreads[i].fCount;
			return true;
		}
	}
	// find empty slot in set
	for (int i=0; i < kMaxReaderThreads; ++i) {
		if ( fReaderThreads[i].fThread == NULL ) {
			fReaderThreads[i].fThread = thread;
			fReaderThreads[i].fCount = 1;
			return true;
		}
	}

	// all reader slots full
	return false;
}

bool RecursiveReaderWriterLock::readerThreadSetRelease(pthread_t thread)
{
	for (int i=0; i < kMaxReaderThreads; ++i) {
		if ( fReaderThreads[i].fThread == thread ) {
			if ( --fReaderThreads[i].fCount == 0 ) {
				fReaderThreads[i].fThread = NULL;
				return true;
			}
			return false;
		}
	}
	// should never get here
	return false;
}

bool RecursiveReaderWriterLock::readerThreadSetContainsAnotherThread(pthread_t thread)
{
	for (int i=0; i < kMaxReaderThreads; ++i) {
		if ( (fReaderThreads[i].fThread != NULL) && (fReaderThreads[i].fThread != thread) ) 
			return true;
	}
	return false;
}
#endif


// dyld's global reader/writer lock
static RecursiveReaderWriterLock	sLock;


LockReaderHelper::LockReaderHelper() 
{ 
	sLock.lockForMultipleReadingThreads();
}

LockReaderHelper::~LockReaderHelper() 
{ 
	sLock.unlockForMultipleReadingThreads();
}


LockWriterHelper::LockWriterHelper() 
{ 
	sLock.lockForSingleWritingThread();
}

LockWriterHelper::~LockWriterHelper() 
{ 
	sLock.unlockForSingleWritingThread();
}


// needed by lazy binding
void lockForLazyBinding() 
{
	sLock.lockForMultipleReadingThreads();
}

void unlockForLazyBinding() 
{
	sLock.unlockForMultipleReadingThreads();
}



