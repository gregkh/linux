SCHED_EXT EXAMPLE SCHEDULERS
============================

# Introduction

This directory contains a number of example sched_ext schedulers. These
schedulers are meant to provide examples of different types of schedulers
that can be built using sched_ext, and illustrate how various features of
sched_ext can be used.

Some of the examples are performant, production-ready schedulers. That is, for
the correct workload and with the correct tuning, they may be deployed in a
production environment with acceptable or possibly even improved performance.
Others are just examples that in practice, would not provide acceptable
performance (though they could be improved to get there).

This README will describe these example schedulers, including describing the
types of workloads or scenarios they're designed to accommodate, and whether or
not they're production ready. For more details on any of these schedulers,
please see the header comment in their .bpf.c file.


# Compiling the examples

There are a few toolchain dependencies for compiling the example schedulers.

## Toolchain dependencies

1. clang >= 16.0.0

The schedulers are BPF programs, and therefore must be compiled with clang. gcc
is actively working on adding a BPF backend compiler as well, but are still
missing some features such as BTF type tags which are necessary for using
kptrs.

2. pahole >= 1.25

You may need pahole in order to generate BTF from DWARF.

3. rust >= 1.70.0

Rust schedulers uses features present in the rust toolchain >= 1.70.0. You
should be able to use the stable build from rustup, but if that doesn't
work, try using the rustup nightly build.

There are other requirements as well, such as make, but these are the main /
non-trivial ones.

## Compiling the kernel

In order to run a sched_ext scheduler, you'll have to run a kernel compiled
with the patches in this repository, and with a minimum set of necessary
Kconfig options:

```
CONFIG_BPF=y
CONFIG_SCHED_CLASS_EXT=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_DEBUG_INFO_BTF=y
```

It's also recommended that you also include the following Kconfig options:

```
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_JIT_DEFAULT_ON=y
CONFIG_PAHOLE_HAS_SPLIT_BTF=y
CONFIG_PAHOLE_HAS_BTF_TAG=y
```

There is a `Kconfig` file in this directory whose contents you can append to
your local `.config` file, as long as there are no conflicts with any existing
options in the file.

## Getting a vmlinux.h file

You may notice that most of the example schedulers include a "vmlinux.h" file.
This is a large, auto-generated header file that contains all of the types
defined in some vmlinux binary that was compiled with
[BTF](https://docs.kernel.org/bpf/btf.html) (i.e. with the BTF-related Kconfig
options specified above).

The header file is created using `bpftool`, by passing it a vmlinux binary
compiled with BTF as follows:

```bash
$ bpftool btf dump file /path/to/vmlinux format c > vmlinux.h
```

`bpftool` analyzes all of the BTF encodings in the binary, and produces a
header file that can be included by BPF programs to access those types.  For
example, using vmlinux.h allows a scheduler to access fields defined directly
in vmlinux as follows:

```c
#include "vmlinux.h"
// vmlinux.h is also implicitly included by scx_common.bpf.h.
#include "scx_common.bpf.h"

/*
 * vmlinux.h provides definitions for struct task_struct and
 * struct scx_enable_args.
 */
void BPF_STRUCT_OPS(example_enable, struct task_struct *p,
		    struct scx_enable_args *args)
{
	bpf_printk("Task %s enabled in example scheduler", p->comm);
}

// vmlinux.h provides the definition for struct sched_ext_ops.
SEC(".struct_ops.link")
struct sched_ext_ops example_ops {
	.enable	= (void *)example_enable,			
	.name	= "example",
}
```

The scheduler build system will generate this vmlinux.h file as part of the
scheduler build pipeline. It looks for a vmlinux file in the following
dependency order:

1. If the O= environment variable is defined, at `$O/vmlinux`
2. If the KBUILD_OUTPUT= environment variable is defined, at
   `$KBUILD_OUTPUT/vmlinux`
3. At `../../vmlinux` (i.e. at the root of the kernel tree where you're
   compiling the schedulers)
3. `/sys/kernel/btf/vmlinux`
4. `/boot/vmlinux-$(uname -r)`

In other words, if you have compiled a kernel in your local repo, its vmlinux
file will be used to generate vmlinux.h. Otherwise, it will be the vmlinux of
the kernel you're currently running on. This means that if you're running on a
kernel with sched_ext support, you may not need to compile a local kernel at
all.

### Aside on CO-RE

One of the cooler features of BPF is that it supports
[CO-RE](https://nakryiko.com/posts/bpf-core-reference-guide/) (Compile Once Run
Everywhere). This feature allows you to reference fields inside of structs with
types defined internal to the kernel, and not have to recompile if you load the
BPF program on a different kernel with the field at a different offset. In our
example above, we print out a task name with `p->comm`. CO-RE would perform
relocations for that access when the program is loaded to ensure that it's
referencing the correct offset for the currently running kernel.

## Compiling the schedulers

Once you have your toolchain setup, and a vmlinux that can be used to generate
a full vmlinux.h file, you can compile the schedulers using `make`:

```bash
$ make -j($nproc)
```

# Schedulers

This section lists, in alphabetical order, all of the current example
schedulers.

--------------------------------------------------------------------------------

## scx_simple

### Overview

A simple scheduler that provides an example of a minimal sched_ext
scheduler. scx_simple can be run in either global weighted vtime mode, or
FIFO mode.

### Typical Use Case

Though very simple, this scheduler should perform reasonably well on
single-socket CPUs with a uniform L3 cache topology. Note that while running in
global FIFO mode may work well for some workloads, saturating threads can
easily drown out inactive ones.

### Production Ready?

This scheduler could be used in a production environment, assuming the hardware
constraints enumerated above, and assuming the workload can accommodate a
simple scheduling policy.

--------------------------------------------------------------------------------

## scx_qmap

### Overview

Another simple, yet slightly more complex scheduler that provides an example of
a basic weighted FIFO queuing policy. It also provides examples of some common
useful BPF features, such as sleepable per-task storage allocation in the
`ops.prep_enable()` callback, and using the `BPF_MAP_TYPE_QUEUE` map type to
enqueue tasks. It also illustrates how core-sched support could be implemented.

### Typical Use Case

Purely used to illustrate sched_ext features.

### Production Ready?

No

--------------------------------------------------------------------------------

## scx_central

### Overview

A "central" scheduler where scheduling decisions are made from a single CPU.
This scheduler illustrates how scheduling decisions can be dispatched from a
single CPU, allowing other cores to run with infinite slices, without timer
ticks, and without having to incur the overhead of making scheduling decisions.

### Typical Use Case

This scheduler could theoretically be useful for any workload that benefits
from minimizing scheduling overhead and timer ticks. An example of where this
could be particularly useful is running VMs, where running with infinite slices
and no timer ticks allows the VM to avoid unnecessary expensive vmexits.

### Production Ready?

Not yet. While tasks are run with an infinite slice (SCX_SLICE_INF), they're
preempted every 20ms in a timer callback. The scheduler also puts the core
schedling logic inside of the central / scheduling CPU's ops.dispatch() path,
and does not yet have any kind of priority mechanism.

--------------------------------------------------------------------------------

## scx_pair

### Overview

A sibling scheduler which ensures that tasks will only ever be co-located on a
physical core if they're in the same cgroup. It illustrates how a scheduling
policy could be implemented to mitigate CPU bugs, such as L1TF, and also shows
how some useful kfuncs such as `scx_bpf_kick_cpu()` can be utilized.

### Typical Use Case

While this scheduler is only meant to be used to illustrate certain sched_ext
features, with a bit more work (e.g. by adding some form of priority handling
inside and across cgroups), it could have been used as a way to quickly
mitigate L1TF before core scheduling was implemented and rolled out.

### Production Ready?

No

--------------------------------------------------------------------------------

## scx_flatcg

### Overview

A flattened cgroup hierarchy scheduler. This scheduler implements hierarchical
weight-based cgroup CPU control by flattening the cgroup hierarchy into a
single layer, by compounding the active weight share at each level. The effect
of this is a much more performant CPU controller, which does not need to
descend down cgroup trees in order to properly compute a cgroup's share.

### Typical Use Case

This scheduler could be useful for any typical workload requiring a CPU
controller, but which cannot tolerate the higher overheads of the fair CPU
controller.

### Production Ready?

Yes, though the scheduler (currently) does not adequately accommodate
thundering herds of cgroups. If, for example, many cgroups which are nested
behind a low-priority cgroup were to wake up around the same time, they may be
able to consume more CPU cycles than they are entitled to.

--------------------------------------------------------------------------------

## scx_userland

### Overview

A simple weighted vtime scheduler where all scheduling decisions take place in
user space. This is in contrast to Rusty, where load balancing lives in user
space, but scheduling decisions are still made in the kernel.

### Typical Use Case

There are many advantages to writing schedulers in user space. For example, you
can use a debugger, you can write the scheduler in Rust, and you can use data
structures bundled with your favorite library.

On the other hand, user space scheduling can be hard to get right. You can
potentially deadlock due to not scheduling a task that's required for the
scheduler itself to make forward progress (though the sched_ext watchdog will
protect the system by unloading your scheduler after a timeout if that
happens). You also have to bootstrap some communication protocol between the
kernel and user space.

A more robust solution to this would be building a user space scheduling
framework that abstracts much of this complexity away from you.

### Production Ready?

No. This scheduler uses an ordered list for vtime scheduling, and is stricly
less performant than just using something like `scx_simple`. It is purely
meant to illustrate that it's possible to build a user space scheduler on
top of sched_ext.

--------------------------------------------------------------------------------

## scx_rusty

### Overview

A multi-domain, BPF / user space hybrid scheduler. The BPF portion of the
scheduler does a simple round robin in each domain, and the user space portion
(written in Rust) calculates the load factor of each domain, and informs BPF of
how tasks should be load balanced accordingly.

### Typical Use Case

Rusty is designed to be flexible, and accommodate different architectures and
workloads. Various load balancing thresholds (e.g. greediness, frequenty, etc),
as well as how Rusty should partition the system into scheduling domains, can
be tuned to achieve the optimal configuration for any given system or workload.

### Production Ready?

Yes. If tuned correctly, rusty should be performant across various CPU
architectures and workloads. Rusty by default creates a separate scheduling
domain per-LLC, so its default configuration may be performant as well.

That said, you may run into an issue with infeasible weights, where a task with
a very high weight may cause the scheduler to incorrectly leave cores idle
because it thinks they're necessary to accommodate the compute for a single
task. This can also happen in CFS, and should soon be addressed for rusty.

--------------------------------------------------------------------------------

# Troubleshooting

There are a number of common issues that you may run into when building the
schedulers. We'll go over some of the common ones here.

## Build Failures

### Old version of clang

```
error: static assertion failed due to requirement 'SCX_DSQ_FLAG_BUILTIN': bpftool generated vmlinux.h is missing high bits for 64bit enums, upgrade clang and pahole
        _Static_assert(SCX_DSQ_FLAG_BUILTIN,
                       ^~~~~~~~~~~~~~~~~~~~
1 error generated.
```

This means you built the kernel or the schedulers with an older version of
clang than what's supported (i.e. older than 16.0.0). To remediate this:

1. `which clang` to make sure you're using a sufficiently new version of clang.

2. `make fullclean` in the root path of the repository, and rebuild the kernel
   and schedulers.

3. Rebuild the kernel, and then your example schedulers.

The schedulers are also cleaned if you invoke `make mrproper` in the root
directory of the tree.

### Stale kernel build / incomplete vmlinux.h file

As described above, you'll need a `vmlinux.h` file that was generated from a
vmlinux built with BTF, and with sched_ext support enabled. If you don't,
you'll see errors such as the following which indicate that a type being
referenced in a scheduler is unknown:

```
/path/to/sched_ext/tools/sched_ext/user_exit_info.h:25:23: note: forward declaration of 'struct scx_exit_info'

const struct scx_exit_info *ei)

^
```

In order to resolve this, please follow the steps above in
[Getting a vmlinux.h file](#getting-a-vmlinuxh-file) in order to ensure your
schedulers are using a vmlinux.h file that includes the requisite types.

## Misc

### llvm: [OFF]

You may see the following output when building the schedulers:

```
Auto-detecting system features:
...                         clang-bpf-co-re: [ on  ]
...                                    llvm: [ OFF ]
...                                  libcap: [ on  ]
...                                  libbfd: [ on  ]
```

Seeing `llvm: [ OFF ]` here is not an issue. You can safely ignore.
