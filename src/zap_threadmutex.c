/* 
 * Cross Platform Thread/Mutex abstraction
 * Copyright(C) 2007 Michael Jerris
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so.
 *
 * This work is provided under this license on an "as is" basis, without warranty of any kind,
 * either expressed or implied, including, without limitation, warranties that the covered code
 * is free of defects, merchantable, fit for a particular purpose or non-infringing. The entire
 * risk as to the quality and performance of the covered code is with you. Should any covered
 * code prove defective in any respect, you (not the initial developer or any other contributor)
 * assume the cost of any necessary servicing, repair or correction. This disclaimer of warranty
 * constitutes an essential part of this license. No use of any covered code is authorized hereunder
 * except under this disclaimer. 
 *
 */

#ifdef WIN32
/* required for TryEnterCriticalSection definition.  Must be defined before windows.h include */
#define _WIN32_WINNT 0x0400
#endif

#include "openzap.h"
#include "zap_threadmutex.h"

#ifdef WIN32
#include <process.h>

#define ZAP_THREAD_CALLING_CONVENTION __stdcall

struct zap_mutex {
	CRITICAL_SECTION mutex;
};

#else

#include <pthread.h>

#define ZAP_THREAD_CALLING_CONVENTION

struct zap_mutex {
	pthread_mutex_t mutex;
};

#endif

struct zap_thread {
#ifdef WIN32
	void *handle;
#else
	pthread_t handle;
#endif
	void *private_data;
	zap_thread_function_t function;
	zap_size_t stack_size;
#ifndef WIN32
	pthread_attr_t attribute;
#endif
};

zap_size_t thread_default_stacksize = 0;

void zap_thread_override_default_stacksize(zap_size_t size)
{
	thread_default_stacksize = size;
}

static void * ZAP_THREAD_CALLING_CONVENTION thread_launch(void *args)
{
	void *exit_val;
    zap_thread_t *thread = (zap_thread_t *)args;
	exit_val = thread->function(thread, thread->private_data);
#ifndef WIN32
	pthread_attr_destroy(&thread->attribute);
#endif
	free(thread);

	return exit_val;
}

zap_status_t zap_thread_create_detached(zap_thread_function_t func, void *data)
{
	return zap_thread_create_detached_ex(func, data, thread_default_stacksize);
}

zap_status_t zap_thread_create_detached_ex(zap_thread_function_t func, void *data, zap_size_t stack_size)
{
	zap_thread_t *thread = NULL;
	zap_status_t status = ZAP_FAIL;

	if (!func || !(thread = (zap_thread_t *)malloc(sizeof(zap_thread_t)))) {
		goto done;
	}

	thread->private_data = data;
	thread->function = func;
	thread->stack_size = stack_size;

#if defined(WIN32)
	thread->handle = (void *)_beginthreadex(NULL, (unsigned)thread->stack_size, (unsigned int (__stdcall *)(void *))thread_launch, thread, 0, NULL);
	if (!thread->handle) {
		goto fail;
	}
	CloseHandle(thread->handle);

	status = ZAP_SUCCESS;
	goto done;
#else
	
	if (pthread_attr_init(&thread->attribute) != 0)	goto fail;

	if (pthread_attr_setdetachstate(&thread->attribute, PTHREAD_CREATE_DETACHED) != 0) goto failpthread;

	if (thread->stack_size && pthread_attr_setstacksize(&thread->attribute, thread->stack_size) != 0) goto failpthread;

	if (pthread_create(&thread->handle, &thread->attribute, thread_launch, thread) != 0) goto failpthread;

	status = ZAP_SUCCESS;
	goto done;
failpthread:
	pthread_attr_destroy(&thread->attribute);
#endif

fail:
	if (thread) {
		free(thread);
	}
done:
	return status;
}


zap_status_t zap_mutex_create(zap_mutex_t **mutex)
{
	zap_status_t status = ZAP_FAIL;
#ifndef WIN32
	pthread_mutexattr_t attr;
#endif
	zap_mutex_t *check = NULL;

	check = (zap_mutex_t *)malloc(sizeof(**mutex));
	if (!check)
		goto done;
#ifdef WIN32
	InitializeCriticalSection(&check->mutex);
#else
	if (pthread_mutexattr_init(&attr))
		goto done;

	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE))
		goto fail;

	if (pthread_mutex_init(&check->mutex, &attr))
		goto fail;

	goto success;

fail:
        pthread_mutexattr_destroy(&attr);
		goto done;

success:
#endif
	*mutex = check;
	status = ZAP_SUCCESS;

done:
	return status;
}

zap_status_t zap_mutex_destroy(zap_mutex_t **mutex)
{
	zap_mutex_t *mp = *mutex;
	*mutex = NULL;
#ifdef WIN32
	DeleteCriticalSection(&mp->mutex);
#else
	if (pthread_mutex_destroy(&mp->mutex))
		return ZAP_FAIL;
#endif
	free(mp);
	return ZAP_SUCCESS;
}

zap_status_t zap_mutex_lock(zap_mutex_t *mutex)
{
#ifdef WIN32
	EnterCriticalSection(&mutex->mutex);
#else
	if (pthread_mutex_lock(&mutex->mutex))
		return ZAP_FAIL;
#endif
	return ZAP_SUCCESS;
}

zap_status_t zap_mutex_trylock(zap_mutex_t *mutex)
{
#ifdef WIN32
	if (!TryEnterCriticalSection(&mutex->mutex))
		return ZAP_FAIL;
#else
	if (pthread_mutex_trylock(&mutex->mutex))
		return ZAP_FAIL;
#endif
	return ZAP_SUCCESS;
}

zap_status_t zap_mutex_unlock(zap_mutex_t *mutex)
{
#ifdef WIN32
	LeaveCriticalSection(&mutex->mutex);
#else
	if (pthread_mutex_unlock(&mutex->mutex))
		return ZAP_FAIL;
#endif
	return ZAP_SUCCESS;
}