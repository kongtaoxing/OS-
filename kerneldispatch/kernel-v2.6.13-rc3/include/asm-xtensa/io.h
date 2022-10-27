/*
 * linux/include/asm-xtensa/io.h
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001 - 2005 Tensilica Inc.
 */

#ifndef _XTENSA_IO_H
#define _XTENSA_IO_H

#ifdef __KERNEL__
#include <linux/config.h>
#include <asm/byteorder.h>

#include <linux/types.h>
#include <asm/fixmap.h>

#define _IO_BASE 0


/*
 * swap functions to change byte order from little-endian to big-endian and
 * vice versa.
 */

static inline unsigned short _swapw (unsigned short v)
{
	return (v << 8) | (v >> 8);
}

static inline unsigned int _swapl (unsigned int v)
{
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

/*
 * Change virtual addresses to physical addresses and vv.
 * These are trivial on the 1:1 Linux/Xtensa mapping
 */

extern inline unsigned long virt_to_phys(volatile void * address)
{
	return PHYSADDR((unsigned long)address);
}

extern inline void * phys_to_virt(unsigned long address)
{
	return (void*) CACHED_ADDR(address);
}

/*
 * IO bus memory addresses are also 1:1 with the physical address
 */

extern inline unsigned long virt_to_bus(volatile void * address)
{
	return PHYSADDR((unsigned long)address);
}

extern inline void * bus_to_virt (unsigned long address)
{
	return (void *) CACHED_ADDR(address);
}

/*
 * Change "struct page" to physical address.
 */

extern inline void *ioremap(unsigned long offset, unsigned long size)
{
        return (void *) CACHED_ADDR_IO(offset);
}

extern inline void *ioremap_nocache(unsigned long offset, unsigned long size)
{
        return (void *) BYPASS_ADDR_IO(offset);
}

extern inline void iounmap(void *addr)
{
}

/*
 * Generic I/O
 */

#define readb(addr) \
	({ unsigned char __v = (*(volatile unsigned char *)(addr)); __v; })
#define readw(addr) \
	({ unsigned short __v = (*(volatile unsigned short *)(addr)); __v; })
#define readl(addr) \
	({ unsigned int __v = (*(volatile unsigned int *)(addr)); __v; })
#define writeb(b, addr) (void)((*(volatile unsigned char *)(addr)) = (b))
#define writew(b, addr) (void)((*(volatile unsigned short *)(addr)) = (b))
#define writel(b, addr) (void)((*(volatile unsigned int *)(addr)) = (b))

static inline __u8 __raw_readb(const volatile void __iomem *addr)
{
          return *(__force volatile __u8 *)(addr);
}
static inline __u16 __raw_readw(const volatile void __iomem *addr)
{
          return *(__force volatile __u16 *)(addr);
}
static inline __u32 __raw_readl(const volatile void __iomem *addr)
{
          return *(__force volatile __u32 *)(addr);
}
static inline void __raw_writeb(__u8 b, volatile void __iomem *addr)
{
          *(__force volatile __u8 *)(addr) = b;
}
static inline void __raw_writew(__u16 b, volatile void __iomem *addr)
{
          *(__force volatile __u16 *)(addr) = b;
}
static inline void __raw_writel(__u32 b, volatile void __iomem *addr)
{
          *(__force volatile __u32 *)(addr) = b;
}




/* These are the definitions for the x86 IO instructions
 * inb/inw/inl/outb/outw/outl, the "string" versions
 * insb/insw/insl/outsb/outsw/outsl, and the "pausing" versions
 * inb_p/inw_p/...
 * The macros don't do byte-swapping.
 */

#define inb(port)		readb((u8 *)((port)+_IO_BASE))
#define outb(val, port)		writeb((val),(u8 *)((unsigned long)(port)+_IO_BASE))
#define inw(port)		readw((u16 *)((port)+_IO_BASE))
#define outw(val, port)		writew((val),(u16 *)((unsigned long)(port)+_IO_BASE))
#define inl(port)		readl((u32 *)((port)+_IO_BASE))
#define outl(val, port)		writel((val),(u32 *)((unsigned long)(port)))

#define inb_p(port)		inb((port))
#define outb_p(val, port)	outb((val), (port))
#define inw_p(port)		inw((port))
#define outw_p(val, port)	outw((val), (port))
#define inl_p(port)		inl((port))
#define outl_p(val, port)	outl((val), (port))

extern void insb (unsigned long port, void *dst, unsigned long count);
extern void insw (unsigned long port, void *dst, unsigned long count);
extern void insl (unsigned long port, void *dst, unsigned long count);
extern void outsb (unsigned long port, const void *src, unsigned long count);
extern void outsw (unsigned long port, const void *src, unsigned long count);
extern void outsl (unsigned long port, const void *src, unsigned long count);

#define IO_SPACE_LIMIT ~0

#define memset_io(a,b,c)       memset((void *)(a),(b),(c))
#define memcpy_fromio(a,b,c)   memcpy((a),(void *)(b),(c))
#define memcpy_toio(a,b,c)      memcpy((void *)(a),(b),(c))

/* At this point the Xtensa doesn't provide byte swap instructions */

#ifdef __XTENSA_EB__
# define in_8(addr) (*(u8*)(addr))
# define in_le16(addr) _swapw(*(u16*)(addr))
# define in_le32(addr) _swapl(*(u32*)(addr))
# define out_8(b, addr) *(u8*)(addr) = (b)
# define out_le16(b, addr) *(u16*)(addr) = _swapw(b)
# define out_le32(b, addr) *(u32*)(addr) = _swapl(b)
#elif defined(__XTENSA_EL__)
# define in_8(addr)  (*(u8*)(addr))
# define in_le16(addr) (*(u16*)(addr))
# define in_le32(addr) (*(u32*)(addr))
# define out_8(b, addr) *(u8*)(addr) = (b)
# define out_le16(b, addr) *(u16*)(addr) = (b)
# define out_le32(b, addr) *(u32*)(addr) = (b)
#else
# error processor byte order undefined!
#endif


/*
 *  * Convert a physical pointer to a virtual kernel pointer for /dev/mem
 *   * access
 *    */
#define xlate_dev_mem_ptr(p)    __va(p)

/*
 *  * Convert a virtual cached pointer to an uncached pointer
 *   */
#define xlate_dev_kmem_ptr(p)   p


#endif	/* __KERNEL__ */

#endif	/* _XTENSA_IO_H */
