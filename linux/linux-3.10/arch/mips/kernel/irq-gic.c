/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2008 Ralf Baechle (ralf@linux-mips.org)
 * Copyright (C) 2012 MIPS Technologies, Inc.  All rights reserved.
 */
#include <linux/bitmap.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/irq.h>
#include <linux/clocksource.h>

#include <asm/io.h>
#include <asm/gic.h>
#include <asm/setup.h>
#include <asm/traps.h>
#include <asm/gcmpregs.h>
#include <linux/hardirq.h>
#include <asm-generic/bitops/find.h>

unsigned int gic_frequency;
unsigned int gic_present;
unsigned long _gic_base;
unsigned int gic_irq_base;
unsigned int gic_irq_flags[GIC_NUM_INTRS];

/* The index into this array is the vector # of the interrupt. */
struct gic_shared_intr_map gic_shared_intr_map[GIC_NUM_INTRS];

static struct gic_pcpu_mask pcpu_masks[NR_CPUS];
static struct gic_pending_regs pending_regs[NR_CPUS];
static struct gic_intrmask_regs intrmask_regs[NR_CPUS];

#if defined(CONFIG_CSRC_GIC) || defined(CONFIG_CEVT_GIC)
cycle_t gic_read_count(void)
{
	unsigned int hi, hi2, lo;

	do {
		GICREAD(GIC_REG(SHARED, GIC_SH_COUNTER_63_32), hi);
		GICREAD(GIC_REG(SHARED, GIC_SH_COUNTER_31_00), lo);
		GICREAD(GIC_REG(SHARED, GIC_SH_COUNTER_63_32), hi2);
	} while (hi2 != hi);

	return (((cycle_t) hi) << 32) + lo;
}

void gic_write_compare(cycle_t cnt)
{
	GICWRITE(GIC_REG(VPE_LOCAL, GIC_VPE_COMPARE_HI),
				(int)(cnt >> 32));
	GICWRITE(GIC_REG(VPE_LOCAL, GIC_VPE_COMPARE_LO),
				(int)(cnt & 0xffffffff));
}

cycle_t gic_read_compare(void)
{
	unsigned int hi, lo;

	GICREAD(GIC_REG(VPE_LOCAL, GIC_VPE_COMPARE_HI), hi);
	GICREAD(GIC_REG(VPE_LOCAL, GIC_VPE_COMPARE_LO), lo);

	return (((cycle_t) hi) << 32) + lo;
}
#endif

unsigned int gic_get_timer_pending(void)
{
	unsigned int vpe_pending;

	GICWRITE(GIC_REG(VPE_LOCAL, GIC_VPE_OTHER_ADDR), 0);
	GICREAD(GIC_REG(VPE_OTHER, GIC_VPE_PEND), vpe_pending);
	return (vpe_pending & GIC_VPE_PEND_TIMER_MSK);
}

void gic_bind_eic_interrupt(int irq, int set)
{
	/* Convert irq vector # to hw int # */
	irq -= GIC_PIN_TO_VEC_OFFSET;

	/* Set irq to use shadow set */
	GICWRITE(GIC_REG_ADDR(VPE_LOCAL, GIC_VPE_EIC_SS(irq)), set);
}

void gic_send_ipi(unsigned int intr)
{
	GICWRITE(GIC_REG(SHARED, GIC_SH_WEDGE), 0x80000000 | intr);
}

static void gic_eic_irq_dispatch(void)
{
	unsigned int cause = read_c0_cause();
	int irq;

	irq = (cause & ST0_IM) >> STATUSB_IP2;
	if (irq == 0)
		irq = -1;

	if (irq >= 0)
		do_IRQ(gic_irq_base + irq);
	else
		spurious_interrupt();
}

static void __init vpe_local_setup(unsigned int vpes, unsigned int flag, unsigned int intr)
{
	unsigned int vpe_ctl;
	int i;

	if (cpu_has_veic) {
		/*
		 * GIC timer/perfcnt interrupt -> CPU HW Int X (vector X+2) ->
		 * map to pin X+2-1 (since GIC adds 1)
		 */
		intr += (GIC_CPU_TO_VEC_OFFSET - GIC_PIN_TO_VEC_OFFSET);
	}

	flag &= GIC_FLAG_VPE_MASK;
	/*
	 * Setup the default performance counter timer interrupts
	 * for all VPEs
	 */
	for (i = 0; i < vpes; i++) {
		GICWRITE(GIC_REG(VPE_LOCAL, GIC_VPE_OTHER_ADDR), i);

		/* Are Interrupts locally routable? */
		GICREAD(GIC_REG(VPE_OTHER, GIC_VPE_CTL), vpe_ctl);
		switch (flag) {
		case GIC_FLAG_VPE_TIMER:
			if (vpe_ctl & GIC_VPE_CTL_TIMER_RTBL_MSK)
				GICWRITE(GIC_REG(VPE_OTHER, GIC_VPE_TIMER_MAP),
					 GIC_MAP_TO_PIN_MSK | intr);
			if (cpu_has_veic) {
				set_vi_handler(intr + GIC_PIN_TO_VEC_OFFSET,
					gic_eic_irq_dispatch);
				gic_shared_intr_map[intr + GIC_PIN_TO_VEC_OFFSET].local_intr_mask |= GIC_VPE_RMASK_TIMER_MSK;
			}
			break;
		case GIC_FLAG_VPE_PERFCTR:
			if (vpe_ctl & GIC_VPE_CTL_PERFCNT_RTBL_MSK)
				GICWRITE(GIC_REG(VPE_OTHER, GIC_VPE_PERFCTR_MAP),
					 GIC_MAP_TO_PIN_MSK | intr);
			if (cpu_has_veic) {
				set_vi_handler(intr + GIC_PIN_TO_VEC_OFFSET, gic_eic_irq_dispatch);
				gic_shared_intr_map[intr + GIC_PIN_TO_VEC_OFFSET].local_intr_mask |= GIC_VPE_RMASK_PERFCNT_MSK;
			}
			break;
		case GIC_FLAG_VPE_SWINT0:
			GICWRITE(GIC_REG(VPE_OTHER, GIC_VPE_SWINT0_MAP),
				 GIC_MAP_TO_PIN_MSK | intr);
			break;
		case GIC_FLAG_VPE_SWINT1:
			GICWRITE(GIC_REG(VPE_OTHER, GIC_VPE_SWINT1_MAP),
				 GIC_MAP_TO_PIN_MSK | intr);
			break;
		default:
			break;
		}
	}
}

unsigned int gic_compare_int(void)
{
	unsigned int pending;

	GICREAD(GIC_REG(VPE_LOCAL, GIC_VPE_PEND), pending);
	if (pending & GIC_VPE_PEND_CMP_MSK)
		return 1;
	else
		return 0;
}

unsigned int gic_get_int(void)
{
	unsigned int i;
	unsigned long *pending, *intrmask, *pcpu_mask;
	unsigned long *pending_abs, *intrmask_abs;

	/* Get per-cpu bitmaps */
	pending = pending_regs[smp_processor_id()].pending;
	intrmask = intrmask_regs[smp_processor_id()].intrmask;
	pcpu_mask = pcpu_masks[smp_processor_id()].pcpu_mask;

	pending_abs = (unsigned long *) GIC_REG_ABS_ADDR(SHARED,
							 GIC_SH_PEND_31_0_OFS);
	intrmask_abs = (unsigned long *) GIC_REG_ABS_ADDR(SHARED,
							  GIC_SH_MASK_31_0_OFS);

	for (i = 0; i < BITS_TO_LONGS(GIC_NUM_INTRS); i++) {
		GICREAD(*pending_abs, pending[i]);
		GICREAD(*intrmask_abs, intrmask[i]);
		pending_abs++;
		intrmask_abs++;
	}

	bitmap_and(pending, pending, intrmask, GIC_NUM_INTRS);
	bitmap_and(pending, pending, pcpu_mask, GIC_NUM_INTRS);

	i = find_first_bit(pending, GIC_NUM_INTRS);
	if (i == GIC_NUM_INTRS)
		return -1;

	pr_debug("CPU%d: %s pend=%d\n", smp_processor_id(), __func__, i);

	return i;
}

static void gic_mask_irq(struct irq_data *d)
{
	GIC_CLR_INTR_MASK(d->irq - gic_irq_base);
}

static void gic_unmask_irq(struct irq_data *d)
{
	GIC_SET_INTR_MASK(d->irq - gic_irq_base);
}

#ifdef CONFIG_SMP
static DEFINE_SPINLOCK(gic_lock);

static int gic_set_affinity(struct irq_data *d, const struct cpumask *cpumask,
			    bool force)
{
	unsigned int irq = (d->irq - gic_irq_base);
	cpumask_t	tmp = CPU_MASK_NONE;
	unsigned long	flags;
	int		i;

	cpumask_and(&tmp, cpumask, cpu_online_mask);
	if (cpus_empty(tmp))
		return -1;

	/* Assumption : cpumask refers to a single CPU */
	spin_lock_irqsave(&gic_lock, flags);

	/* Re-route this IRQ */
	GIC_SH_MAP_TO_VPE_SMASK(irq, first_cpu(tmp));

	/* Update the pcpu_masks */
	for (i = 0; i < NR_CPUS; i++)
		clear_bit(irq, pcpu_masks[i].pcpu_mask);
	set_bit(irq, pcpu_masks[first_cpu(tmp)].pcpu_mask);

	cpumask_copy(d->affinity, cpumask);
	spin_unlock_irqrestore(&gic_lock, flags);

	return IRQ_SET_MASK_OK_NOCOPY;
}
#endif

static struct irq_chip gic_irq_controller = {
	.name			=	"MIPS GIC",
	.irq_ack		=	gic_irq_ack,
	.irq_mask		=	gic_mask_irq,
	.irq_mask_ack		=	gic_mask_irq,
	.irq_unmask		=	gic_unmask_irq,
	.irq_eoi		=	gic_finish_irq,
#ifdef CONFIG_SMP
	.irq_set_affinity	=	gic_set_affinity,
#endif
};

static void __init gic_setup_intr(unsigned int intr, unsigned int cpu,
	unsigned int pin, unsigned int polarity, unsigned int trigtype,
	unsigned int flags)
{
	struct gic_shared_intr_map *map_ptr;

	/* Setup Intr to Pin mapping */
	if (pin & GIC_MAP_TO_NMI_MSK) {
		int i;

		GICWRITE(GIC_REG_ADDR(SHARED, GIC_SH_MAP_TO_PIN(intr)), pin);
		/* FIXME: hack to route NMI to all cpu's */
		for (i = 0; i < NR_CPUS; i += 32) {
			GICWRITE(GIC_REG_ADDR(SHARED,
					  GIC_SH_MAP_TO_VPE_REG_OFF(intr, i)),
				 0xffffffff);
		}
	} else {
		GICWRITE(GIC_REG_ADDR(SHARED, GIC_SH_MAP_TO_PIN(intr)),
			 GIC_MAP_TO_PIN_MSK | pin);
		/* Setup Intr to CPU mapping */
		GIC_SH_MAP_TO_VPE_SMASK(intr, cpu);
		if (cpu_has_veic) {
			set_vi_handler(pin + GIC_PIN_TO_VEC_OFFSET,
				gic_eic_irq_dispatch);
			map_ptr = &gic_shared_intr_map[pin + GIC_PIN_TO_VEC_OFFSET];
			if (map_ptr->num_shared_intr >= GIC_MAX_SHARED_INTR)
				BUG();
			map_ptr->intr_list[map_ptr->num_shared_intr++] = intr;
		}
	}

	/* Setup Intr Polarity */
	GIC_SET_POLARITY(intr, polarity);

	/* Setup Intr Trigger Type */
	GIC_SET_TRIGGER(intr, trigtype);

	/* Init Intr Masks */
	GIC_CLR_INTR_MASK(intr);
	/* Initialise per-cpu Interrupt software masks */
	if (flags & GIC_FLAG_IPI)
		set_bit(intr, pcpu_masks[cpu].pcpu_mask);
	if (trigtype == GIC_TRIG_EDGE)
		gic_irq_flags[intr] |= GIC_TRIG_EDGE;
}

/*
 * Setup IPI interrupts
 *
 * IPI interrupts are installed at the end of gic_intr_map
 * shown as follows:
 *
 * cpu0         resched		GIC_NUM_INTRS - NR_CPUS * 2
 * cpu1         resched
 * cpu2         resched
 * cpu3         resched
 * cpu0         call		GIC_NUM_INTRS - NR_CPUS
 * cpu1         call
 * cpu2         call
 * cpu3         call
 */

void __init gic_setup_ipi(unsigned int *ipimap, unsigned int resched,
			  unsigned int call)
{
	int cpu;

	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		gic_setup_intr(GIC_IPI_RESCHED(cpu), cpu, resched,
			       GIC_POL_POS, GIC_TRIG_EDGE, GIC_FLAG_IPI);

		gic_setup_intr(GIC_IPI_CALL(cpu), cpu, call,
			       GIC_POL_POS, GIC_TRIG_EDGE, GIC_FLAG_IPI);

		ipimap[cpu] |= (1 << (resched + 2));
		ipimap[cpu] |= (1 << (call + 2));
	}
}

static void __init gic_basic_init(int numintrs, int numvpes,
			struct gic_intr_map *intrmap)
{
	unsigned int i, cpu;
	unsigned int pin_offset = 0;

	board_bind_eic_interrupt = &gic_bind_eic_interrupt;

	/* Setup defaults */
	for (i = 0; i < numintrs; i++) {

#ifdef CONFIG_RTL_8198C
		if(i<50)
			GIC_SET_POLARITY(i, GIC_POL_POS);
		else                    
			GIC_SET_POLARITY(i, GIC_POL_NEG);
#else
		GIC_SET_POLARITY(i, GIC_POL_POS);
#endif
		
		GIC_SET_TRIGGER(i, GIC_TRIG_LEVEL);
		GIC_CLR_INTR_MASK(i);
		if (i < GIC_NUM_INTRS) {
			gic_irq_flags[i] = 0;
			gic_shared_intr_map[i].num_shared_intr = 0;
			gic_shared_intr_map[i].local_intr_mask = 0;
		}
	}

	/*
	 * In EIC mode, the HW_INT# is offset by (2-1). Need to subtract
	 * one because the GIC will add one (since 0=no intr).
	 */
	if (cpu_has_veic)
		pin_offset = (GIC_CPU_TO_VEC_OFFSET - GIC_PIN_TO_VEC_OFFSET);

	/* Setup specifics */
	for (i = 0; i < GIC_NUM_INTRS; i++) {
		cpu = intrmap[i].cpunum;

		if (intrmap[i].flags & GIC_FLAG_VPE_MASK) {
			vpe_local_setup(numvpes, intrmap[i].flags,
					intrmap[i].pin);
			continue;
		}

		irq_set_chip(gic_irq_base + i, &gic_irq_controller);

		if (cpu == GIC_UNUSED)
			continue;
		if (cpu == 0 && i != 0 && intrmap[i].flags == 0)
			continue;

		gic_setup_intr(i,
			intrmap[i].cpunum,
			intrmap[i].pin + pin_offset,
			intrmap[i].polarity,
			intrmap[i].trigtype,
			intrmap[i].flags);

		irq_set_handler(gic_irq_base + i, handle_level_irq);
	}

#ifdef CONFIG_SMP
	/*
	 * In case of SMP, interrupts are routed via GIC,
	 * unmask all interrupts by default, and let GIC
	 * do the routing
	 */
	change_c0_status(ST0_IM, 0xff00);
#endif
}

void __init gic_init(unsigned long gic_base_addr, unsigned long gic_base_size,
		     struct gic_intr_map *intrmap, unsigned int irqbase)
{
	unsigned int gicconfig;
	int numvpes, numintrs;

	_gic_base = (unsigned long) ioremap_nocache(gic_base_addr,
						    gic_base_size);
	gic_irq_base = irqbase;

	GCMPGCB(GICBA) = gic_base_addr | GCMP_GCB_GICBA_EN_MSK;
	GICREAD(GIC_REG(SHARED, GIC_SH_CONFIG), gicconfig);
	numintrs = (gicconfig & GIC_SH_CONFIG_NUMINTRS_MSK) >>
		   GIC_SH_CONFIG_NUMINTRS_SHF;
	numintrs = ((numintrs + 1) * 8);

	numvpes = (gicconfig & GIC_SH_CONFIG_NUMVPES_MSK) >>
		  GIC_SH_CONFIG_NUMVPES_SHF;
	numvpes = numvpes + 1;

#ifdef CONFIG_RTL_8198C
	{
	extern void gic_cascade_init(void);
	gic_cascade_init();
	}
#endif
	gic_basic_init(numintrs, numvpes, intrmap);
}
