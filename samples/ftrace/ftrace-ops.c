// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt)       KBUILD_MODNAME ": " fmt

#include <linux/ftrace.h>
#include <linux/ktime.h>
#include <linux/module.h>

#include <asm/barrier.h>

/*
 * Arbitrary large value chosen to be sufficiently large to minimize noise but
 * sufficiently small to complete quickly.
 */
static unsigned int nr_function_calls = 100000;
module_param(nr_function_calls, uint, 0);
MODULE_PARM_DESC(nr_function_calls, "How many times to call the relevant tracee");

/*
 * The number of ops associated with a call site affects whether a tracer can
 * be called directly or whether it's necessary to go via the list func, which
 * can be significantly more expensive.
 */
static unsigned int nr_ops_relevant = 1;
module_param(nr_ops_relevant, uint, 0);
MODULE_PARM_DESC(nr_ops_relevant, "How many ftrace_ops to associate with the relevant tracee");

/*
 * On architectures where all call sites share the same trampoline, having
 * tracers enabled for distinct functions can force the use of the list func
 * and incur overhead for all call sites.
 */
static unsigned int nr_ops_irrelevant;
module_param(nr_ops_irrelevant, uint, 0);
MODULE_PARM_DESC(nr_ops_irrelevant, "How many ftrace_ops to associate with the irrelevant tracee");

/*
 * On architectures with DYNAMIC_FTRACE_WITH_REGS, saving the full pt_regs can
 * be more expensive than only saving the minimal necessary regs.
 */
static bool save_regs;
module_param(save_regs, bool, 0);
MODULE_PARM_DESC(save_regs, "Register ops with FTRACE_OPS_FL_SAVE_REGS (save all registers in the trampoline)");

static bool assist_recursion;
module_param(assist_recursion, bool, 0);
MODULE_PARM_DESC(assist_reursion, "Register ops with FTRACE_OPS_FL_RECURSION");

static bool assist_rcu;
module_param(assist_rcu, bool, 0);
MODULE_PARM_DESC(assist_reursion, "Register ops with FTRACE_OPS_FL_RCU");

/*
 * By default, a trivial tracer is used which immediately returns to mimimize
 * overhead. Sometimes a consistency check using a more expensive tracer is
 * desireable.
 */
static bool check_count;
module_param(check_count, bool, 0);
MODULE_PARM_DESC(check_count, "Check that tracers are called the expected number of times\n");

/*
 * Usually it's not interesting to leave the ops registered after the test
 * runs, but sometimes it can be useful to leave them registered so that they
 * can be inspected through the tracefs 'enabled_functions' file.
 */
static bool persist;
module_param(persist, bool, 0);
MODULE_PARM_DESC(persist, "Successfully load module and leave ftrace ops registered after test completes\n");

/*
 * Marked as noinline to ensure that an out-of-line traceable copy is
 * generated by the compiler.
 *
 * The barrier() ensures the compiler won't elide calls by determining there
 * are no side-effects.
 */
static noinline void tracee_relevant(void)
{
	barrier();
}

/*
 * Marked as noinline to ensure that an out-of-line traceable copy is
 * generated by the compiler.
 *
 * The barrier() ensures the compiler won't elide calls by determining there
 * are no side-effects.
 */
static noinline void tracee_irrelevant(void)
{
	barrier();
}

struct sample_ops {
	struct ftrace_ops ops;
	unsigned int count;
};

static void ops_func_nop(unsigned long ip, unsigned long parent_ip,
			 struct ftrace_ops *op,
			 struct ftrace_regs *fregs)
{
	/* do nothing */
}

static void ops_func_count(unsigned long ip, unsigned long parent_ip,
			   struct ftrace_ops *op,
			   struct ftrace_regs *fregs)
{
	struct sample_ops *self;

	self = container_of(op, struct sample_ops, ops);
	self->count++;
}

static struct sample_ops *ops_relevant;
static struct sample_ops *ops_irrelevant;

static struct sample_ops *ops_alloc_init(void *tracee, ftrace_func_t func,
					 unsigned long flags, int nr)
{
	struct sample_ops *ops;

	ops = kcalloc(nr, sizeof(*ops), GFP_KERNEL);
	if (WARN_ON_ONCE(!ops))
		return NULL;

	for (unsigned int i = 0; i < nr; i++) {
		ops[i].ops.func = func;
		ops[i].ops.flags = flags;
		WARN_ON_ONCE(ftrace_set_filter_ip(&ops[i].ops, (unsigned long)tracee, 0, 0));
		WARN_ON_ONCE(register_ftrace_function(&ops[i].ops));
	}

	return ops;
}

static void ops_destroy(struct sample_ops *ops, int nr)
{
	if (!ops)
		return;

	for (unsigned int i = 0; i < nr; i++) {
		WARN_ON_ONCE(unregister_ftrace_function(&ops[i].ops));
		ftrace_free_filter(&ops[i].ops);
	}

	kfree(ops);
}

static void ops_check(struct sample_ops *ops, int nr,
		      unsigned int expected_count)
{
	if (!ops || !check_count)
		return;

	for (unsigned int i = 0; i < nr; i++) {
		if (ops->count == expected_count)
			continue;
		pr_warn("Counter called %u times (expected %u)\n",
			ops->count, expected_count);
	}
}

static ftrace_func_t tracer_relevant = ops_func_nop;
static ftrace_func_t tracer_irrelevant = ops_func_nop;

static int __init ftrace_ops_sample_init(void)
{
	unsigned long flags = 0;
	ktime_t start, end;
	u64 period;

	if (!IS_ENABLED(CONFIG_DYNAMIC_FTRACE_WITH_REGS) && save_regs) {
		pr_info("this kernel does not support saving registers\n");
		save_regs = false;
	} else if (save_regs) {
		flags |= FTRACE_OPS_FL_SAVE_REGS;
	}

	if (assist_recursion)
		flags |= FTRACE_OPS_FL_RECURSION;

	if (assist_rcu)
		flags |= FTRACE_OPS_FL_RCU;

	if (check_count) {
		tracer_relevant = ops_func_count;
		tracer_irrelevant = ops_func_count;
	}

	pr_info("registering:\n"
		"  relevant ops: %u\n"
		"    tracee: %ps\n"
		"    tracer: %ps\n"
		"  irrelevant ops: %u\n"
		"    tracee: %ps\n"
		"    tracer: %ps\n"
		"  saving registers: %s\n"
		"  assist recursion: %s\n"
		"  assist RCU: %s\n",
		nr_ops_relevant, tracee_relevant, tracer_relevant,
		nr_ops_irrelevant, tracee_irrelevant, tracer_irrelevant,
		save_regs ? "YES" : "NO",
		assist_recursion ? "YES" : "NO",
		assist_rcu ? "YES" : "NO");

	ops_relevant = ops_alloc_init(tracee_relevant, tracer_relevant,
				      flags, nr_ops_relevant);
	ops_irrelevant = ops_alloc_init(tracee_irrelevant, tracer_irrelevant,
					flags, nr_ops_irrelevant);

	start = ktime_get();
	for (unsigned int i = 0; i < nr_function_calls; i++)
		tracee_relevant();
	end = ktime_get();

	ops_check(ops_relevant, nr_ops_relevant, nr_function_calls);
	ops_check(ops_irrelevant, nr_ops_irrelevant, 0);

	period = ktime_to_ns(ktime_sub(end, start));

	pr_info("Attempted %u calls to %ps in %lluns (%lluns / call)\n",
		nr_function_calls, tracee_relevant,
		period, div_u64(period, nr_function_calls));

	if (persist)
		return 0;

	ops_destroy(ops_relevant, nr_ops_relevant);
	ops_destroy(ops_irrelevant, nr_ops_irrelevant);

	/*
	 * The benchmark completed sucessfully, but there's no reason to keep
	 * the module around. Return an error do the user doesn't have to
	 * manually unload the module.
	 */
	return -EINVAL;
}
module_init(ftrace_ops_sample_init);

static void __exit ftrace_ops_sample_exit(void)
{
	ops_destroy(ops_relevant, nr_ops_relevant);
	ops_destroy(ops_irrelevant, nr_ops_irrelevant);
}
module_exit(ftrace_ops_sample_exit);

MODULE_AUTHOR("Mark Rutland");
MODULE_DESCRIPTION("Example of using custom ftrace_ops");
MODULE_LICENSE("GPL");