#ifndef _LINUX_SEM_H
#define _LINUX_SEM_H

#include <linux/ipc.h>

/* semop flags */
#define SEM_UNDO        0x1000  /* undo the operation on exit */

/* semctl Command Definitions. */
#define GETPID  11       /* get sempid */
#define GETVAL  12       /* get semval */
#define GETALL  13       /* get all semval's */
#define GETNCNT 14       /* get semncnt */
#define GETZCNT 15       /* get semzcnt */
#define SETVAL  16       /* set semval */
#define SETALL  17       /* set all semval's */

/* ipcs ctl cmds */
#define SEM_STAT 18
#define SEM_INFO 19

/* Obsolete, used only for backwards compatibility and libc5 compiles */
struct semid_ds {
	struct ipc_perm	sem_perm;		/* permissions .. see ipc.h */
	__kernel_time_t	sem_otime;		/* last semop time */
	__kernel_time_t	sem_ctime;		/* last change time */
	struct sem	*sem_base;		/* ptr to first semaphore in array */
	struct sem_queue *sem_pending;		/* pending operations to be processed */
	struct sem_queue **sem_pending_last;	/* last pending operation */
	struct sem_undo	*undo;			/* undo requests on this array */
	unsigned short	sem_nsems;		/* no. of semaphores in array */
};

/* Include the definition of semid64_ds */
#include <asm/sembuf.h>

/* semop system calls takes an array of these. */
struct sembuf {
	unsigned short  sem_num;	/* semaphore index in array */
	short		sem_op;		/* semaphore operation */
	short		sem_flg;	/* operation flags */
};

/* arg for semctl system calls. */
union semun {
	int val;			/* value for SETVAL */
	struct semid_ds __user *buf;	/* buffer for IPC_STAT & IPC_SET */
	unsigned short __user *array;	/* array for GETALL & SETALL */
	struct seminfo __user *__buf;	/* buffer for IPC_INFO */
	void __user *__pad;
};

struct  seminfo {
	int semmap;
	int semmni;
	int semmns;
	int semmnu;
	int semmsl;
	int semopm;
	int semume;
	int semusz;
	int semvmx;
	int semaem;
};

#define SEMMNI  128             /* <= IPCMNI  max # of semaphore identifiers */
#define SEMMSL  250             /* <= 8 000 max num of semaphores per id */
#define SEMMNS  (SEMMNI*SEMMSL) /* <= INT_MAX max # of semaphores in system */
#define SEMOPM  32	        /* <= 1 000 max num of ops per semop call */
#define SEMVMX  32767           /* <= 32767 semaphore maximum value */
#define SEMAEM  SEMVMX          /* adjust on exit max value */

/* unused */
#define SEMUME  SEMOPM          /* max num of undo entries per process */
#define SEMMNU  SEMMNS          /* num of undo structures system wide */
#define SEMMAP  SEMMNS          /* # of entries in semaphore map */
#define SEMUSZ  20		/* sizeof struct sem_undo */

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/rcupdate.h>
#include <linux/cache.h>

struct task_struct;

/* One sem_array data structure for each set of semaphores in the system. */
struct sem_array {
	struct kern_ipc_perm	____cacheline_aligned_in_smp
				sem_perm;	/* permissions .. see ipc.h */
	time_t			sem_otime;	/* last semop time */
	time_t			sem_ctime;	/* last change time */
	struct sem		*sem_base;	/* ptr to first semaphore in array */
	struct list_head	sem_pending;	/* pending operations to be processed */
	struct list_head	list_id;	/* undo requests on this array */
	int			sem_nsems;	/* no. of semaphores in array */
	int			complex_count;	/* pending complex operations */
};

#ifdef CONFIG_SYSVIPC

struct sysv_sem {
	struct sem_undo_list *undo_list;
};

extern int copy_semundo(unsigned long clone_flags, struct task_struct *tsk);
extern void exit_sem(struct task_struct *tsk);

#else

struct sysv_sem {
	/* empty */
};

static inline int copy_semundo(unsigned long clone_flags, struct task_struct *tsk)
{
	return 0;
}

static inline void exit_sem(struct task_struct *tsk)
{
	return;
}
#endif

#endif /* __KERNEL__ */

#endif /* _LINUX_SEM_H */
