/*
 * More than one processor on generic (non-Apple) ARMv8 boards: see psci_smp.c.
 */
#ifndef _ARM64_PSCI_SMP_H_
#define _ARM64_PSCI_SMP_H_

#include <mach/mach_types.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* How far a processor other than the boot processor has come (psci_smp_cpu_reached()). */
#define PSCI_SMP_CPU_ASKED      1       /* the firmware has agreed to start it */
#define PSCI_SMP_CPU_IN_KERNEL  2       /* it runs C code on the kernel's page tables (arm_init_cpu()) */
#define PSCI_SMP_CPU_RUNNING    3       /* it has joined the scheduler (psci_smp_cpu_running()) */
#define PSCI_SMP_CPU_OFF        4       /* it has switched itself off, or is about to */

extern void             psci_smp_init(void);
extern kern_return_t    psci_smp_start_cpu(int cpu);
extern void             psci_smp_cpu_reached(uint32_t stage);
extern void             psci_smp_cpu_running(void);
extern void             psci_smp_cpu_off(void) __dead2;

extern void             psci_smp_stop_others(void);
extern void             psci_smp_stop_here(void);
extern int              psci_smp_report(char *buf, size_t size);

/* osfmk/arm64/gic_v3_irq.c */
#define GIC_SIGNAL_KINDS        13      /* SIGP bits 0..11, then the signals with none (SIGPnop) */
#define GIC_PROF_TABLE          2048    /* slots of one processor's table (gic_v3_irq.c) */
#define GIC_PROF_USER_MASK      0x0000007fffffffffULL   /* a user pc or lr without its signature */
struct gic_prof_slot {
	uint64_t pc, lr;
	uint64_t up[2];                 /* a kernel sample's next two return addresses up the frame chain */
	int32_t pid;                    /* 0: a kernel thread, -2: a processor's idle thread */
	uint16_t user;
	uint16_t pad;
	uint32_t count;
};
extern void             gic_v3_prof_control(bool on, int only_pid);
extern void             gic_v3_prof_table(int cpu, struct gic_prof_slot *out, uint32_t *lost);
extern int              psci_smp_prof_report(char *buf, size_t size);
extern void             gic_v3_signal_kinds(int cpu, uint32_t kinds[GIC_SIGNAL_KINDS]);
extern void             gic_v3_irq_signal(uint32_t phys_id);
extern void             gic_v3_irq_signal_stop(uint32_t phys_id);
extern void             gic_v3_irq_quiesce(void);
extern void             gic_v3_irq_cpu_shutdown(void);

__END_DECLS

#endif /* _ARM64_PSCI_SMP_H_ */
