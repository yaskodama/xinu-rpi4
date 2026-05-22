/* conf.h (GENERATED FILE; DO NOT EDIT) */

#ifndef _CONF_H_
#define _CONF_H_

#include <stddef.h>

/* Device table declarations */

/* Device table entry */
typedef struct dentry
{
	int     num;
	int     minor;
	char    *name;
	devcall (*init)(struct dentry *);
	devcall (*open)(struct dentry *, ...);
	devcall (*close)(struct dentry *);
	devcall (*read)(struct dentry *, void *, uint);
	devcall (*write)(struct dentry *, const void *, uint);
	devcall (*seek)(struct dentry *, long);
	devcall (*getc)(struct dentry *);
	devcall (*putc)(struct dentry *, char);
	devcall (*control)(struct dentry *, int, long, long);
	void    *csr;
	void    (*intr)(void);
	uchar   irq;
} device;

extern const device devtab[]; /* one entry per device */

/* Device name definitions */

#define SERIAL0     0       /* type uart     */
#define DEVNULL     1       /* type null     */
#define LOOP0       2       /* type loopback */
#define CONSOLE     3       /* type tty      */
#define WMCON0      4       /* type wmcon    */
#define TTYLOOP     5       /* type tty      */
#define RAMDISK0    6       /* type ramdisk  */
#define ETH0        7       /* type ether    */
#define ELOOP       8       /* type ethloop  */
#define RAW0        9       /* type raw      */
#define RAW1        10       /* type raw      */
#define UDP0        11       /* type udp      */
#define UDP1        12       /* type udp      */
#define UDP2        13       /* type udp      */
#define UDP3        14       /* type udp      */
#define TCP0        15       /* type tcp      */
#define TCP1        16       /* type tcp      */
#define TCP2        17       /* type tcp      */
#define TCP3        18       /* type tcp      */
#define TCP4        19       /* type tcp      */
#define TCP5        20       /* type tcp      */
#define TCP6        21       /* type tcp      */
#define TCP7        22       /* type tcp      */
#define TCP8        23       /* type tcp      */
#define TCP9        24       /* type tcp      */
#define TCP10       25       /* type tcp      */
#define TCP11       26       /* type tcp      */
#define TCP12       27       /* type tcp      */
#define TCP13       28       /* type tcp      */
#define TCP14       29       /* type tcp      */
#define TCP15       30       /* type tcp      */
#define TELNET0     31       /* type telnet   */
#define TELNET1     32       /* type telnet   */

/* Control block sizes */

#define NLOOPBACK 1
#define NNULL 1
#define NUART 1
#define NTTY 2
#define NWMCON 1
#define NRAMDISK 1
#define NETHER 1
#define NETHLOOP 1
#define NRAW 2
#define NUDP 4
#define NTCP 16
#define NTELNET 2

#define DEVMAXNAME 20
#define NDEVS 33


/* Interrupt line for the SP804 timer located at address 0x101E2000  */
#define IRQ_TIMER     4

/* Configuration and Size Constants */

#define LITTLE_ENDIAN 0x1234
#define BIG_ENDIAN    0x4321

#define BYTE_ORDER    LITTLE_ENDIAN

#define NTHREAD   100           /* number of user threads           */
#define NSEM      100           /* number of semaphores             */
#define NMAILBOX  15            /* number of mailboxes              */
#define RTCLOCK   TRUE          /* timer support                    */
#define NETEMU    FALSE         /* Network Emulator support         */
#define NVRAM     FALSE         /* nvram support                    */
#define SB_BUS    FALSE         /* Silicon Backplane support        */
#define USE_TLB   FALSE         /* make use of TLB                  */
#define USE_TAR   FALSE         /* enable data archives             */
#define NPOOL     8             /* number of buffer pools available */
#define POOL_MAX_BUFSIZE 2048   /* max size of a buffer in a pool   */
#define POOL_MIN_BUFSIZE 8      /* min size of a buffer in a pool   */
#define POOL_MAX_NBUFS   8192   /* max number of buffers in a pool  */

#endif /* _CONF_H_ */
