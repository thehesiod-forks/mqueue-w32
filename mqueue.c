/*****************************************************************************
 *
 * POSIX Message Queue for Windows NT
 *
 *****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h> // PATH_MAX

#if defined(WIN32) || defined(__APPLE__)
#include <string.h> // strlen
#endif

#if defined(WIN32)
    #include <io.h> // _lseek, etc
#else
    #include <sys/mman.h>
    #include <unistd.h>  // lseek, etc
    #include <signal.h> // kill
#endif

#include "mqueue.h"

#if defined(WIN32)
#   define S_IXUSR  0000100
#   define sleep(a) Sleep((a)*1000)

    typedef unsigned short mode_t;
#endif

//#define POSIX_IPC_DEBUG
#ifdef POSIX_IPC_DEBUG
#define DPRINTF(fmt, args...) fprintf(stderr, "+++ " fmt, ## args)
#else
#define DPRINTF(fmt, args...)
#endif

#ifdef WIN32
/* TODO: these should be setting errno appropriately like the pthread methods */
int MUTEX_LOCK(struct mq_hdr* hdr)
{
	DWORD dwWaitResult = WaitForSingleObject( 
		hdr->mqh_lock,    // handle to mutex
		INFINITE);  // no time-out interval
 
    switch (dwWaitResult) 
    {
        // The thread got ownership of the mutex
        case WAIT_OBJECT_0: 
			return 0;

        // The thread got ownership of an abandoned mutex
        // The database is in an indeterminate state
        case WAIT_ABANDONED: 
            return ECANCELED ; 
		default:
			return EINVAL;
    }
}

/* Returns 0 on success */
int COND_WAIT(struct mq_hdr* hdr)
{
	DWORD dwWaitResult;

	ReleaseMutex(hdr->mqh_lock);
	dwWaitResult = WaitForSingleObject(hdr->mqh_wait, INFINITE);
	if(dwWaitResult != WAIT_OBJECT_0) {
		return EINVAL;
	}

	return MUTEX_LOCK(hdr);
}

/* Returns 0 on success */
int COND_TIMED_WAIT(struct mq_hdr* hdr, const struct timespec* abstime)
{
	DWORD dwWaitResult;

	ReleaseMutex(hdr->mqh_lock);
	dwWaitResult = WaitForSingleObject(hdr->mqh_wait, (DWORD) abstime->tv_sec);
	switch(dwWaitResult) {
	    case WAIT_OBJECT_0:
	        break
	    case WAIT_TIMEOUT:
	        return ETIMEDOUT;
	    default:
	        return EINVAL;
	}

	dwWaitResult = WaitForSingleObject(hdr->mqh_lock,(DWORD) abstime->tv_sec);
	if(dwWaitResult == WAIT_OBJECT_0) {
	    return 0;
	}

    if(dwWaitResult == WAIT_TIMEOUT) {
        return ETIMEDOUT;
    }

    return EINVAL;
}

/* Returns 0 on success */
int COND_SIGNAL(struct mq_hdr* hdr)
{
	BOOL result = SetEvent(hdr->mqh_wait);
	return result == 0;
}
void MUTEX_UNLOCK(struct mq_hdr* hdr)
{
	ReleaseMutex(hdr->mqh_lock);
}

#define open _open
#define close _close
#define unlink _unlink
#define lseek _lseek
#define write _write
#else
#define INFINITE -1
#define MUTEX_LOCK(x)   pthread_mutex_lock(&x->mqh_lock)
#define MUTEX_UNLOCK(x) pthread_mutex_unlock(&x->mqh_lock)
#define COND_WAIT(x)  pthread_cond_wait(&x->mqh_wait, &x->mqh_lock)
#define COND_TIMED_WAIT(x, y) pthread_cond_timedwait(&x->mqh_wait, &x->mqh_lock, y)
#define COND_SIGNAL(x)  pthread_cond_signal(&x->mqh_wait)
#endif


#define MAX_TRIES   10
struct mq_attr defattr = { 0, 128, 1024, 0 };


int mq_close(mqd_t mqd)
{
    long            msgsize, filesize;
    struct mq_hdr  *mqhdr;
    struct mq_attr *attr;
    struct mq_info *mqinfo;

    mqinfo = mqd;
    if (mqinfo->mqi_magic != MQI_MAGIC) {
        errno = EBADF;
        return(-1);
    }
    mqhdr = mqinfo->mqi_hdr;
    attr = &mqhdr->mqh_attr;

    if (mq_notify(mqd, NULL) != 0)        /* unregister calling process */
        return(-1);

    msgsize = MSGSIZE(attr->mq_msgsize);
    filesize = sizeof(struct mq_hdr) + (attr->mq_maxmsg *
                      (sizeof(struct msg_hdr) + msgsize));
#if defined(WIN32)
    if (!UnmapViewOfFile(mqinfo->mqi_hdr) || !CloseHandle(mqinfo->mqi_fmap))
#else
    if (munmap(mqinfo->mqi_hdr, filesize) == -1)
#endif
        return(-1);

    mqinfo->mqi_magic = 0;          /* just in case */
    free(mqinfo);
    return(0);
}

int mq_getattr(mqd_t mqd, struct mq_attr *mqstat)
{
    int             n;
    struct mq_hdr  *mqhdr;
    struct mq_attr *attr;
    struct mq_info *mqinfo;

    mqinfo = mqd;
    if (mqinfo->mqi_magic != MQI_MAGIC) {
        errno = EBADF;
        return(-1);
    }
    mqhdr = mqinfo->mqi_hdr;
    attr = &mqhdr->mqh_attr;

    if ( (n = MUTEX_LOCK(mqhdr)) != 0) {
		errno = n;
        return(-1);
    }

    mqstat->mq_flags = mqinfo->mqi_flags;   /* per-open */
    mqstat->mq_maxmsg = attr->mq_maxmsg;    /* remaining three per-queue */
    mqstat->mq_msgsize = attr->mq_msgsize;
    mqstat->mq_curmsgs = attr->mq_curmsgs;

    MUTEX_UNLOCK(mqhdr);
    return(0);
}

int mq_notify(mqd_t mqd, const struct sigevent *notification)
{
#if !defined(WIN32)
    int             n;
    pid_t           pid;
    struct mq_hdr  *mqhdr;
    struct mq_info *mqinfo;

    mqinfo = mqd;
    if (mqinfo->mqi_magic != MQI_MAGIC) {
        errno = EBADF;
        return(-1);
    }
    mqhdr = mqinfo->mqi_hdr;
    if ( (n = MUTEX_LOCK(mqhdr)) != 0) {
        errno = n;
        return(-1);
    }

    pid = getpid();
    if (notification == NULL) {
        if (mqhdr->mqh_pid == pid) {
            mqhdr->mqh_pid = 0;     /* unregister calling process */
        }                           /* no error if c aller not registered */
    } else {
        if (mqhdr->mqh_pid != 0) {
            if (kill(mqhdr->mqh_pid, 0) != -1 || errno != ESRCH) {
                errno = EBUSY;
                goto err;
            }
        }

        if(notification->sigev_notify == SIGEV_THREAD) {
            errno = ENOTSUP;
            goto err;
        }

        mqhdr->mqh_pid = pid;
        mqhdr->mqh_event = *notification;
    }
    MUTEX_UNLOCK(mqhdr);
    return(0);

err:
    MUTEX_UNLOCK(mqhdr);
    return(-1);
#else
    errno = ENOSYS;
    return(-1);
#endif
}

/* These end with a slash */
#ifdef WIN32
#define TMP_ENV_NAME "TEMP"
#else
#define TMP_ENV_NAME "TMPDIR"
#endif

#if defined(WIN32) || defined(__APPLE__)
int get_temp_path(char* pathBuffer, int pathBufferSize, const char* pathPart) {
	const char* temp = getenv(TMP_ENV_NAME);
	if(strlen(temp) >= pathBufferSize - strlen(pathPart)) {
		return 0;
	}

	/* We've done the size check above so we don't need to use the string safe methods */
	strcpy(pathBuffer, temp);
	strcat(pathBuffer, pathPart);
	return 1;
}
#endif

mqd_t mq_open(const char *pathname1, int oflag, ...)
{
    int                  i, fd, nonblock, created, save_errno;
    long                 msgsize, filesize, index;
    va_list              ap;
    mode_t               mode;
    char                *mptr;
    struct stat          statbuff;
    struct mq_hdr       *mqhdr;
    struct msg_hdr      *msghdr;
    struct mq_attr      *attr =  NULL;
    struct mq_info      *mqinfo;

#if  defined(WIN32) || defined(__APPLE__)
	char pathBuffer[PATH_MAX];
#else
	const char* pathBuffer;
#endif

#ifndef WIN32
    pthread_mutexattr_t  mattr;
    pthread_condattr_t   cattr;
#endif

#ifdef WIN32
    HANDLE fmap = NULL;

    mptr = NULL;
#else
    mptr = (char *) MAP_FAILED;
#endif

    created = 0;
    nonblock = oflag & O_NONBLOCK;
    oflag &= ~O_NONBLOCK;
    mqinfo = NULL;

#if defined(WIN32) || defined(__APPLE__)
	{
		if(!get_temp_path(pathBuffer, sizeof(pathBuffer), pathname1)) {
			return((mqd_t) -1);
		}
	}
#else
	pathBuffer = (char*)pathname1;
#endif

again:
    if (oflag & O_CREAT) {
        va_start(ap, oflag); /* init ap to final named argument */
        mode = va_arg(ap, mode_t) & ~S_IXUSR;
        attr = va_arg(ap, struct mq_attr *);
        va_end(ap);

        /* open and specify O_EXCL and user-execute */
		oflag &= ~3; // strip off RDONLY, WRONLY or RDWR bits
        fd = open(pathBuffer, oflag | O_EXCL | O_RDWR, mode | S_IXUSR);
        if (fd < 0) {
            if (errno == EEXIST && (oflag & O_EXCL) == 0)
                goto exists;            /* already exists, OK */
            else
                return((mqd_t) -1);
            }
            created = 1;
                        /* first one to create the file initializes it */
            if (attr == NULL)
                attr = &defattr;
            else {
                if (attr->mq_maxmsg <= 0 || attr->mq_msgsize <= 0) {
                    errno = EINVAL;
                    goto err;
                }
            }
            /* calculate and set the file size */
            msgsize = MSGSIZE(attr->mq_msgsize);
            filesize = sizeof(struct mq_hdr) + (attr->mq_maxmsg *
                               (sizeof(struct msg_hdr) + msgsize));
            if (lseek(fd, filesize - 1, SEEK_SET) == -1)
                goto err;
            if (write(fd, "", 1) == -1)
                goto err;

            /* memory map the file */
#if defined(WIN32)
            fmap = CreateFileMapping((HANDLE)_get_osfhandle(fd), NULL, 
                                     PAGE_READWRITE, 0, 0, NULL);
            if (fmap == NULL)
                goto err;
            mptr = (char*)MapViewOfFile(fmap, FILE_MAP_WRITE, 0, 0, filesize);
            if (mptr == NULL)
#else
            mptr = mmap(NULL, filesize, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0);
            if (mptr == MAP_FAILED)
#endif
                goto err;

            /* allocate one mq_info{} for the queue */
            if ( (mqinfo = malloc(sizeof(struct mq_info))) == NULL)
                goto err;
#if defined(WIN32)
            mqinfo->mqi_fmap = fmap;
#endif
            mqinfo->mqi_hdr = mqhdr = (struct mq_hdr *) mptr;
            mqinfo->mqi_magic = MQI_MAGIC;
            mqinfo->mqi_flags = nonblock;

            /* initialize header at beginning of file */
            /* create free list with all messages on it */
            mqhdr->mqh_attr.mq_flags = 0;
            mqhdr->mqh_attr.mq_maxmsg = attr->mq_maxmsg;
            mqhdr->mqh_attr.mq_msgsize = attr->mq_msgsize;
            mqhdr->mqh_attr.mq_curmsgs = 0;
            mqhdr->mqh_nwait = 0;
            mqhdr->mqh_pid = 0;
            mqhdr->mqh_head = 0;
            index = sizeof(struct mq_hdr);
            mqhdr->mqh_free = index;
            for (i = 0; i < attr->mq_maxmsg - 1; i++) {
                msghdr = (struct msg_hdr *) &mptr[index];
                index += sizeof(struct msg_hdr) + msgsize;
                msghdr->msg_next = index;
            }
            msghdr = (struct msg_hdr *) &mptr[index];
            msghdr->msg_next = 0;           /* end of free list */

            /* initialize mutex & condition variable */
#if defined(WIN32)
			{

				wchar_t nameBuffer[128];
				size_t len;

				len = strlen(pathname1);
				if (len >= 64) {
					errno = EINVAL;
					goto err;
				}
				lstrcpyW(nameBuffer,TEXT("Global\\Mqueue_Mutex_"));
				len = lstrlenW(nameBuffer);
				mbstowcs(nameBuffer+len, pathname1+1, 128-len);
				mqhdr->mqh_lock = CreateMutex( 
					NULL,              // default security attributes
					FALSE,             // initially not owned
					nameBuffer);       // named mutex

				lstrcpyW(nameBuffer,TEXT("Global\\Mqueue_Event_"));
				len = lstrlenW(nameBuffer);
				mbstowcs(nameBuffer+len, pathname1+1, 128-len);
				mqhdr->mqh_wait = CreateEvent(
					NULL, 
					FALSE, 
					FALSE,
					nameBuffer
					);
			}
#else
            if ( (i = pthread_mutexattr_init(&mattr)) != 0)
                goto pthreaderr;
            i = pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
            if (i != 0)
                goto pthreaderr;
            i = pthread_mutex_init(&mqhdr->mqh_lock, &mattr);
            pthread_mutexattr_destroy(&mattr);      /* be sure to destroy */
            if (i != 0)
                goto pthreaderr;

            if ( (i = pthread_condattr_init(&cattr)) != 0)
                goto pthreaderr;
            pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
            i = pthread_cond_init(&mqhdr->mqh_wait, &cattr);
            pthread_condattr_destroy(&cattr);       /* be sure to destroy */
            if (i != 0)
                goto pthreaderr;
#endif


            /* initialization complete, turn off user-execute bit */
#if defined(WIN32)
            if (_chmod(pathBuffer, mode) == -1)
#else
            if (fchmod(fd, mode) == -1)
#endif
                goto err;
            close(fd);
            return((mqd_t) mqinfo);
    }
exists:
    /* open the file then memory map */
    if ( (fd = open(pathBuffer, O_RDWR)) < 0) {
        if (errno == ENOENT && (oflag & O_CREAT))
            goto again;
        goto err;
    }

    /* make certain initialization is complete */
    for (i = 0; i < MAX_TRIES; i++) {
        if (stat(pathBuffer, &statbuff) == -1) {
            if (errno == ENOENT && (oflag & O_CREAT)) {
                close(fd);
                goto again;
            }
            goto err;
        }
        if ((statbuff.st_mode & S_IXUSR) == 0)
            break;
        sleep(1);
    }
    if (i == MAX_TRIES) {
        errno = ETIMEDOUT;
        goto err;
    }

    filesize = statbuff.st_size;
#if defined(WIN32)
    fmap = CreateFileMapping((HANDLE)_get_osfhandle(fd), NULL, PAGE_READWRITE, 
                             0, 0, NULL);                             
    if (fmap == NULL)
        goto err;
    mptr = (char*)MapViewOfFile(fmap, FILE_MAP_WRITE, 0, 0, filesize);
    if (mptr == NULL)
#else
    mptr = mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mptr == MAP_FAILED)
#endif
        goto err;
    close(fd);

    /* allocate one mq_info{} for each open */
    if ( (mqinfo = malloc(sizeof(struct mq_info))) == NULL)
        goto err;
    mqinfo->mqi_hdr = (struct mq_hdr *) mptr;
    mqinfo->mqi_magic = MQI_MAGIC;
    mqinfo->mqi_flags = nonblock;
    return((mqd_t) mqinfo);
#if !defined(WIN32)
pthreaderr:
    errno = i;
#endif
err:
    /* don't let following function calls change errno */
    save_errno = errno;
    if (created)
        unlink(pathBuffer);
#if defined(WIN32)
    if (fmap != NULL) {
        if (mptr != NULL) {
            UnmapViewOfFile(mptr);
        }
        CloseHandle(fmap);
    }
#else
    if (mptr != MAP_FAILED)
        munmap(mptr, filesize);
#endif
    if (mqinfo != NULL)
        free(mqinfo);
	if (fd !=-1)
		close(fd);
    errno = save_errno;
    return((mqd_t) -1);
}

ssize_t  mq_timedreceive(mqd_t mqd, char *ptr,
                         size_t maxlen, unsigned int *priop,
                         const struct timespec *abs_timeout)
{


    int             n;
    long            index;
    char           *mptr;
    ssize_t         len;
    struct mq_hdr  *mqhdr;
    struct mq_attr *attr;
    struct msg_hdr *msghdr;
    struct mq_info *mqinfo;

    mqinfo = mqd;
    if (mqinfo->mqi_magic != MQI_MAGIC) {
        errno = EBADF;
        return(-1);
    }
    mqhdr = mqinfo->mqi_hdr;        /* struct pointer */
    mptr = (char *) mqhdr;          /* byte pointer */
    attr = &mqhdr->mqh_attr;
    if ( (n = MUTEX_LOCK(mqhdr)) != 0) {
        errno = n;
        return(-1);
    }

    if (maxlen < (size_t)attr->mq_msgsize) {
        errno = EMSGSIZE;
        goto err;
    }

    if (attr->mq_curmsgs == 0) {            /* queue is empty */
        if (mqinfo->mqi_flags & O_NONBLOCK) {
            errno = EAGAIN;
            goto err;
        }

        /* wait for a message to be placed onto queue */
        mqhdr->mqh_nwait++;
        while (attr->mq_curmsgs == 0)
		{
			if (abs_timeout->tv_sec == INFINITE) {
				n = COND_WAIT(mqhdr);
			}
			else {
			    n = COND_TIMED_WAIT(mqhdr, abs_timeout);
			}

            if(n != 0) {
                errno = n;
                mqhdr->mqh_nwait--;
                goto err;
            }
		}

        mqhdr->mqh_nwait--;
    }

    if ((index = mqhdr->mqh_head) == 0) {
        DPRINTF("mq_receive: curmsgs = %ld; head = 0\n", attr->mq_curmsgs);
        abort();
    }

    msghdr = (struct msg_hdr *) &mptr[index];
    mqhdr->mqh_head = msghdr->msg_next;     /* new head of list */
    len = msghdr->msg_len;
    memcpy(ptr, msghdr + 1, len);           /* copy the message itself */
    if (priop != NULL)
        *priop = msghdr->msg_prio;

    /* just-read message goes to front of free list */
    msghdr->msg_next = mqhdr->mqh_free;
    mqhdr->mqh_free = index;

    /* wake up anyone blocked in mq_send waiting for room */
    if (attr->mq_curmsgs == attr->mq_maxmsg)
        COND_SIGNAL(mqhdr);
    attr->mq_curmsgs--;

    MUTEX_UNLOCK(mqhdr);
    return(len);

err:
    MUTEX_UNLOCK(mqhdr);
    return(-1);
}

ssize_t mq_receive(mqd_t mqd, char *ptr, size_t maxlen, unsigned int *priop)
{
	struct timespec abstime;
	abstime.tv_sec = INFINITE;
	abstime.tv_nsec = INFINITE;
	return mq_timedreceive(mqd,ptr,maxlen,priop,&abstime);
}


int mq_timedsend(mqd_t mqd, const char *ptr, size_t len, unsigned int prio,
const struct timespec *abs_timeout)
{
    int              n;
    long             index, freeindex;
    char            *mptr;
    struct sigevent *sigev;
    struct mq_hdr   *mqhdr;
    struct mq_attr  *attr;
    struct msg_hdr  *msghdr, *nmsghdr, *pmsghdr;
    struct mq_info  *mqinfo;

    mqinfo = mqd;
    if (mqinfo->mqi_magic != MQI_MAGIC) {
        errno = EBADF;
        return(-1);
    }
    mqhdr = mqinfo->mqi_hdr;        /* struct pointer */
    mptr = (char *) mqhdr;          /* byte pointer */
    attr = &mqhdr->mqh_attr;
    if ( (n = MUTEX_LOCK(mqhdr)) != 0) {
        errno = n;
        return(-1);
    }

    if (len > (size_t)attr->mq_msgsize) {
        errno = EMSGSIZE;
        goto err;
    }
    if (attr->mq_curmsgs == 0) {
        if (mqhdr->mqh_pid != 0 && mqhdr->mqh_nwait == 0) {
            sigev = &mqhdr->mqh_event;
#if !defined(WIN32)
            if (sigev->sigev_notify == SIGEV_SIGNAL) {
#ifdef __APPLE__
                kill(mqhdr->mqh_pid, sigev->sigev_signo);
#else
                sigqueue(mqhdr->mqh_pid, sigev->sigev_signo,
                                         sigev->sigev_value);
#endif
            }
#endif
            mqhdr->mqh_pid = 0;             /* unregister */
        }
    } else if (attr->mq_curmsgs >= attr->mq_maxmsg) {
        /* queue is full */
        if (mqinfo->mqi_flags & O_NONBLOCK) {
            errno = EAGAIN;
            goto err;
        }

        /* wait for room for one message on the queue */
        while (attr->mq_curmsgs >= attr->mq_maxmsg) {
			if (abs_timeout->tv_sec == INFINITE) {
				n = COND_WAIT(mqhdr);
			}
			else {
			    n = COND_TIMED_WAIT(mqhdr, abs_timeout);
			}

            DPRINTF("n: %d\n", n);
            if (n != 0) {
                errno = n;
                goto err;
            }
		}
    }

    /* nmsghdr will point to new message */
    if ( (freeindex = mqhdr->mqh_free) == 0) {
        DPRINTF("mq_send: curmsgs = %ld; free = 0\n", attr->mq_curmsgs);
    }

    nmsghdr = (struct msg_hdr *) &mptr[freeindex];
    nmsghdr->msg_prio = prio;
    nmsghdr->msg_len = (ssize_t) len;
    memcpy(nmsghdr + 1, ptr, len);          /* copy message from caller */
    mqhdr->mqh_free = nmsghdr->msg_next;    /* new freelist head */

    /* find right place for message in linked list */
    index = mqhdr->mqh_head;
    pmsghdr = (struct msg_hdr *) &(mqhdr->mqh_head);
    while (index != 0) {
        msghdr = (struct msg_hdr *) &mptr[index];
        if (prio > msghdr->msg_prio) {
            nmsghdr->msg_next = index;
            pmsghdr->msg_next = freeindex;
            break;
        }
        index = msghdr->msg_next;
        pmsghdr = msghdr;
    }
    if (index == 0) {
        /* queue was empty or new goes at end of list */
        pmsghdr->msg_next = freeindex;
        nmsghdr->msg_next = 0;
    }
    /* wake up anyone blocked in mq_receive waiting for a message */ 
    if (attr->mq_curmsgs == 0)
        COND_SIGNAL(mqhdr);
    attr->mq_curmsgs++;

    MUTEX_UNLOCK(mqhdr);
    return(0);

err:
    MUTEX_UNLOCK(mqhdr);
    return(-1);
}


int mq_send(mqd_t mqd, const char *ptr, size_t len, unsigned int prio)
{
	struct timespec abstime;
	abstime.tv_sec = INFINITE;
	abstime.tv_nsec = INFINITE;
    return mq_timedsend(mqd, ptr, len, prio, &abstime);
}

int mq_setattr(mqd_t mqd, const struct mq_attr *mqstat, struct mq_attr *omqstat)
{
    int             n;
    struct mq_hdr  *mqhdr;
    struct mq_attr *attr;
    struct mq_info *mqinfo;

    mqinfo = mqd;
    if (mqinfo->mqi_magic != MQI_MAGIC) {
        errno = EBADF;
        return(-1);
    }
    mqhdr = mqinfo->mqi_hdr;
    attr = &mqhdr->mqh_attr;
    if ( (n = MUTEX_LOCK(mqhdr)) != 0) {
        errno = n;
        return(-1);
    }

    if (omqstat != NULL) {
        omqstat->mq_flags = mqinfo->mqi_flags;  /* previous attributes */
        omqstat->mq_maxmsg = attr->mq_maxmsg;
        omqstat->mq_msgsize = attr->mq_msgsize;
        omqstat->mq_curmsgs = attr->mq_curmsgs; /* and current status */
    }

    if (mqstat->mq_flags & O_NONBLOCK)
        mqinfo->mqi_flags |= O_NONBLOCK;
    else
        mqinfo->mqi_flags &= ~O_NONBLOCK;

    MUTEX_UNLOCK(mqhdr);
    return(0);
}

int mq_unlink(const char *pathname1)
{
#if defined(WIN32) || defined(__APPLE__)
	char pathname[PATH_MAX];

	if(!get_temp_path(pathname, sizeof(pathname), pathname1)) {
		return(-1);
	}
#else
	const char* pathname = pathname1;
#endif

    if (unlink(pathname) == -1)
        return(-1);

    return(0);
}
