#ifndef __PARISC_MMAN_H__
#define __PARISC_MMAN_H__

#define PROT_READ	0x1		/* page can be read */
#define PROT_WRITE	0x2		/* page can be written */
#define PROT_EXEC	0x4		/* page can be executed */
#define PROT_SEM	0x8		/* page may be used for atomic ops */
#define PROT_NONE	0x0		/* page can not be accessed */
#define PROT_GROWSDOWN	0x01000000	/* mprotect flag: extend change to start of growsdown vma */
#define PROT_GROWSUP	0x02000000	/* mprotect flag: extend change to end of growsup vma */

#define MAP_SHARED	0x01		/* Share changes */
#define MAP_PRIVATE	0x02		/* Changes are private */
#define MAP_TYPE	0x03		/* Mask for type of mapping */
#define MAP_FIXED	0x04		/* Interpret addr exactly */
#define MAP_ANONYMOUS	0x10		/* don't use a file */

#define MAP_DENYWRITE	0x0800		/* ETXTBSY */
#define MAP_EXECUTABLE	0x1000		/* mark it as an executable */
#define MAP_LOCKED	0x2000		/* pages are locked */
#define MAP_NORESERVE	0x4000		/* don't check for reservations */
#define MAP_GROWSDOWN	0x8000		/* stack-like segment */
#define MAP_POPULATE	0x10000		/* populate (prefault) pagetables */
#define MAP_NONBLOCK	0x20000		/* do not block on IO */

#define MS_SYNC		1		/* synchronous memory sync */
#define MS_ASYNC	2		/* sync memory asynchronously */
#define MS_INVALIDATE	4		/* invalidate the caches */

#define MCL_CURRENT	1		/* lock all current mappings */
#define MCL_FUTURE	2		/* lock all future mappings */

#define MADV_NORMAL     0               /* no further special treatment */
#define MADV_RANDOM     1               /* expect random page references */
#define MADV_SEQUENTIAL 2               /* expect sequential page references */
#define MADV_WILLNEED   3               /* will need these pages */
#define MADV_DONTNEED   4               /* don't need these pages */
#define MADV_SPACEAVAIL 5               /* insure that resources are reserved */
#define MADV_VPS_PURGE  6               /* Purge pages from VM page cache */
#define MADV_VPS_INHERIT 7              /* Inherit parents page size */
#define MADV_REMOVE     8		/* remove these pages & resources */

/* The range 12-64 is reserved for page size specification. */
#define MADV_4K_PAGES   12              /* Use 4K pages  */
#define MADV_16K_PAGES  14              /* Use 16K pages */
#define MADV_64K_PAGES  16              /* Use 64K pages */
#define MADV_256K_PAGES 18              /* Use 256K pages */
#define MADV_1M_PAGES   20              /* Use 1 Megabyte pages */
#define MADV_4M_PAGES   22              /* Use 4 Megabyte pages */
#define MADV_16M_PAGES  24              /* Use 16 Megabyte pages */
#define MADV_64M_PAGES  26              /* Use 64 Megabyte pages */

/* compatibility flags */
#define MAP_ANON	MAP_ANONYMOUS
#define MAP_FILE	0
#define MAP_VARIABLE	0

#endif /* __PARISC_MMAN_H__ */
