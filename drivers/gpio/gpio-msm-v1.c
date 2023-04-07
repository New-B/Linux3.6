/*
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2009-2011, Code Aurora Forum. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/bitops.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <mach/cpu.h>
#include <mach/msm_gpiomux.h>
#include <mach/msm_iomap.h>

/* see 80-VA736-2 Rev C pp 695-751
**
** These are actually the *shadow* gpio registers, since the
** real ones (which allow full access) are only available to the
** ARM9 side of the world.
**
** Since the _BASE need to be page-aligned when we're mapping them
** to virtual addresses, adjust for the additional offset in these
** macros.
*/

#define MSM_GPIO1_REG(off) (MSM_GPIO1_BASE + (off))
#define MSM_GPIO2_REG(off) (MSM_GPIO2_BASE + 0x400 + (off))
#define MSM_GPIO1_SHADOW_REG(off) (MSM_GPIO1_BASE + 0x800 + (off))
#define MSM_GPIO2_SHADOW_REG(off) (MSM_GPIO2_BASE + 0xC00 + (off))

/*
 * MSM7X00 registers
 */
/* output value */
#define MSM7X00_GPIO_OUT_0	MSM_GPIO1_SHADOW_REG(0x00)  /* gpio  15-0  */
#define MSM7X00_GPIO_OUT_1	MSM_GPIO2_SHADOW_REG(0x00)  /* gpio  42-16 */
#define MSM7X00_GPIO_OUT_2	MSM_GPIO1_SHADOW_REG(0x04)  /* gpio  67-43 */
#define MSM7X00_GPIO_OUT_3	MSM_GPIO1_SHADOW_REG(0x08)  /* gpio  94-68 */
#define MSM7X00_GPIO_OUT_4	MSM_GPIO1_SHADOW_REG(0x0C)  /* gpio 106-95 */
#define MSM7X00_GPIO_OUT_5	MSM_GPIO1_SHADOW_REG(0x50)  /* gpio 107-121 */

/* same pin map as above, output enable */
#define MSM7X00_GPIO_OE_0	MSM_GPIO1_SHADOW_REG(0x10)
#define MSM7X00_GPIO_OE_1	MSM_GPIO2_SHADOW_REG(0x08)
#define MSM7X00_GPIO_OE_2	MSM_GPIO1_SHADOW_REG(0x14)
#define MSM7X00_GPIO_OE_3	MSM_GPIO1_SHADOW_REG(0x18)
#define MSM7X00_GPIO_OE_4	MSM_GPIO1_SHADOW_REG(0x1C)
#define MSM7X00_GPIO_OE_5	MSM_GPIO1_SHADOW_REG(0x54)

/* same pin map as above, input read */
#define MSM7X00_GPIO_IN_0	MSM_GPIO1_SHADOW_REG(0x34)
#define MSM7X00_GPIO_IN_1	MSM_GPIO2_SHADOW_REG(0x20)
#define MSM7X00_GPIO_IN_2	MSM_GPIO1_SHADOW_REG(0x38)
#define MSM7X00_GPIO_IN_3	MSM_GPIO1_SHADOW_REG(0x3C)
#define MSM7X00_GPIO_IN_4	MSM_GPIO1_SHADOW_REG(0x40)
#define MSM7X00_GPIO_IN_5	MSM_GPIO1_SHADOW_REG(0x44)

/* same pin map as above, 1=edge 0=level interrup */
#define MSM7X00_GPIO_INT_EDGE_0	MSM_GPIO1_SHADOW_REG(0x60)
#define MSM7X00_GPIO_INT_EDGE_1	MSM_GPIO2_SHADOW_REG(0x50)
#define MSM7X00_GPIO_INT_EDGE_2	MSM_GPIO1_SHADOW_REG(0x64)
#define MSM7X00_GPIO_INT_EDGE_3	MSM_GPIO1_SHADOW_REG(0x68)
#define MSM7X00_GPIO_INT_EDGE_4	MSM_GPIO1_SHADOW_REG(0x6C)
#define MSM7X00_GPIO_INT_EDGE_5	MSM_GPIO1_SHADOW_REG(0xC0)

/* same pin map as above, 1=positive 0=negative */
#define MSM7X00_GPIO_INT_POS_0	MSM_GPIO1_SHADOW_REG(0x70)
#define MSM7X00_GPIO_INT_POS_1	MSM_GPIO2_SHADOW_REG(0x58)
#define MSM7X00_GPIO_INT_POS_2	MSM_GPIO1_SHADOW_REG(0x74)
#define MSM7X00_GPIO_INT_POS_3	MSM_GPIO1_SHADOW_REG(0x78)
#define MSM7X00_GPIO_INT_POS_4	MSM_GPIO1_SHADOW_REG(0x7C)
#define MSM7X00_GPIO_INT_POS_5	MSM_GPIO1_SHADOW_REG(0xBC)

/* same pin map as above, interrupt enable */
#define MSM7X00_GPIO_INT_EN_0	MSM_GPIO1_SHADOW_REG(0x80)
#define MSM7X00_GPIO_INT_EN_1	MSM_GPIO2_SHADOW_REG(0x60)
#define MSM7X00_GPIO_INT_EN_2	MSM_GPIO1_SHADOW_REG(0x84)
#define MSM7X00_GPIO_INT_EN_3	MSM_GPIO1_SHADOW_REG(0x88)
#define MSM7X00_GPIO_INT_EN_4	MSM_GPIO1_SHADOW_REG(0x8C)
#define MSM7X00_GPIO_INT_EN_5	MSM_GPIO1_SHADOW_REG(0xB8)

/* same pin map as above, write 1 to clear interrupt */
#define MSM7X00_GPIO_INT_CLEAR_0	MSM_GPIO1_SHADOW_REG(0x90)
#define MSM7X00_GPIO_INT_CLEAR_1	MSM_GPIO2_SHADOW_REG(0x68)
#define MSM7X00_GPIO_INT_CLEAR_2	MSM_GPIO1_SHADOW_REG(0x94)
#define MSM7X00_GPIO_INT_CLEAR_3	MSM_GPIO1_SHADOW_REG(0x98)
#define MSM7X00_GPIO_INT_CLEAR_4	MSM_GPIO1_SHADOW_REG(0x9C)
#define MSM7X00_GPIO_INT_CLEAR_5	MSM_GPIO1_SHADOW_REG(0xB4)

/* same pin map as above, 1=interrupt pending */
#define MSM7X00_GPIO_INT_STATUS_0	MSM_GPIO1_SHADOW_REG(0xA0)
#define MSM7X00_GPIO_INT_STATUS_1	MSM_GPIO2_SHADOW_REG(0x70)
#define MSM7X00_GPIO_INT_STATUS_2	MSM_GPIO1_SHADOW_REG(0xA4)
#define MSM7X00_GPIO_INT_STATUS_3	MSM_GPIO1_SHADOW_REG(0xA8)
#define MSM7X00_GPIO_INT_STATUS_4	MSM_GPIO1_SHADOW_REG(0xAC)
#define MSM7X00_GPIO_INT_STATUS_5	MSM_GPIO1_SHADOW_REG(0xB0)

/*
 * QSD8X50 registers
 */
/* output value */
#define QSD8X50_GPIO_OUT_0	MSM_GPIO1_SHADOW_REG(0x00)  /* gpio  15-0   */
#define QSD8X50_GPIO_OUT_1	MSM_GPIO2_SHADOW_REG(0x00)  /* gpio  42-16  */
#define QSD8X50_GPIO_OUT_2	MSM_GPIO1_SHADOW_REG(0x04)  /* gpio  67-43  */
#define QSD8X50_GPIO_OUT_3	MSM_GPIO1_SHADOW_REG(0x08)  /* gpio  94-68  */
#define QSD8X50_GPIO_OUT_4	MSM_GPIO1_SHADOW_REG(0x0C)  /* gpio 103-95  */
#define QSD8X50_GPIO_OUT_5	MSM_GPIO1_SHADOW_REG(0x10)  /* gpio 121-104 */
#define QSD8X50_GPIO_OUT_6	MSM_GPIO1_SHADOW_REG(0x14)  /* gpio 152-122 */
#define QSD8X50_GPIO_OUT_7	MSM_GPIO1_SHADOW_REG(0x18)  /* gpio 164-153 */

/* same pin map as above, output enable */
#define QSD8X50_GPIO_OE_0	MSM_GPIO1_SHADOW_REG(0x20)
#define QSD8X50_GPIO_OE_1	MSM_GPIO2_SHADOW_REG(0x08)
#define QSD8X50_GPIO_OE_2	MSM_GPIO1_SHADOW_REG(0x24)
#define QSD8X50_GPIO_OE_3	MSM_GPIO1_SHADOW_REG(0x28)
#define QSD8X50_GPIO_OE_4	MSM_GPIO1_SHADOW_REG(0x2C)
#define QSD8X50_GPIO_OE_5	MSM_GPIO1_SHADOW_REG(0x30)
#define QSD8X50_GPIO_OE_6	MSM_GPIO1_SHADOW_REG(0x34)
#define QSD8X50_GPIO_OE_7	MSM_GPIO1_SHADOW_REG(0x38)

/* same pin map as above, input read */
#define QSD8X50_GPIO_IN_0	MSM_GPIO1_SHADOW_REG(0x50)
#define QSD8X50_GPIO_IN_1	MSM_GPIO2_SHADOW_REG(0x20)
#define QSD8X50_GPIO_IN_2	MSM_GPIO1_SHADOW_REG(0x54)
#define QSD8X50_GPIO_IN_3	MSM_GPIO1_SHADOW_REG(0x58)
#define QSD8X50_GPIO_IN_4	MSM_GPIO1_SHADOW_REG(0x5C)
#define QSD8X50_GPIO_IN_5	MSM_GPIO1_SHADOW_REG(0x60)
#define QSD8X50_GPIO_IN_6	MSM_GPIO1_SHADOW_REG(0x64)
#define QSD8X50_GPIO_IN_7	MSM_GPIO1_SHADOW_REG(0x68)

/* same pin map as above, 1=edge 0=level interrup */
#define QSD8X50_GPIO_INT_EDGE_0	MSM_GPIO1_SHADOW_REG(0x70)
#define QSD8X50_GPIO_INT_EDGE_1	MSM_GPIO2_SHADOW_REG(0x50)
#define QSD8X50_GPIO_INT_EDGE_2	MSM_GPIO1_SHADOW_REG(0x74)
#define QSD8X50_GPIO_INT_EDGE_3	MSM_GPIO1_SHADOW_REG(0x78)
#define QSD8X50_GPIO_INT_EDGE_4	MSM_GPIO1_SHADOW_REG(0x7C)
#define QSD8X50_GPIO_INT_EDGE_5	MSM_GPIO1_SHADOW_REG(0x80)
#define QSD8X50_GPIO_INT_EDGE_6	MSM_GPIO1_SHADOW_REG(0x84)
#define QSD8X50_GPIO_INT_EDGE_7	MSM_GPIO1_SHADOW_REG(0x88)

/* same pin map as above, 1=positive 0=negative */
#define QSD8X50_GPIO_INT_POS_0	MSM_GPIO1_SHADOW_REG(0x90)
#define QSD8X50_GPIO_INT_POS_1	MSM_GPIO2_SHADOW_REG(0x58)
#define QSD8X50_GPIO_INT_POS_2	MSM_GPIO1_SHADOW_REG(0x94)
#define QSD8X50_GPIO_INT_POS_3	MSM_GPIO1_SHADOW_REG(0x98)
#define QSD8X50_GPIO_INT_POS_4	MSM_GPIO1_SHADOW_REG(0x9C)
#define QSD8X50_GPIO_INT_POS_5	MSM_GPIO1_SHADOW_REG(0xA0)
#define QSD8X50_GPIO_INT_POS_6	MSM_GPIO1_SHADOW_REG(0xA4)
#define QSD8X50_GPIO_INT_POS_7	MSM_GPIO1_SHADOW_REG(0xA8)

/* same pin map as above, interrupt enable */
#define QSD8X50_GPIO_INT_EN_0	MSM_GPIO1_SHADOW_REG(0xB0)
#define QSD8X50_GPIO_INT_EN_1	MSM_GPIO2_SHADOW_REG(0x60)
#define QSD8X50_GPIO_INT_EN_2	MSM_GPIO1_SHADOW_REG(0xB4)
#define QSD8X50_GPIO_INT_EN_3	MSM_GPIO1_SHADOW_REG(0xB8)
#define QSD8X50_GPIO_INT_EN_4	MSM_GPIO1_SHADOW_REG(0xBC)
#define QSD8X50_GPIO_INT_EN_5	MSM_GPIO1_SHADOW_REG(0xC0)
#define QSD8X50_GPIO_INT_EN_6	MSM_GPIO1_SHADOW_REG(0xC4)
#define QSD8X50_GPIO_INT_EN_7	MSM_GPIO1_SHADOW_REG(0xC8)

/* same pin map as above, write 1 to clear interrupt */
#define QSD8X50_GPIO_INT_CLEAR_0	MSM_GPIO1_SHADOW_REG(0xD0)
#define QSD8X50_GPIO_INT_CLEAR_1	MSM_GPIO2_SHADOW_REG(0x68)
#define QSD8X50_GPIO_INT_CLEAR_2	MSM_GPIO1_SHADOW_REG(0xD4)
#define QSD8X50_GPIO_INT_CLEAR_3	MSM_GPIO1_SHADOW_REG(0xD8)
#define QSD8X50_GPIO_INT_CLEAR_4	MSM_GPIO1_SHADOW_REG(0xDC)
#define QSD8X50_GPIO_INT_CLEAR_5	MSM_GPIO1_SHADOW_REG(0xE0)
#define QSD8X50_GPIO_INT_CLEAR_6	MSM_GPIO1_SHADOW_REG(0xE4)
#define QSD8X50_GPIO_INT_CLEAR_7	MSM_GPIO1_SHADOW_REG(0xE8)

/* same pin map as above, 1=interrupt pending */
#define QSD8X50_GPIO_INT_STATUS_0	MSM_GPIO1_SHADOW_REG(0xF0)
#define QSD8X50_GPIO_INT_STATUS_1	MSM_GPIO2_SHADOW_REG(0x70)
#define QSD8X50_GPIO_INT_STATUS_2	MSM_GPIO1_SHADOW_REG(0xF4)
#define QSD8X50_GPIO_INT_STATUS_3	MSM_GPIO1_SHADOW_REG(0xF8)
#define QSD8X50_GPIO_INT_STATUS_4	MSM_GPIO1_SHADOW_REG(0xFC)
#define QSD8X50_GPIO_INT_STATUS_5	MSM_GPIO1_SHADOW_REG(0x100)
#define QSD8X50_GPIO_INT_STATUS_6	MSM_GPIO1_SHADOW_REG(0x104)
#define QSD8X50_GPIO_INT_STATUS_7	MSM_GPIO1_SHADOW_REG(0x108)

/*
 * MSM7X30 registers
 */
/* output value */
#define MSM7X30_GPIO_OUT_0	MSM_GPIO1_REG(0x00)   /* gpio  15-0   */
#define MSM7X30_GPIO_OUT_1	MSM_GPIO2_REG(0x00)   /* gpio  43-16  */
#define MSM7X30_GPIO_OUT_2	MSM_GPIO1_REG(0x04)   /* gpio  67-44  */
#define MSM7X30_GPIO_OUT_3	MSM_GPIO1_REG(0x08)   /* gpio  94-68  */
#define MSM7X30_GPIO_OUT_4	MSM_GPIO1_REG(0x0C)   /* gpio 106-95  */
#define MSM7X30_GPIO_OUT_5	MSM_GPIO1_REG(0x50)   /* gpio 133-107 */
#define MSM7X30_GPIO_OUT_6	MSM_GPIO1_REG(0xC4)   /* gpio 150-134 */
#define MSM7X30_GPIO_OUT_7	MSM_GPIO1_REG(0x214)  /* gpio 181-151 */

/* same pin map as above, output enable */
#define MSM7X30_GPIO_OE_0	MSM_GPIO1_REG(0x10)
#define MSM7X30_GPIO_OE_1	MSM_GPIO2_REG(0x08)
#define MSM7X30_GPIO_OE_2	MSM_GPIO1_REG(0x14)
#define MSM7X30_GPIO_OE_3	MSM_GPIO1_REG(0x18)
#define MSM7X30_GPIO_OE_4	MSM_GPIO1_REG(0x1C)
#define MSM7X30_GPIO_OE_5	MSM_GPIO1_REG(0x54)
#define MSM7X30_GPIO_OE_6	MSM_GPIO1_REG(0xC8)
#define MSM7X30_GPIO_OE_7	MSM_GPIO1_REG(0x218)

/* same pin map as above, input read */
#define MSM7X30_GPIO_IN_0	MSM_GPIO1_REG(0x34)
#define MSM7X30_GPIO_IN_1	MSM_GPIO2_REG(0x20)
#define MSM7X30_GPIO_IN_2	MSM_GPIO1_REG(0x38)
#define MSM7X30_GPIO_IN_3	MSM_GPIO1_REG(0x3C)
#define MSM7X30_GPIO_IN_4	MSM_GPIO1_REG(0x40)
#define MSM7X30_GPIO_IN_5	MSM_GPIO1_REG(0x44)
#define MSM7X30_GPIO_IN_6	MSM_GPIO1_REG(0xCC)
#define MSM7X30_GPIO_IN_7	MSM_GPIO1_REG(0x21C)

/* same pin map as above, 1=edge 0=level interrup */
#define MSM7X30_GPIO_INT_EDGE_0	MSM_GPIO1_REG(0x60)
#define MSM7X30_GPIO_INT_EDGE_1	MSM_GPIO2_REG(0x50)
#define MSM7X30_GPIO_INT_EDGE_2	MSM_GPIO1_REG(0x64)
#define MSM7X30_GPIO_INT_EDGE_3	MSM_GPIO1_REG(0x68)
#define MSM7X30_GPIO_INT_EDGE_4	MSM_GPIO1_REG(0x6C)
#define MSM7X30_GPIO_INT_EDGE_5	MSM_GPIO1_REG(0xC0)
#define MSM7X30_GPIO_INT_EDGE_6	MSM_GPIO1_REG(0xD0)
#define MSM7X30_GPIO_INT_EDGE_7	MSM_GPIO1_REG(0x240)

/* same pin map as above, 1=positive 0=negative */
#define MSM7X30_GPIO_INT_POS_0	MSM_GPIO1_REG(0x70)
#define MSM7X30_GPIO_INT_POS_1	MSM_GPIO2_REG(0x58)
#define MSM7X30_GPIO_INT_POS_2	MSM_GPIO1_REG(0x74)
#define MSM7X30_GPIO_INT_POS_3	MSM_GPIO1_REG(0x78)
#define MSM7X30_GPIO_INT_POS_4	MSM_GPIO1_REG(0x7C)
#define MSM7X30_GPIO_INT_POS_5	MSM_GPIO1_REG(0xBC)
#define MSM7X30_GPIO_INT_POS_6	MSM_GPIO1_REG(0xD4)
#define MSM7X30_GPIO_INT_POS_7	MSM_GPIO1_REG(0x228)

/* same pin map as above, interrupt enable */
#define MSM7X30_GPIO_INT_EN_0	MSM_GPIO1_REG(0x80)
#define MSM7X30_GPIO_INT_EN_1	MSM_GPIO2_REG(0x60)
#define MSM7X30_GPIO_INT_EN_2	MSM_GPIO1_REG(0x84)
#define MSM7X30_GPIO_INT_EN_3	MSM_GPIO1_REG(0x88)
#define MSM7X30_GPIO_INT_EN_4	MSM_GPIO1_REG(0x8C)
#define MSM7X30_GPIO_INT_EN_5	MSM_GPIO1_REG(0xB8)
#define MSM7X30_GPIO_INT_EN_6	MSM_GPIO1_REG(0xD8)
#define MSM7X30_GPIO_INT_EN_7	MSM_GPIO1_REG(0x22C)

/* same pin map as above, write 1 to clear interrupt */
#define MSM7X30_GPIO_INT_CLEAR_0	MSM_GPIO1_REG(0x90)
#define MSM7X30_GPIO_INT_CLEAR_1	MSM_GPIO2_REG(0x68)
#define MSM7X30_GPIO_INT_CLEAR_2	MSM_GPIO1_REG(0x94)
#define MSM7X30_GPIO_INT_CLEAR_3	MSM_GPIO1_REG(0x98)
#define MSM7X30_GPIO_INT_CLEAR_4	MSM_GPIO1_REG(0x9C)
#define MSM7X30_GPIO_INT_CLEAR_5	MSM_GPIO1_REG(0xB4)
#define MSM7X30_GPIO_INT_CLEAR_6	MSM_GPIO1_REG(0xDC)
#define MSM7X30_GPIO_INT_CLEAR_7	MSM_GPIO1_REG(0x230)

/* same pin map as above, 1=interrupt pending */
#define MSM7X30_GPIO_INT_STATUS_0	MSM_GPIO1_REG(0xA0)
#define MSM7X30_GPIO_INT_STATUS_1	MSM_GPIO2_REG(0x70)
#define MSM7X30_GPIO_INT_STATUS_2	MSM_GPIO1_REG(0xA4)
#define MSM7X30_GPIO_INT_STATUS_3	MSM_GPIO1_REG(0xA8)
#define MSM7X30_GPIO_INT_STATUS_4	MSM_GPIO1_REG(0xAC)
#define MSM7X30_GPIO_INT_STATUS_5	MSM_GPIO1_REG(0xB0)
#define MSM7X30_GPIO_INT_STATUS_6	MSM_GPIO1_REG(0xE0)
#define MSM7X30_GPIO_INT_STATUS_7	MSM_GPIO1_REG(0x234)

#define FIRST_GPIO_IRQ MSM_GPIO_TO_INT(0)

#define MSM_GPIO_BANK(soc, bank, first, last)				\
	{								\
		.regs = {						\
			.out =         soc##_GPIO_OUT_##bank,		\
			.in =          soc##_GPIO_IN_##bank,		\
			.int_status =  soc##_GPIO_INT_STATUS_##bank,	\
			.int_clear =   soc##_GPIO_INT_CLEAR_##bank,	\
			.int_en =      soc##_GPIO_INT_EN_##bank,	\
			.int_edge =    soc##_GPIO_INT_EDGE_##bank,	\
			.int_pos =     soc##_GPIO_INT_POS_##bank,	\
			.oe =          soc##_GPIO_OE_##bank,		\
		},							\
		.chip = {						\
			.base = (first),				\
			.ngpio = (last) - (first) + 1,			\
			.get = msm_gpio_get,				\
			.set = msm_gpio_set,				\
			.direction_input = msm_gpio_direction_input,	\
			.direction_output = msm_gpio_direction_output,	\
			.to_irq = msm_gpio_to_irq,			\
			.request = msm_gpio_request,			\
			.free = msm_gpio_free,				\
		}							\
	}

#define MSM_GPIO_BROKEN_INT_CLEAR 1

struct msm_gpio_regs {
	void __iomem *out;
	void __iomem *in;
	void __iomem *int_status;
	void __iomem *int_clear;
	void __iomem *int_en;
	void __iomem *int_edge;
	void __iomem *int_pos;
	void __iomem *oe;
};

struct msm_gpio_chip {
	spinlock_t		lock;
	struct gpio_chip	chip;
	struct msm_gpio_regs	regs;
#if MSM_GPIO_BROKEN_INT_CLEAR
	unsigned                int_status_copy;
#endif
	unsigned int            both_edge_detect;
	unsigned int            int_enable[2]; /* 0: awake, 1: sleep */
};

static int msm_gpio_write(struct msm_gpio_chip *msm_chip,
			  unsigned offset, unsigned on)
{
	unsigned mask = BIT(offset);
	unsigned val;

	val = readl(msm_chip->regs.out);
	if (on)
		writel(val | mask, msm_chip->regs.out);
	else
		writel(val & ~mask, msm_chip->regs.out);
	return 0;
}

static void msm_gpio_update_both_edge_detect(struct msm_gpio_chip *msm_chip)
{
	int loop_limit = 100;
	unsigned pol, val, val2, intstat;
	do {
		val = readl(msm_chip->regs.in);
		pol = readl(msm_chip->regs.int_pos);
		pol = (pol & ~msm_chip->both_edge_detect) |
		      (~val & msm_chip->both_edge_detect);
		writel(pol, msm_chip->regs.int_pos);
		intstat = readl(msm_chip->regs.int_status);
		val2 = readl(msm_chip->regs.in);
		if (((val ^ val2) & msm_chip->both_edge_detect & ~intstat) == 0)
			return;
	} while (loop_limit-- > 0);
	printk(KERN_ERR "msm_gpio_update_both_edge_detect, "
	       "failed to reach stable state %x != %x\n", val, val2);
}

static int msm_gpio_clear_detect_status(struct msm_gpio_chip *msm_chip,
					unsigned offset)
{
	unsigned bit = BIT(offset);

#if MSM_GPIO_BROKEN_INT_CLEAR
	/* Save interrupts that already triggered before we loose them. */
	/* Any interrupt that triggers between the read of int_status */
	/* and the write to int_clear will still be lost though. */
	msm_chip->int_status_copy |= readl(msm_chip->regs.int_status);
	msm_chip->int_status_copy &= ~bit;
#endif
	writel(bit, msm_chip->regs.int_clear);
	msm_gpio_update_both_edge_detect(msm_chip);
	return 0;
}

static int msm_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct msm_gpio_chip *msm_chip;
	unsigned long irq_flags;

	msm_chip = container_of(chip, struct msm_gpio_chip, chip);
	spin_lock_irqsave(&msm_chip->lock, irq_flags);
	writel(readl(msm_chip->regs.oe) & ~BIT(offset), msm_chip->regs.oe);
	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
	return 0;
}

static int
msm_gpio_direction_output(struct gpio_chip *chip, unsigned offset, int value)
{
	struct msm_gpio_chip *msm_chip;
	unsigned long irq_flags;

	msm_chip = container_of(chip, struct msm_gpio_chip, chip);
	spin_lock_irqsave(&msm_chip->lock, irq_flags);
	msm_gpio_write(msm_chip, offset, value);
	writel(readl(msm_chip->regs.oe) | BIT(offset), msm_chip->regs.oe);
	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
	return 0;
}

static int msm_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct msm_gpio_chip *msm_chip;

	msm_chip = container_of(chip, struct msm_gpio_chip, chip);
	return (readl(msm_chip->regs.in) & (1U << offset)) ? 1 : 0;
}

static void msm_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct msm_gpio_chip *msm_chip;
	unsigned long irq_flags;

	msm_chip = container_of(chip, struct msm_gpio_chip, chip);
	spin_lock_irqsave(&msm_chip->lock, irq_flags);
	msm_gpio_write(msm_chip, offset, value);
	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
}

static int msm_gpio_to_irq(struct gpio_chip *chip, unsigned offset)
{
	return MSM_GPIO_TO_INT(chip->base + offset);
}

#ifdef CONFIG_MSM_GPIOMUX
static int msm_gpio_request(struct gpio_chip *chip, unsigned offset)
{
	return msm_gpiomux_get(chip->base + offset);
}

static void msm_gpio_free(struct gpio_chip *chip, unsigned offset)
{
	msm_gpiomux_put(chip->base + offset);
}
#else
#define msm_gpio_request NULL
#define msm_gpio_free NULL
#endif

static struct msm_gpio_chip *msm_gpio_chips;
static int msm_gpio_count;

static struct msm_gpio_chip msm_gpio_chips_msm7x01[] = {
	MSM_GPIO_BANK(MSM7X00, 0,   0,  15),
	MSM_GPIO_BANK(MSM7X00, 1,  16,  42),
	MSM_GPIO_BANK(MSM7X00, 2,  43,  67),
	MSM_GPIO_BANK(MSM7X00, 3,  68,  94),
	MSM_GPIO_BANK(MSM7X00, 4,  95, 106),
	MSM_GPIO_BANK(MSM7X00, 5, 107, 121),
};

static struct msm_gpio_chip msm_gpio_chips_msm7x30[] = {
	MSM_GPIO_BANK(MSM7X30, 0,   0,  15),
	MSM_GPIO_BANK(MSM7X30, 1,  16,  43),
	MSM_GPIO_BANK(MSM7X30, 2,  44,  67),
	MSM_GPIO_BANK(MSM7X30, 3,  68,  94),
	MSM_GPIO_BANK(MSM7X30, 4,  95, 106),
	MSM_GPIO_BANK(MSM7X30, 5, 107, 133),
	MSM_GPIO_BANK(MSM7X30, 6, 134, 150),
	MSM_GPIO_BANK(MSM7X30, 7, 151, 181),
};

static struct msm_gpio_chip msm_gpio_chips_qsd8x50[] = {
	MSM_GPIO_BANK(QSD8X50, 0,   0,  15),
	MSM_GPIO_BANK(QSD8X50, 1,  16,  42),
	MSM_GPIO_BANK(QSD8X50, 2,  43,  67),
	MSM_GPIO_BANK(QSD8X50, 3,  68,  94),
	MSM_GPIO_BANK(QSD8X50, 4,  95, 103),
	MSM_GPIO_BANK(QSD8X50, 5, 104, 121),
	MSM_GPIO_BANK(QSD8X50, 6, 122, 152),
	MSM_GPIO_BANK(QSD8X50, 7, 153, 164),
};

static void msm_gpio_irq_ack(struct irq_data *d)
{
	unsigned long irq_flags;
	struct msm_gpio_chip *msm_chip = irq_data_get_irq_chip_data(d);
	spin_lock_irqsave(&msm_chip->lock, irq_flags);
	msm_gpio_clear_detect_status(msm_chip,
				     d->irq - gpio_to_irq(msm_chip->chip.base));
	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
}

static void msm_gpio_irq_mask(struct irq_data *d)
{
	unsigned long irq_flags;
	struct msm_gpio_chip *msm_chip = irq_data_get_irq_chip_data(d);
	unsigned offset = d->irq - gpio_to_irq(msm_chip->chip.base);

	spin_lock_irqsave(&msm_chip->lock, irq_flags);
	/* level triggered interrupts are also latched */
	if (!(readl(msm_chip->regs.int_edge) & BIT(offset)))
		msm_gpio_clear_detect_status(msm_chip, offset);
	msm_chip->int_enable[0] &= ~BIT(offset);
	writel(msm_chip->int_enable[0], msm_chip->regs.int_en);
	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
}

static void msm_gpio_irq_unmask(struct irq_data *d)
{
	unsigned long irq_flags;
	struct msm_gpio_chip *msm_chip = irq_data_get_irq_chip_data(d);
	unsigned offset = d->irq - gpio_to_irq(msm_chip->chip.base);

	spin_lock_irqsave(&msm_chip->lock, irq_flags);
	/* level triggered interrupts are also latched */
	if (!(readl(msm_chip->regs.int_edge) & BIT(offset)))
		msm_gpio_clear_detect_status(msm_chip, offset);
	msm_chip->int_enable[0] |= BIT(offset);
	writel(msm_chip->int_enable[0], msm_chip->regs.int_en);
	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
}

static int msm_gpio_irq_set_wake(struct irq_data *d, unsigned int on)
{
	unsigned long irq_flags;
	struct msm_gpio_chip *msm_chip = irq_data_get_irq_chip_data(d);
	unsigned offset = d->irq - gpio_to_irq(msm_chip->chip.base);

	spin_lock_irqsave(&msm_chip->lock, irq_flags);

	if (on)
		msm_chip->int_enable[1] |= BIT(offset);
	else
		msm_chip->int_enable[1] &= ~BIT(offset);

	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
	return 0;
}

static int msm_gpio_irq_set_type(struct irq_data *d, unsigned int flow_type)
{
	unsigned long irq_flags;
	struct msm_gpio_chip *msm_chip = irq_data_get_irq_chip_data(d);
	unsigned offset = d->irq - gpio_to_irq(msm_chip->chip.base);
	unsigned val, mask = BIT(offset);

	spin_lock_irqsave(&msm_chip->lock, irq_flags);
	val = readl(msm_chip->regs.int_edge);
	if (flow_type & IRQ_TYPE_EDGE_BOTH) {
		writel(val | mask, msm_chip->regs.int_edge);
		__irq_set_handler_locked(d->irq, handle_edge_irq);
	} else {
		writel(val & ~mask, msm_chip->regs.int_edge);
		__irq_set_handler_locked(d->irq, handle_level_irq);
	}
	if ((flow_type & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH) {
		msm_chip->both_edge_detect |= mask;
		msm_gpio_update_both_edge_detect(msm_chip);
	} else {
		msm_chip->both_edge_detect &= ~mask;
		val = readl(msm_chip->regs.int_pos);
		if (flow_type & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_HIGH))
			writel(val | mask, msm_chip->regs.int_pos);
		else
			writel(val & ~mask, msm_chip->regs.int_pos);
	}
	spin_unlock_irqrestore(&msm_chip->lock, irq_flags);
	return 0;
}

static void msm_gpio_irq_handler(unsigned int irq, struct irq_desc *desc)
{
	int i, j, mask;
	unsigned val;

	for (i = 0; i < msm_gpio_count; i++) {
		struct msm_gpio_chip *msm_chip = &msm_gpio_chips[i];
		val = readl(msm_chip->regs.int_status);
		val &= msm_chip->int_enable[0];
		while (val) {
			mask = val & -val;
			j = fls(mask) - 1;
			/* printk("%s %08x %08x bit %d gpio %d irq %d\n",
				__func__, v, m, j, msm_chip->chip.start + j,
				FIRST_GPIO_IRQ + msm_chip->chip.start + j); */
			val &= ~mask;
			generic_handle_irq(FIRST_GPIO_IRQ +
					   msm_chip->chip.base + j);
		}
	}
	desc->irq_data.chip->irq_ack(&desc->irq_data);
}

static struct irq_chip msm_gpio_irq_chip = {
	.name          = "msmgpio",
	.irq_ack       = msm_gpio_irq_ack,
	.irq_mask      = msm_gpio_irq_mask,
	.irq_unmask    = msm_gpio_irq_unmask,
	.irq_set_wake  = msm_gpio_irq_set_wake,
	.irq_set_type  = msm_gpio_irq_set_type,
};

static int __init msm_init_gpio(void)
{
	int i, j = 0;

	if (cpu_is_msm7x01()) {
		msm_gpio_chips = msm_gpio_chips_msm7x01;
		msm_gpio_count = ARRAY_SIZE(msm_gpio_chips_msm7x01);
	} else if (cpu_is_msm7x30()) {
		msm_gpio_chips = msm_gpio_chips_msm7x30;
		msm_gpio_count = ARRAY_SIZE(msm_gpio_chips_msm7x30);
	} else if (cpu_is_qsd8x50()) {
		msm_gpio_chips = msm_gpio_chips_qsd8x50;
		msm_gpio_count = ARRAY_SIZE(msm_gpio_chips_qsd8x50);
	} else {
		return 0;
	}

	for (i = FIRST_GPIO_IRQ; i < FIRST_GPIO_IRQ + NR_GPIO_IRQS; i++) {
		if (i - FIRST_GPIO_IRQ >=
			msm_gpio_chips[j].chip.base +
			msm_gpio_chips[j].chip.ngpio)
			j++;
		irq_set_chip_data(i, &msm_gpio_chips[j]);
		irq_set_chip_and_handler(i, &msm_gpio_irq_chip,
					 handle_edge_irq);
		set_irq_flags(i, IRQF_VALID);
	}

	for (i = 0; i < msm_gpio_count; i++) {
		spin_lock_init(&msm_gpio_chips[i].lock);
		writel(0, msm_gpio_chips[i].regs.int_en);
		gpiochip_add(&msm_gpio_chips[i].chip);
	}

	irq_set_chained_handler(INT_GPIO_GROUP1, msm_gpio_irq_handler);
	irq_set_chained_handler(INT_GPIO_GROUP2, msm_gpio_irq_handler);
	irq_set_irq_wake(INT_GPIO_GROUP1, 1);
	irq_set_irq_wake(INT_GPIO_GROUP2, 2);
	return 0;
}

postcore_initcall(msm_init_gpio);
