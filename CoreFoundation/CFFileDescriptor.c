/*
 * Copyright (c) 2009 Stuart Crook.  All rights reserved.
 *
 * Created by Stuart Crook on 02/03/2009.
 * This source code is a reverse-engineered implementation of the notification center from
 * Apple's core foundation library.
 *
 * The PureFoundation code base consists of a combination of original code provided by contributors
 * and open-source code drawn from a nuber of other projects -- chiefly Cocotron (www.coctron.org)
 * and GNUStep (www.gnustep.org). Under the principal that the least-liberal licence trumps the others,
 * the PureFoundation project as a whole is released under the GNU Lesser General Public License (LGPL).
 * Where code has been included from other projects, that project's licence, along with a note of the
 * exact source (eg. file name) is included inline in the source.
 *
 * Since PureFoundation is a dynamically-loaded shared library, it is my interpretation of the LGPL
 * that any application linking to it is not automatically bound by its terms.
 *
 * See the text of the LGPL (from http://www.gnu.org/licenses/lgpl-3.0.txt, accessed 26/2/09):
 * 
 */

#include <CoreFoundation/CFRunLoop.h>
#include "CFPriv.h"
#include "CFInternal.h"
#include "CFFileDescriptor.h"

#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_LINUX
// for kqueue
#include <sys/types.h>
#if DEPLOYMENT_TARGET_MACOSX
#include <sys/event.h>
#endif
#include <sys/time.h>

// for threads
#include <pthread.h>

#if defined(__MACH__)
// for mach ports
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/notify.h>
#endif

// for close
#include <unistd.h>
#elif DEPLOYMENT_TARGET_WINDOWS
#include <io.h>
#include <stdio.h>
#define close _close
#endif


typedef struct __CFFileDescriptor {
	CFRuntimeBase _base;
	CFFileDescriptorNativeDescriptor fd;
	CFFileDescriptorNativeDescriptor qd;
	CFFileDescriptorCallBack callback;
	CFFileDescriptorContext context; // includes info for callback
	CFRunLoopSourceRef rls;	
#if defined(__MACH__)
	mach_port_t port;
#endif
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_LINUX
	pthread_t thread;
#endif
	CFSpinLock_t lock;
} __CFFileDescriptor;


#if DEPLOYMENT_TARGET_MACOSX
/*
 *	callbacks, etc.
 */
// threaded kqueue watcher
void *_CFFDWait(void *info)
{
	CFFileDescriptorRef f = (CFFileDescriptorRef)info;

	struct kevent events[2];
	//struct kevent change[2];
	struct timespec ts = { 0, 0 };
	int res;
	mach_msg_header_t header;
	mach_msg_id_t msgid;
	mach_msg_return_t ret;
	
	header.msgh_bits = MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_MAKE_SEND);
	header.msgh_size = 0;
	header.msgh_remote_port = f->port;
	header.msgh_local_port = MACH_PORT_NULL;
	header.msgh_reserved = 0;

	while(TRUE) 
	{
		res = kevent(f->qd, NULL, 0, events, 2, NULL);
		
		if( res > 0 )
		{
			msgid = 0;
			for( int i = 0; i < res; i++ )
				msgid |= ((events[i].filter == EVFILT_READ) ? kCFFileDescriptorReadCallBack : kCFFileDescriptorWriteCallBack);

			//fprintf(stderr, "sending message, id = %d\n", msgid);
			
			header.msgh_id = msgid;
			ret = mach_msg(&header, MACH_SEND_MSG, sizeof(mach_msg_header_t), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
			
			//fprintf(stderr, "message ret = %X\n", ret);
			if( ret == MACH_MSG_SUCCESS ) fprintf(stderr, "message sent OK\n");
		}
	}
}

// runloop get port callback: lazily create and start thread, if needed
mach_port_t __CFFDGetPort(void *info)
{
	CFFileDescriptorRef f = (CFFileDescriptorRef)info;
	__CFSpinLock(&f->lock);
	if( f->port == MACH_PORT_NULL )
	{
		// create a mach_port (taken from CFMachPort source)
		mach_port_t port;
		pthread_t thread;
		
		if(KERN_SUCCESS != mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port))
		{
			__CFSpinUnlock(&f->lock);
			return MACH_PORT_NULL;
		}
		
		if(0 != pthread_create(&thread, NULL, _CFFDWait, info)) // info is the file descriptor
		{
			mach_port_destroy(mach_task_self(), port);
			__CFSpinUnlock(&f->lock);
			return MACH_PORT_NULL;
		}
		
		f->port = port;
		f->thread = thread;
	}
	__CFSpinUnlock(&f->lock);
	return f->port;
}
#endif

// main runloop callback: invoke the user's callback
void *__CFFDRunLoopCallBack(void *msg, CFIndex size, CFAllocatorRef allocator, void *info)
{
	//fprintf(stderr, "runloop callback\n");
#if defined(__MACH__)
	((__CFFileDescriptor *)info)->callback(info, ((mach_msg_header_t *)msg)->msgh_id, ((__CFFileDescriptor *)info)->context.info);
#endif
	return NULL;
}


static void __CFFileDescriptorDeallocate(CFTypeRef cf) {
    CFFileDescriptorRef f = (CFFileDescriptorRef)cf;
	__CFSpinLock(&f->lock);
	//fprintf(stderr, "deallocating a CFFileDescriptor\n");
    CFFileDescriptorInvalidate(f); // does most of the tear-down
	__CFSpinUnlock(&f->lock);
}

static const CFRuntimeClass __CFFileDescriptorClass = {
	0,
	"CFFileDescriptor",
	NULL,	// init
	NULL,	// copy
	__CFFileDescriptorDeallocate,
	NULL, //__CFDataEqual,
	NULL, //__CFDataHash,
	NULL,	// 
	NULL, //__CFDataCopyDescription
};

static CFTypeID __kCFFileDescriptorTypeID = _kCFRuntimeNotATypeID;
CFTypeID CFFileDescriptorGetTypeID(void) { return __kCFFileDescriptorTypeID; }

// register the type with the CF runtime
__private_extern__ void __CFFileDescriptorInitialize(void) {
    __kCFFileDescriptorTypeID = _CFRuntimeRegisterClass(&__CFFileDescriptorClass);
		//fprintf(stderr, "Registered CFFileDescriptor as type %d\n", __kCFFileDescriptorTypeID);
}

// use the base reserved bits for storage (like CFMachPort does)
Boolean __CFFDIsValid(CFFileDescriptorRef f) { 
	return (Boolean)__CFBitfieldGetValue(((const CFRuntimeBase *)f)->_cfinfo[CF_INFO_BITS], 0, 0); 
}

// create a file descriptor object
CFFileDescriptorRef	CFFileDescriptorCreate(CFAllocatorRef allocator, CFFileDescriptorNativeDescriptor fd, Boolean closeOnInvalidate, CFFileDescriptorCallBack callout, const CFFileDescriptorContext *context)
{
	if(callout == NULL) return NULL;

#if DEPLOYMENT_TARGET_MACOSX
	// create the kqueue and add the events we'll be monitoring, disabled
	int qd = kqueue();
		//fprintf(stderr, "kqueue() returned %d\n", qd);
#else
   int qd = 0;
#endif
	if( qd == -1 ) return NULL;

	CFIndex size = sizeof(struct __CFFileDescriptor) - sizeof(CFRuntimeBase);
	CFFileDescriptorRef memory = (CFFileDescriptorRef)_CFRuntimeCreateInstance(allocator, __kCFFileDescriptorTypeID, size, NULL);
	if (memory == NULL) { close(qd); return NULL; }

	//fprintf(stderr, "Allocated %d at %p\n", size, memory);

	memory->fd = fd;
	memory->qd = qd;
	memory->callback = callout;
	
	memory->context.version = 0;
	if( context == NULL )
	{
		memory->context.info = NULL;
		memory->context.retain = NULL;
		memory->context.release = NULL;
		memory->context.copyDescription = NULL;
	}
	else
	{
		memory->context.info = context->info;
		memory->context.retain = context->retain;
		memory->context.release = context->release;
		memory->context.copyDescription = context->copyDescription;
	}
	
	memory->rls = NULL;
#if DEPLOYMENT_TARGET_MACOSX
	memory->port = MACH_PORT_NULL;
#endif
#if DEPLOYMENT_TARGET_MACOSX || DEPLOYMENT_TARGET_LINUX
	memory->thread = NULL;
#endif
	
	__CFBitfieldSetValue(((CFRuntimeBase *)memory)->_cfinfo[CF_INFO_BITS], 0, 0, 1); // valid
	__CFBitfieldSetValue(((CFRuntimeBase *)memory)->_cfinfo[CF_INFO_BITS], 1, 1, closeOnInvalidate); 

	return memory;
}


CFFileDescriptorNativeDescriptor CFFileDescriptorGetNativeDescriptor(CFFileDescriptorRef f)
{
	//fprintf(stderr, "Entering CFFileDescriptorNativeDescriptor()\n");

	if( (f == NULL) || (CFGetTypeID(f) != CFFileDescriptorGetTypeID()) || !__CFFDIsValid(f) ) return -1;

	//fprintf(stderr, "Leaving CFFileDescriptorNativeDescriptor()\n");
	return f->fd;
}

void CFFileDescriptorGetContext(CFFileDescriptorRef f, CFFileDescriptorContext *context)
{
	if( (f == NULL) || (CFGetTypeID(f) != CFFileDescriptorGetTypeID()) || (context == NULL) || (context->version != 0) || !__CFFDIsValid(f) )
		return;

	context->info = f->context.info;
	context->retain = f->context.retain;
	context->release = f->context.release;
	context->copyDescription = f->context.copyDescription;
}

// enable callbacks, setting kqueue filter, regardless of whether watcher thread is running
void CFFileDescriptorEnableCallBacks(CFFileDescriptorRef f, CFOptionFlags callBackTypes)
{
	//fprintf(stderr, "Entering CFFileDescriptorEnableCallBacks() with flags = %d\n", callBackTypes);

	if( (f == NULL) || (CFGetTypeID(f) != CFFileDescriptorGetTypeID()) || !__CFFDIsValid(f) ) return;

	__CFSpinLock(&f->lock);

#if DEPLOYMENT_TARGET_MACOSX
   struct kevent ev;
	struct timespec ts = { 0, 0 };

	if( callBackTypes | kCFFileDescriptorReadCallBack )
	{
		EV_SET(&ev, f->fd, EVFILT_READ, EV_ADD|EV_ONESHOT, 0, 0, 0);
		kevent(f->qd, &ev, 1, NULL, 0, &ts);
	}
	
	if( callBackTypes | kCFFileDescriptorWriteCallBack )
	{
		EV_SET(&ev, f->fd, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0, 0, 0);
		kevent(f->qd, &ev, 1, NULL, 0, &ts);
	}
#endif

	__CFSpinUnlock(&f->lock);
}

// disable callbacks, setting kqueue filter, regardless of whether watcher thread is running
void CFFileDescriptorDisableCallBacks(CFFileDescriptorRef f, CFOptionFlags callBackTypes)
{
	if( (f == NULL) || (CFGetTypeID(f) != CFFileDescriptorGetTypeID()) || !__CFFDIsValid(f) ) return;
	
	__CFSpinLock(&f->lock);

#if DEPLOYMENT_TARGET_MACOSX
   struct kevent ev;
	struct timespec ts = { 0, 0 };
	
	if( callBackTypes | kCFFileDescriptorReadCallBack )
	{
		EV_SET(&ev, f->fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
		kevent(f->qd, &ev, 1, NULL, 0, &ts);
	}
	
	if( callBackTypes | kCFFileDescriptorWriteCallBack )
	{
		EV_SET(&ev, f->fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
		kevent(f->qd, &ev, 1, NULL, 0, &ts);
	}
#endif

	__CFSpinUnlock(&f->lock);
}

// invalidate the file descriptor, possibly closing the fd
void CFFileDescriptorInvalidate(CFFileDescriptorRef f)
{
	if( (f == NULL) || (CFGetTypeID(f) != CFFileDescriptorGetTypeID()) || !__CFFDIsValid(f) ) return;
	
	__CFSpinLock(&f->lock);

	__CFBitfieldSetValue(((CFRuntimeBase *)f)->_cfinfo[CF_INFO_BITS], 0, 0, 0); // invalidate flag

#if DEPLOYMENT_TARGET_MACOSX
	if( f->thread != NULL ) // assume there is a thread and a mach port
	{
		pthread_cancel(f->thread);
		mach_port_destroy(mach_task_self(), f->port);
		
		f->thread = NULL;
		f->port = MACH_PORT_NULL;
	}
#endif

	if( f->rls != NULL )
	{
		CFRelease(f->rls);
		f->rls = NULL;
	}
	
#if DEPLOYMENT_TARGET_MACOSX
	close(f->qd);
	f->qd = -1;
#endif
	
	if( __CFBitfieldGetValue(((const CFRuntimeBase *)f)->_cfinfo[CF_INFO_BITS], 1, 1) ) // close fd on invalidate
		close(f->fd);
	
	__CFSpinUnlock(&f->lock);
}

// is file descriptor still valid, based on _base header flags?
Boolean	CFFileDescriptorIsValid(CFFileDescriptorRef f)
{
	if( (f == NULL) || (CFGetTypeID(f) != CFFileDescriptorGetTypeID()) ) return FALSE;
    return __CFFDIsValid(f);
}

CFRunLoopSourceRef CFFileDescriptorCreateRunLoopSource(CFAllocatorRef allocator, CFFileDescriptorRef f, CFIndex order)
{
	//fprintf(stderr,"Entering CFFileDescriptorCreateRunLoopSource()\n");

	if( (f == NULL) || (CFGetTypeID(f) != CFFileDescriptorGetTypeID()) || !__CFFDIsValid(f) ) return NULL;

	__CFSpinLock(&f->lock);
	if( f->rls == NULL )
	{
#if DEPLOYMENT_TARGET_MACOSX
		CFRunLoopSourceContext1 context = { 1, CFRetain(f), (CFAllocatorRetainCallBack)f->context.retain, (CFAllocatorReleaseCallBack)f->context.release, (CFAllocatorCopyDescriptionCallBack)f->context.copyDescription, NULL, NULL, __CFFDGetPort, __CFFDRunLoopCallBack };
		CFRunLoopSourceRef rls = CFRunLoopSourceCreate( allocator, order, (CFRunLoopSourceContext *)&context );
		if( rls != NULL ) f->rls = rls;
#endif
   }
	__CFSpinUnlock(&f->lock);
	//fprintf(stderr,"Leaving CFFileDescriptorCreateRunLoopSource()\n");

	return f->rls;
}
