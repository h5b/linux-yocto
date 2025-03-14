/*
 *  Kernel Probes (KProbes)
 *  arch/i386/kernel/kprobes.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) IBM Corporation, 2002, 2004
 *
 * 2002-Oct	Created by Vamsi Krishna S <vamsi_krishna@in.ibm.com> Kernel
 *		Probes initial implementation ( includes contributions from
 *		Rusty Russell).
 * 2004-July	Suparna Bhattacharya <suparna@in.ibm.com> added jumper probes
 *		interface to access function arguments.
 * 2005-May	Hien Nguyen <hien@us.ibm.com>, Jim Keniston
 *		<jkenisto@us.ibm.com> and Prasanna S Panchamukhi
 *		<prasanna@in.ibm.com> added function-return probes.
 */

#include <linux/config.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/preempt.h>
#include <asm/cacheflush.h>
#include <asm/kdebug.h>
#include <asm/desc.h>

void jprobe_return_end(void);

DEFINE_PER_CPU(struct kprobe *, current_kprobe) = NULL;
DEFINE_PER_CPU(struct kprobe_ctlblk, kprobe_ctlblk);

/*
 * returns non-zero if opcode modifies the interrupt flag.
 */
static inline int is_IF_modifier(kprobe_opcode_t opcode)
{
	switch (opcode) {
	case 0xfa:		/* cli */
	case 0xfb:		/* sti */
	case 0xcf:		/* iret/iretd */
	case 0x9d:		/* popf/popfd */
		return 1;
	}
	return 0;
}

int __kprobes arch_prepare_kprobe(struct kprobe *p)
{
	memcpy(p->ainsn.insn, p->addr, MAX_INSN_SIZE * sizeof(kprobe_opcode_t));
	p->opcode = *p->addr;
	return 0;
}

void __kprobes arch_arm_kprobe(struct kprobe *p)
{
	*p->addr = BREAKPOINT_INSTRUCTION;
	flush_icache_range((unsigned long) p->addr,
			   (unsigned long) p->addr + sizeof(kprobe_opcode_t));
}

void __kprobes arch_disarm_kprobe(struct kprobe *p)
{
	*p->addr = p->opcode;
	flush_icache_range((unsigned long) p->addr,
			   (unsigned long) p->addr + sizeof(kprobe_opcode_t));
}

static inline void save_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	kcb->prev_kprobe.kp = kprobe_running();
	kcb->prev_kprobe.status = kcb->kprobe_status;
	kcb->prev_kprobe.old_eflags = kcb->kprobe_old_eflags;
	kcb->prev_kprobe.saved_eflags = kcb->kprobe_saved_eflags;
}

static inline void restore_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	__get_cpu_var(current_kprobe) = kcb->prev_kprobe.kp;
	kcb->kprobe_status = kcb->prev_kprobe.status;
	kcb->kprobe_old_eflags = kcb->prev_kprobe.old_eflags;
	kcb->kprobe_saved_eflags = kcb->prev_kprobe.saved_eflags;
}

static inline void set_current_kprobe(struct kprobe *p, struct pt_regs *regs,
				struct kprobe_ctlblk *kcb)
{
	__get_cpu_var(current_kprobe) = p;
	kcb->kprobe_saved_eflags = kcb->kprobe_old_eflags
		= (regs->eflags & (TF_MASK | IF_MASK));
	if (is_IF_modifier(p->opcode))
		kcb->kprobe_saved_eflags &= ~IF_MASK;
}

static inline void prepare_singlestep(struct kprobe *p, struct pt_regs *regs)
{
	regs->eflags |= TF_MASK;
	regs->eflags &= ~IF_MASK;
	/*single step inline if the instruction is an int3*/
	if (p->opcode == BREAKPOINT_INSTRUCTION)
		regs->eip = (unsigned long)p->addr;
	else
		regs->eip = (unsigned long)&p->ainsn.insn;
}

/* Called with kretprobe_lock held */
void __kprobes arch_prepare_kretprobe(struct kretprobe *rp,
				      struct pt_regs *regs)
{
	unsigned long *sara = (unsigned long *)&regs->esp;
        struct kretprobe_instance *ri;

        if ((ri = get_free_rp_inst(rp)) != NULL) {
                ri->rp = rp;
                ri->task = current;
		ri->ret_addr = (kprobe_opcode_t *) *sara;

		/* Replace the return addr with trampoline addr */
		*sara = (unsigned long) &kretprobe_trampoline;

                add_rp_inst(ri);
        } else {
                rp->nmissed++;
        }
}

/*
 * Interrupts are disabled on entry as trap3 is an interrupt gate and they
 * remain disabled thorough out this function.
 */
static int __kprobes kprobe_handler(struct pt_regs *regs)
{
	struct kprobe *p;
	int ret = 0;
	kprobe_opcode_t *addr = NULL;
	unsigned long *lp;
	struct kprobe_ctlblk *kcb;

	/*
	 * We don't want to be preempted for the entire
	 * duration of kprobe processing
	 */
	preempt_disable();
	kcb = get_kprobe_ctlblk();

	/* Check if the application is using LDT entry for its code segment and
	 * calculate the address by reading the base address from the LDT entry.
	 */
	if ((regs->xcs & 4) && (current->mm)) {
		lp = (unsigned long *) ((unsigned long)((regs->xcs >> 3) * 8)
					+ (char *) current->mm->context.ldt);
		addr = (kprobe_opcode_t *) (get_desc_base(lp) + regs->eip -
						sizeof(kprobe_opcode_t));
	} else {
		addr = (kprobe_opcode_t *)(regs->eip - sizeof(kprobe_opcode_t));
	}
	/* Check we're not actually recursing */
	if (kprobe_running()) {
		p = get_kprobe(addr);
		if (p) {
			if (kcb->kprobe_status == KPROBE_HIT_SS &&
				*p->ainsn.insn == BREAKPOINT_INSTRUCTION) {
				regs->eflags &= ~TF_MASK;
				regs->eflags |= kcb->kprobe_saved_eflags;
				goto no_kprobe;
			}
			/* We have reentered the kprobe_handler(), since
			 * another probe was hit while within the handler.
			 * We here save the original kprobes variables and
			 * just single step on the instruction of the new probe
			 * without calling any user handlers.
			 */
			save_previous_kprobe(kcb);
			set_current_kprobe(p, regs, kcb);
			kprobes_inc_nmissed_count(p);
			prepare_singlestep(p, regs);
			kcb->kprobe_status = KPROBE_REENTER;
			return 1;
		} else {
			if (regs->eflags & VM_MASK) {
			/* We are in virtual-8086 mode. Return 0 */
				goto no_kprobe;
			}
			if (*addr != BREAKPOINT_INSTRUCTION) {
			/* The breakpoint instruction was removed by
			 * another cpu right after we hit, no further
			 * handling of this interrupt is appropriate
			 */
				regs->eip -= sizeof(kprobe_opcode_t);
				ret = 1;
				goto no_kprobe;
			}
			p = __get_cpu_var(current_kprobe);
			if (p->break_handler && p->break_handler(p, regs)) {
				goto ss_probe;
			}
		}
		goto no_kprobe;
	}

	p = get_kprobe(addr);
	if (!p) {
		if (regs->eflags & VM_MASK) {
			/* We are in virtual-8086 mode. Return 0 */
			goto no_kprobe;
		}

		if (*addr != BREAKPOINT_INSTRUCTION) {
			/*
			 * The breakpoint instruction was removed right
			 * after we hit it.  Another cpu has removed
			 * either a probepoint or a debugger breakpoint
			 * at this address.  In either case, no further
			 * handling of this interrupt is appropriate.
			 * Back up over the (now missing) int3 and run
			 * the original instruction.
			 */
			regs->eip -= sizeof(kprobe_opcode_t);
			ret = 1;
		}
		/* Not one of ours: let kernel handle it */
		goto no_kprobe;
	}

	set_current_kprobe(p, regs, kcb);
	kcb->kprobe_status = KPROBE_HIT_ACTIVE;

	if (p->pre_handler && p->pre_handler(p, regs))
		/* handler has already set things up, so skip ss setup */
		return 1;

ss_probe:
	prepare_singlestep(p, regs);
	kcb->kprobe_status = KPROBE_HIT_SS;
	return 1;

no_kprobe:
	preempt_enable_no_resched();
	return ret;
}

/*
 * For function-return probes, init_kprobes() establishes a probepoint
 * here. When a retprobed function returns, this probe is hit and
 * trampoline_probe_handler() runs, calling the kretprobe's handler.
 */
 void kretprobe_trampoline_holder(void)
 {
 	asm volatile (  ".global kretprobe_trampoline\n"
 			"kretprobe_trampoline: \n"
 			"nop\n");
 }

/*
 * Called when we hit the probe point at kretprobe_trampoline
 */
int __kprobes trampoline_probe_handler(struct kprobe *p, struct pt_regs *regs)
{
        struct kretprobe_instance *ri = NULL;
        struct hlist_head *head;
        struct hlist_node *node, *tmp;
	unsigned long flags, orig_ret_address = 0;
	unsigned long trampoline_address =(unsigned long)&kretprobe_trampoline;

	spin_lock_irqsave(&kretprobe_lock, flags);
        head = kretprobe_inst_table_head(current);

	/*
	 * It is possible to have multiple instances associated with a given
	 * task either because an multiple functions in the call path
	 * have a return probe installed on them, and/or more then one return
	 * return probe was registered for a target function.
	 *
	 * We can handle this because:
	 *     - instances are always inserted at the head of the list
	 *     - when multiple return probes are registered for the same
         *       function, the first instance's ret_addr will point to the
	 *       real return address, and all the rest will point to
	 *       kretprobe_trampoline
	 */
	hlist_for_each_entry_safe(ri, node, tmp, head, hlist) {
                if (ri->task != current)
			/* another task is sharing our hash bucket */
                        continue;

		if (ri->rp && ri->rp->handler)
			ri->rp->handler(ri, regs);

		orig_ret_address = (unsigned long)ri->ret_addr;
		recycle_rp_inst(ri);

		if (orig_ret_address != trampoline_address)
			/*
			 * This is the real return address. Any other
			 * instances associated with this task are for
			 * other calls deeper on the call stack
			 */
			break;
	}

	BUG_ON(!orig_ret_address || (orig_ret_address == trampoline_address));
	regs->eip = orig_ret_address;

	reset_current_kprobe();
	spin_unlock_irqrestore(&kretprobe_lock, flags);
	preempt_enable_no_resched();

	/*
	 * By returning a non-zero value, we are telling
	 * kprobe_handler() that we don't want the post_handler
	 * to run (and have re-enabled preemption)
	 */
        return 1;
}

/*
 * Called after single-stepping.  p->addr is the address of the
 * instruction whose first byte has been replaced by the "int 3"
 * instruction.  To avoid the SMP problems that can occur when we
 * temporarily put back the original opcode to single-step, we
 * single-stepped a copy of the instruction.  The address of this
 * copy is p->ainsn.insn.
 *
 * This function prepares to return from the post-single-step
 * interrupt.  We have to fix up the stack as follows:
 *
 * 0) Except in the case of absolute or indirect jump or call instructions,
 * the new eip is relative to the copied instruction.  We need to make
 * it relative to the original instruction.
 *
 * 1) If the single-stepped instruction was pushfl, then the TF and IF
 * flags are set in the just-pushed eflags, and may need to be cleared.
 *
 * 2) If the single-stepped instruction was a call, the return address
 * that is atop the stack is the address following the copied instruction.
 * We need to make it the address following the original instruction.
 */
static void __kprobes resume_execution(struct kprobe *p,
		struct pt_regs *regs, struct kprobe_ctlblk *kcb)
{
	unsigned long *tos = (unsigned long *)&regs->esp;
	unsigned long next_eip = 0;
	unsigned long copy_eip = (unsigned long)&p->ainsn.insn;
	unsigned long orig_eip = (unsigned long)p->addr;

	switch (p->ainsn.insn[0]) {
	case 0x9c:		/* pushfl */
		*tos &= ~(TF_MASK | IF_MASK);
		*tos |= kcb->kprobe_old_eflags;
		break;
	case 0xc3:		/* ret/lret */
	case 0xcb:
	case 0xc2:
	case 0xca:
		regs->eflags &= ~TF_MASK;
		/* eip is already adjusted, no more changes required*/
		return;
	case 0xe8:		/* call relative - Fix return addr */
		*tos = orig_eip + (*tos - copy_eip);
		break;
	case 0xff:
		if ((p->ainsn.insn[1] & 0x30) == 0x10) {
			/* call absolute, indirect */
			/* Fix return addr; eip is correct. */
			next_eip = regs->eip;
			*tos = orig_eip + (*tos - copy_eip);
		} else if (((p->ainsn.insn[1] & 0x31) == 0x20) ||	/* jmp near, absolute indirect */
			   ((p->ainsn.insn[1] & 0x31) == 0x21)) {	/* jmp far, absolute indirect */
			/* eip is correct. */
			next_eip = regs->eip;
		}
		break;
	case 0xea:		/* jmp absolute -- eip is correct */
		next_eip = regs->eip;
		break;
	default:
		break;
	}

	regs->eflags &= ~TF_MASK;
	if (next_eip) {
		regs->eip = next_eip;
	} else {
		regs->eip = orig_eip + (regs->eip - copy_eip);
	}
}

/*
 * Interrupts are disabled on entry as trap1 is an interrupt gate and they
 * remain disabled thoroughout this function.
 */
static inline int post_kprobe_handler(struct pt_regs *regs)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	if (!cur)
		return 0;

	if ((kcb->kprobe_status != KPROBE_REENTER) && cur->post_handler) {
		kcb->kprobe_status = KPROBE_HIT_SSDONE;
		cur->post_handler(cur, regs, 0);
	}

	resume_execution(cur, regs, kcb);
	regs->eflags |= kcb->kprobe_saved_eflags;

	/*Restore back the original saved kprobes variables and continue. */
	if (kcb->kprobe_status == KPROBE_REENTER) {
		restore_previous_kprobe(kcb);
		goto out;
	}
	reset_current_kprobe();
out:
	preempt_enable_no_resched();

	/*
	 * if somebody else is singlestepping across a probe point, eflags
	 * will have TF set, in which case, continue the remaining processing
	 * of do_debug, as if this is not a probe hit.
	 */
	if (regs->eflags & TF_MASK)
		return 0;

	return 1;
}

static inline int kprobe_fault_handler(struct pt_regs *regs, int trapnr)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	if (cur->fault_handler && cur->fault_handler(cur, regs, trapnr))
		return 1;

	if (kcb->kprobe_status & KPROBE_HIT_SS) {
		resume_execution(cur, regs, kcb);
		regs->eflags |= kcb->kprobe_old_eflags;

		reset_current_kprobe();
		preempt_enable_no_resched();
	}
	return 0;
}

/*
 * Wrapper routine to for handling exceptions.
 */
int __kprobes kprobe_exceptions_notify(struct notifier_block *self,
				       unsigned long val, void *data)
{
	struct die_args *args = (struct die_args *)data;
	int ret = NOTIFY_DONE;

	switch (val) {
	case DIE_INT3:
		if (kprobe_handler(args->regs))
			ret = NOTIFY_STOP;
		break;
	case DIE_DEBUG:
		if (post_kprobe_handler(args->regs))
			ret = NOTIFY_STOP;
		break;
	case DIE_GPF:
	case DIE_PAGE_FAULT:
		/* kprobe_running() needs smp_processor_id() */
		preempt_disable();
		if (kprobe_running() &&
		    kprobe_fault_handler(args->regs, args->trapnr))
			ret = NOTIFY_STOP;
		preempt_enable();
		break;
	default:
		break;
	}
	return ret;
}

int __kprobes setjmp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct jprobe *jp = container_of(p, struct jprobe, kp);
	unsigned long addr;
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	kcb->jprobe_saved_regs = *regs;
	kcb->jprobe_saved_esp = &regs->esp;
	addr = (unsigned long)(kcb->jprobe_saved_esp);

	/*
	 * TBD: As Linus pointed out, gcc assumes that the callee
	 * owns the argument space and could overwrite it, e.g.
	 * tailcall optimization. So, to be absolutely safe
	 * we also save and restore enough stack bytes to cover
	 * the argument area.
	 */
	memcpy(kcb->jprobes_stack, (kprobe_opcode_t *)addr,
			MIN_STACK_SIZE(addr));
	regs->eflags &= ~IF_MASK;
	regs->eip = (unsigned long)(jp->entry);
	return 1;
}

void __kprobes jprobe_return(void)
{
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	asm volatile ("       xchgl   %%ebx,%%esp     \n"
		      "       int3			\n"
		      "       .globl jprobe_return_end	\n"
		      "       jprobe_return_end:	\n"
		      "       nop			\n"::"b"
		      (kcb->jprobe_saved_esp):"memory");
}

int __kprobes longjmp_break_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();
	u8 *addr = (u8 *) (regs->eip - 1);
	unsigned long stack_addr = (unsigned long)(kcb->jprobe_saved_esp);
	struct jprobe *jp = container_of(p, struct jprobe, kp);

	if ((addr > (u8 *) jprobe_return) && (addr < (u8 *) jprobe_return_end)) {
		if (&regs->esp != kcb->jprobe_saved_esp) {
			struct pt_regs *saved_regs =
			    container_of(kcb->jprobe_saved_esp,
					    struct pt_regs, esp);
			printk("current esp %p does not match saved esp %p\n",
			       &regs->esp, kcb->jprobe_saved_esp);
			printk("Saved registers for jprobe %p\n", jp);
			show_registers(saved_regs);
			printk("Current registers\n");
			show_registers(regs);
			BUG();
		}
		*regs = kcb->jprobe_saved_regs;
		memcpy((kprobe_opcode_t *) stack_addr, kcb->jprobes_stack,
		       MIN_STACK_SIZE(stack_addr));
		preempt_enable_no_resched();
		return 1;
	}
	return 0;
}

static struct kprobe trampoline_p = {
	.addr = (kprobe_opcode_t *) &kretprobe_trampoline,
	.pre_handler = trampoline_probe_handler
};

int __init arch_init_kprobes(void)
{
	return register_kprobe(&trampoline_p);
}
