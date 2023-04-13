// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.
#[path = "bpf/.output/atropos.skel.rs"]
mod atropos;
pub use atropos::*;
pub mod atropos_sys;

use std::cell::Cell;
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CStr;
use std::ops::Bound::{Included, Unbounded};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, SystemTime};

use ::fb_procfs as procfs;
use anyhow::{anyhow, bail, Context, Result};
use bitvec::prelude::*;
use clap::Parser;
use log::{info, trace, warn};
use ordered_float::OrderedFloat;

/// Atropos is a multi-domain BPF / userspace hybrid scheduler where the BPF
/// part does simple round robin in each domain and the userspace part
/// calculates the load factor of each domain and tells the BPF part how to load
/// balance the domains.

/// This scheduler demonstrates dividing scheduling logic between BPF and
/// userspace and using rust to build the userspace part. An earlier variant of
/// this scheduler was used to balance across six domains, each representing a
/// chiplet in a six-chiplet AMD processor, and could match the performance of
/// production setup using CFS.
#[derive(Debug, Parser)]
struct Opts {
    /// Scheduling slice duration in microseconds.
    #[clap(short, long, default_value = "20000")]
    slice_us: u64,

    /// Monitoring and load balance interval in seconds.
    #[clap(short, long, default_value = "2.0")]
    interval: f64,

    /// Build domains according to how CPUs are grouped at this cache level
    /// as determined by /sys/devices/system/cpu/cpuX/cache/indexI/id.
    #[clap(short = 'c', long, default_value = "3")]
    cache_level: u32,

    /// Instead of using cache locality, set the cpumask for each domain
    /// manually, provide multiple --cpumasks, one for each domain. E.g.
    /// --cpumasks 0xff_00ff --cpumasks 0xff00 will create two domains with
    /// the corresponding CPUs belonging to each domain. Each CPU must
    /// belong to precisely one domain.
    #[clap(short = 'C', long, num_args = 1.., conflicts_with = "cache_level")]
    cpumasks: Vec<String>,

    /// When non-zero, enable greedy task stealing. When a domain is idle, a
    /// cpu will attempt to steal tasks from a domain with at least
    /// greedy_threshold tasks enqueued. These tasks aren't permanently
    /// stolen from the domain.
    #[clap(short, long, default_value = "4")]
    greedy_threshold: u32,

    /// The load decay factor. Every interval, the existing load is decayed
    /// by this factor and new load is added. Must be in the range [0.0,
    /// 0.99]. The smaller the value, the more sensitive load calculation
    /// is to recent changes. When 0.0, history is ignored and the load
    /// value from the latest period is used directly.
    #[clap(short, long, default_value = "0.5")]
    load_decay_factor: f64,

    /// Disable load balancing. Unless disabled, periodically userspace will
    /// calculate the load factor of each domain and instruct BPF which
    /// processes to move.
    #[clap(short, long, action = clap::ArgAction::SetTrue)]
    no_load_balance: bool,

    /// Put per-cpu kthreads directly into local dsq's.
    #[clap(short, long, action = clap::ArgAction::SetTrue)]
    kthreads_local: bool,

    /// Use FIFO scheduling instead of weighted vtime scheduling.
    #[clap(short, long, action = clap::ArgAction::SetTrue)]
    fifo_sched: bool,

    /// If specified, only tasks which have their scheduling policy set to
    /// SCHED_EXT using sched_setscheduler(2) are switched. Otherwise, all
    /// tasks are switched.
    #[clap(short, long, action = clap::ArgAction::SetTrue)]
    partial: bool,

    /// Enable verbose output including libbpf details. Specify multiple
    /// times to increase verbosity.
    #[clap(short, long, action = clap::ArgAction::Count)]
    verbose: u8,
}

fn read_total_cpu(reader: &mut procfs::ProcReader) -> Result<procfs::CpuStat> {
    Ok(reader
        .read_stat()
        .context("Failed to read procfs")?
        .total_cpu
        .ok_or_else(|| anyhow!("Could not read total cpu stat in proc"))?)
}

fn now_monotonic() -> u64 {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let ret = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut time) };
    assert!(ret == 0);
    time.tv_sec as u64 * 1_000_000_000 + time.tv_nsec as u64
}

fn clear_map(map: &mut libbpf_rs::Map) {
    // XXX: libbpf_rs has some design flaw that make it impossible to
    // delete while iterating despite it being safe so we alias it here
    let deleter: &mut libbpf_rs::Map = unsafe { &mut *(map as *mut _) };
    for key in map.keys() {
        let _ = deleter.delete(&key);
    }
}

#[derive(Debug)]
struct TaskLoad {
    runnable_for: u64,
    load: f64,
}

#[derive(Debug)]
struct TaskInfo {
    pid: i32,
    dom_mask: u64,
    migrated: Cell<bool>,
}

struct LoadBalancer<'a, 'b, 'c> {
    maps: AtroposMapsMut<'a>,
    task_loads: &'b mut BTreeMap<i32, TaskLoad>,
    nr_doms: usize,
    load_decay_factor: f64,

    tasks_by_load: Vec<BTreeMap<OrderedFloat<f64>, TaskInfo>>,
    load_avg: f64,
    dom_loads: Vec<f64>,

    imbal: Vec<f64>,
    doms_to_push: BTreeMap<OrderedFloat<f64>, u32>,
    doms_to_pull: BTreeMap<OrderedFloat<f64>, u32>,

    nr_lb_data_errors: &'c mut u64,
}

impl<'a, 'b, 'c> LoadBalancer<'a, 'b, 'c> {
    const LOAD_IMBAL_HIGH_RATIO: f64 = 0.10;
    const LOAD_IMBAL_REDUCTION_MIN_RATIO: f64 = 0.1;
    const LOAD_IMBAL_PUSH_MAX_RATIO: f64 = 0.50;

    fn new(
        maps: AtroposMapsMut<'a>,
        task_loads: &'b mut BTreeMap<i32, TaskLoad>,
        nr_doms: usize,
        load_decay_factor: f64,
        nr_lb_data_errors: &'c mut u64,
    ) -> Self {
        Self {
            maps,
            task_loads,
            nr_doms,
            load_decay_factor,

            tasks_by_load: (0..nr_doms).map(|_| BTreeMap::<_, _>::new()).collect(),
            load_avg: 0f64,
            dom_loads: vec![0.0; nr_doms],

            imbal: vec![0.0; nr_doms],
            doms_to_pull: BTreeMap::new(),
            doms_to_push: BTreeMap::new(),

            nr_lb_data_errors,
        }
    }

    fn read_task_loads(&mut self, period: Duration) -> Result<()> {
        let now_mono = now_monotonic();
        let task_data = self.maps.task_data();
        let mut this_task_loads = BTreeMap::<i32, TaskLoad>::new();
        let mut load_sum = 0.0f64;
        self.dom_loads = vec![0f64; self.nr_doms];

        for key in task_data.keys() {
            if let Some(task_ctx_vec) = task_data
                .lookup(&key, libbpf_rs::MapFlags::ANY)
                .context("Failed to lookup task_data")?
            {
                let task_ctx =
                    unsafe { &*(task_ctx_vec.as_slice().as_ptr() as *const atropos_sys::task_ctx) };
                let pid = i32::from_ne_bytes(
                    key.as_slice()
                        .try_into()
                        .context("Invalid key length in task_data map")?,
                );

                let (this_at, this_for, weight) = unsafe {
                    (
                        std::ptr::read_volatile(&task_ctx.runnable_at as *const u64),
                        std::ptr::read_volatile(&task_ctx.runnable_for as *const u64),
                        std::ptr::read_volatile(&task_ctx.weight as *const u32),
                    )
                };

                let (mut delta, prev_load) = match self.task_loads.get(&pid) {
                    Some(prev) => (this_for - prev.runnable_for, Some(prev.load)),
                    None => (this_for, None),
                };

                // Non-zero this_at indicates that the task is currently
                // runnable. Note that we read runnable_at and runnable_for
                // without any synchronization and there is a small window
                // where we end up misaccounting. While this can cause
                // temporary error, it's unlikely to cause any noticeable
                // misbehavior especially given the load value clamping.
                if this_at > 0 && this_at < now_mono {
                    delta += now_mono - this_at;
                }

                delta = delta.min(period.as_nanos() as u64);
                let this_load = (weight as f64 * delta as f64 / period.as_nanos() as f64)
                    .clamp(0.0, weight as f64);

                let this_load = match prev_load {
                    Some(prev_load) => {
                        prev_load * self.load_decay_factor
                            + this_load * (1.0 - self.load_decay_factor)
                    }
                    None => this_load,
                };

                this_task_loads.insert(
                    pid,
                    TaskLoad {
                        runnable_for: this_for,
                        load: this_load,
                    },
                );

                load_sum += this_load;
                self.dom_loads[task_ctx.dom_id as usize] += this_load;
                // Only record pids that are eligible for load balancing
                if task_ctx.dom_mask == (1u64 << task_ctx.dom_id) {
                    continue;
                }
                self.tasks_by_load[task_ctx.dom_id as usize].insert(
                    OrderedFloat(this_load),
                    TaskInfo {
                        pid,
                        dom_mask: task_ctx.dom_mask,
                        migrated: Cell::new(false),
                    },
                );
            }
        }

        self.load_avg = load_sum / self.nr_doms as f64;
        *self.task_loads = this_task_loads;
        Ok(())
    }

    // To balance dom loads we identify doms with lower and higher load than average
    fn calculate_dom_load_balance(&mut self) -> Result<()> {
        for (dom, dom_load) in self.dom_loads.iter().enumerate() {
            let imbal = dom_load - self.load_avg;
            if imbal.abs() >= self.load_avg * Self::LOAD_IMBAL_HIGH_RATIO {
                if imbal > 0f64 {
                    self.doms_to_push.insert(OrderedFloat(imbal), dom as u32);
                } else {
                    self.doms_to_pull.insert(OrderedFloat(-imbal), dom as u32);
                }
                self.imbal[dom] = imbal;
            }
        }
        Ok(())
    }

    // Find the first candidate pid which hasn't already been migrated and
    // can run in @pull_dom.
    fn find_first_candidate<'d, I>(tasks_by_load: I, pull_dom: u32) -> Option<(f64, &'d TaskInfo)>
    where
        I: IntoIterator<Item = (&'d OrderedFloat<f64>, &'d TaskInfo)>,
    {
        match tasks_by_load
            .into_iter()
            .skip_while(|(_, task)| task.migrated.get() || task.dom_mask & (1 << pull_dom) == 0)
            .next()
        {
            Some((OrderedFloat(load), task)) => Some((*load, task)),
            None => None,
        }
    }

    fn pick_victim(
        &self,
        (push_dom, to_push): (u32, f64),
        (pull_dom, to_pull): (u32, f64),
    ) -> Option<(&TaskInfo, f64)> {
        let to_xfer = to_pull.min(to_push);

        trace!(
            "considering dom {}@{:.2} -> {}@{:.2}",
            push_dom,
            to_push,
            pull_dom,
            to_pull
        );

        let calc_new_imbal = |xfer: f64| (to_push - xfer).abs() + (to_pull - xfer).abs();

        trace!(
            "to_xfer={:.2} tasks_by_load={:?}",
            to_xfer,
            &self.tasks_by_load[push_dom as usize]
        );

        // We want to pick a task to transfer from push_dom to pull_dom to
        // maximize the reduction of load imbalance between the two. IOW,
        // pick a task which has the closest load value to $to_xfer that can
        // be migrated. Find such task by locating the first migratable task
        // while scanning left from $to_xfer and the counterpart while
        // scanning right and picking the better of the two.
        let (load, task, new_imbal) = match (
            Self::find_first_candidate(
                self.tasks_by_load[push_dom as usize]
                    .range((Unbounded, Included(&OrderedFloat(to_xfer))))
                    .rev(),
                pull_dom,
            ),
            Self::find_first_candidate(
                self.tasks_by_load[push_dom as usize]
                    .range((Included(&OrderedFloat(to_xfer)), Unbounded)),
                pull_dom,
            ),
        ) {
            (None, None) => return None,
            (Some((load, task)), None) | (None, Some((load, task))) => {
                (load, task, calc_new_imbal(load))
            }
            (Some((load0, task0)), Some((load1, task1))) => {
                let (new_imbal0, new_imbal1) = (calc_new_imbal(load0), calc_new_imbal(load1));
                if new_imbal0 <= new_imbal1 {
                    (load0, task0, new_imbal0)
                } else {
                    (load1, task1, new_imbal1)
                }
            }
        };

        // If the best candidate can't reduce the imbalance, there's nothing
        // to do for this pair.
        let old_imbal = to_push + to_pull;
        if old_imbal * (1.0 - Self::LOAD_IMBAL_REDUCTION_MIN_RATIO) < new_imbal {
            trace!(
                "skipping pid {}, dom {} -> {} won't improve imbal {:.2} -> {:.2}",
                task.pid,
                push_dom,
                pull_dom,
                old_imbal,
                new_imbal
            );
            return None;
        }

        trace!(
            "migrating pid {}, dom {} -> {}, imbal={:.2} -> {:.2}",
            task.pid,
            push_dom,
            pull_dom,
            old_imbal,
            new_imbal,
        );

        Some((task, load))
    }

    // Actually execute the load balancing. Concretely this writes pid -> dom
    // entries into the lb_data map for bpf side to consume.
    fn load_balance(&mut self) -> Result<()> {
        clear_map(self.maps.lb_data());

        trace!("imbal={:?}", &self.imbal);
        trace!("doms_to_push={:?}", &self.doms_to_push);
        trace!("doms_to_pull={:?}", &self.doms_to_pull);

        // Push from the most imbalanced to least.
        while let Some((OrderedFloat(mut to_push), push_dom)) = self.doms_to_push.pop_last() {
            let push_max = self.dom_loads[push_dom as usize] * Self::LOAD_IMBAL_PUSH_MAX_RATIO;
            let mut pushed = 0f64;

            // Transfer tasks from push_dom to reduce imbalance.
            loop {
                let last_pushed = pushed;

                // Pull from the most imbalaned to least.
                let mut doms_to_pull = BTreeMap::<_, _>::new();
                std::mem::swap(&mut self.doms_to_pull, &mut doms_to_pull);
                let mut pull_doms = doms_to_pull.into_iter().rev().collect::<Vec<(_, _)>>();

                for (to_pull, pull_dom) in pull_doms.iter_mut() {
                    if let Some((task, load)) =
                        self.pick_victim((push_dom, to_push), (*pull_dom, f64::from(*to_pull)))
                    {
                        // Execute migration.
                        task.migrated.set(true);
                        to_push -= load;
                        *to_pull -= load;
                        pushed += load;

                        // Ask BPF code to execute the migration.
                        let pid = task.pid;
                        let cpid = (pid as libc::pid_t).to_ne_bytes();
                        if let Err(e) = self.maps.lb_data().update(
                            &cpid,
                            &pull_dom.to_ne_bytes(),
                            libbpf_rs::MapFlags::NO_EXIST,
                        ) {
                            warn!(
                                "Failed to update lb_data map for pid={} error={:?}",
                                pid, &e
                            );
                            *self.nr_lb_data_errors += 1;
                        }

                        // Always break after a successful migration so that
                        // the pulling domains are always considered in the
                        // descending imbalance order.
                        break;
                    }
                }

                pull_doms
                    .into_iter()
                    .map(|(k, v)| self.doms_to_pull.insert(k, v))
                    .count();

                // Stop repeating if nothing got transferred or pushed enough.
                if pushed == last_pushed || pushed >= push_max {
                    break;
                }
            }
        }
        Ok(())
    }
}

struct Scheduler<'a> {
    skel: AtroposSkel<'a>,
    struct_ops: Option<libbpf_rs::Link>,

    nr_cpus: usize,
    nr_doms: usize,
    load_decay_factor: f64,
    balance_load: bool,

    proc_reader: procfs::ProcReader,

    prev_at: SystemTime,
    prev_total_cpu: procfs::CpuStat,
    task_loads: BTreeMap<i32, TaskLoad>,

    nr_lb_data_errors: u64,
}

impl<'a> Scheduler<'a> {
    // Returns Vec of cpuset for each dq and a vec of dq for each cpu
    fn parse_cpusets(
        cpumasks: &[String],
        nr_cpus: usize,
    ) -> Result<(Vec<BitVec<u64, Lsb0>>, Vec<i32>)> {
        if cpumasks.len() > atropos_sys::MAX_DOMS as usize {
            bail!(
                "Number of requested DSQs ({}) is greater than MAX_DOMS ({})",
                cpumasks.len(),
                atropos_sys::MAX_DOMS
            );
        }
        let mut cpus = vec![-1i32; nr_cpus];
        let mut cpusets =
            vec![bitvec![u64, Lsb0; 0; atropos_sys::MAX_CPUS as usize]; cpumasks.len()];
        for (dq, cpumask) in cpumasks.iter().enumerate() {
            let hex_str = {
                let mut tmp_str = cpumask
                    .strip_prefix("0x")
                    .unwrap_or(cpumask)
                    .replace('_', "");
                if tmp_str.len() % 2 != 0 {
                    tmp_str = "0".to_string() + &tmp_str;
                }
                tmp_str
            };
            let byte_vec = hex::decode(&hex_str)
                .with_context(|| format!("Failed to parse cpumask: {}", cpumask))?;

            for (index, &val) in byte_vec.iter().rev().enumerate() {
                let mut v = val;
                while v != 0 {
                    let lsb = v.trailing_zeros() as usize;
                    v &= !(1 << lsb);
                    let cpu = index * 8 + lsb;
                    if cpu > nr_cpus {
                        bail!(
                            concat!(
                                "Found cpu ({}) in cpumask ({}) which is larger",
                                " than the number of cpus on the machine ({})"
                            ),
                            cpu,
                            cpumask,
                            nr_cpus
                        );
                    }
                    if cpus[cpu] != -1 {
                        bail!(
                            "Found cpu ({}) with dq ({}) but also in cpumask ({})",
                            cpu,
                            cpus[cpu],
                            cpumask
                        );
                    }
                    cpus[cpu] = dq as i32;
                    cpusets[dq].set(cpu, true);
                }
            }
            cpusets[dq].set_uninitialized(false);
        }

        for (cpu, &dq) in cpus.iter().enumerate() {
            if dq < 0 {
                bail!(
                "Cpu {} not assigned to any dq. Make sure it is covered by some --cpumasks argument.",
                cpu
            );
            }
        }

        Ok((cpusets, cpus))
    }

    // Returns Vec of cpuset for each dq and a vec of dq for each cpu
    fn cpusets_from_cache(
        level: u32,
        nr_cpus: usize,
    ) -> Result<(Vec<BitVec<u64, Lsb0>>, Vec<i32>)> {
        let mut cpu_to_cache = vec![]; // (cpu_id, cache_id)
        let mut cache_ids = BTreeSet::<u32>::new();
        let mut nr_not_found = 0;

        // Build cpu -> cache ID mapping.
        for cpu in 0..nr_cpus {
            let path = format!("/sys/devices/system/cpu/cpu{}/cache/index{}/id", cpu, level);
            let id = match std::fs::read_to_string(&path) {
                Ok(val) => val
                    .trim()
                    .parse::<u32>()
                    .with_context(|| format!("Failed to parse {:?}'s content {:?}", &path, &val))?,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                    nr_not_found += 1;
                    0
                }
                Err(e) => return Err(e).with_context(|| format!("Failed to open {:?}", &path)),
            };

            cpu_to_cache.push(id);
            cache_ids.insert(id);
        }

        if nr_not_found > 1 {
            warn!(
                "Couldn't determine level {} cache IDs for {} CPUs out of {}, assigned to cache ID 0",
                level, nr_not_found, nr_cpus
            );
        }

        // Cache IDs may have holes. Assign consecutive domain IDs to
        // existing cache IDs.
        let mut cache_to_dom = BTreeMap::<u32, u32>::new();
        let mut nr_doms = 0;
        for cache_id in cache_ids.iter() {
            cache_to_dom.insert(*cache_id, nr_doms);
            nr_doms += 1;
        }

        if nr_doms > atropos_sys::MAX_DOMS {
            bail!(
                "Total number of doms {} is greater than MAX_DOMS ({})",
                nr_doms,
                atropos_sys::MAX_DOMS
            );
        }

        // Build and return dom -> cpumask and cpu -> dom mappings.
        let mut cpusets =
            vec![bitvec![u64, Lsb0; 0; atropos_sys::MAX_CPUS as usize]; nr_doms as usize];
        let mut cpu_to_dom = vec![];

        for cpu in 0..nr_cpus {
            let dom_id = cache_to_dom[&cpu_to_cache[cpu]];
            cpusets[dom_id as usize].set(cpu, true);
            cpu_to_dom.push(dom_id as i32);
        }

        Ok((cpusets, cpu_to_dom))
    }

    fn init(opts: &Opts) -> Result<Self> {
        // Open the BPF prog first for verification.
        let mut skel_builder = AtroposSkelBuilder::default();
        skel_builder.obj_builder.debug(opts.verbose > 0);
        let mut skel = skel_builder.open().context("Failed to open BPF program")?;

        let nr_cpus = libbpf_rs::num_possible_cpus().unwrap();
        if nr_cpus > atropos_sys::MAX_CPUS as usize {
            bail!(
                "nr_cpus ({}) is greater than MAX_CPUS ({})",
                nr_cpus,
                atropos_sys::MAX_CPUS
            );
        }

        // Initialize skel according to @opts.
        let (cpusets, cpus) = if opts.cpumasks.len() > 0 {
            Self::parse_cpusets(&opts.cpumasks, nr_cpus)?
        } else {
            Self::cpusets_from_cache(opts.cache_level, nr_cpus)?
        };
        let nr_doms = cpusets.len();
        skel.rodata().nr_doms = nr_doms as u32;
        skel.rodata().nr_cpus = nr_cpus as u32;

        for (cpu, dom) in cpus.iter().enumerate() {
            skel.rodata().cpu_dom_id_map[cpu] = *dom as u32;
        }

        for (dom, cpuset) in cpusets.iter().enumerate() {
            let raw_cpuset_slice = cpuset.as_raw_slice();
            let dom_cpumask_slice = &mut skel.rodata().dom_cpumasks[dom];
            let (left, _) = dom_cpumask_slice.split_at_mut(raw_cpuset_slice.len());
            left.clone_from_slice(cpuset.as_raw_slice());
            let cpumask_str = dom_cpumask_slice
                .iter()
                .take((nr_cpus + 63) / 64)
                .rev()
                .fold(String::new(), |acc, x| format!("{} {:016X}", acc, x));
            info!(
                "DOM[{:02}] cpumask{} ({} cpus)",
                dom,
                &cpumask_str,
                cpuset.count_ones()
            );
        }

        skel.rodata().slice_us = opts.slice_us;
        skel.rodata().kthreads_local = opts.kthreads_local;
        skel.rodata().fifo_sched = opts.fifo_sched;
        skel.rodata().switch_partial = opts.partial;
        skel.rodata().greedy_threshold = opts.greedy_threshold;

        // Attach.
        let mut skel = skel.load().context("Failed to load BPF program")?;
        skel.attach().context("Failed to attach BPF program")?;
        let struct_ops = Some(
            skel.maps_mut()
                .atropos()
                .attach_struct_ops()
                .context("Failed to attach atropos struct ops")?,
        );
        info!("Atropos Scheduler Attached");

        // Other stuff.
        let mut proc_reader = procfs::ProcReader::new();
        let prev_total_cpu = read_total_cpu(&mut proc_reader)?;

        Ok(Self {
            skel,
            struct_ops, // should be held to keep it attached

            nr_cpus,
            nr_doms,
            load_decay_factor: opts.load_decay_factor.clamp(0.0, 0.99),
            balance_load: !opts.no_load_balance,

            proc_reader,

            prev_at: SystemTime::now(),
            prev_total_cpu,
            task_loads: BTreeMap::new(),

            nr_lb_data_errors: 0,
        })
    }

    fn get_cpu_busy(&mut self) -> Result<f64> {
        let total_cpu = read_total_cpu(&mut self.proc_reader)?;
        let busy = match (&self.prev_total_cpu, &total_cpu) {
            (
                procfs::CpuStat {
                    user_usec: Some(prev_user),
                    nice_usec: Some(prev_nice),
                    system_usec: Some(prev_system),
                    idle_usec: Some(prev_idle),
                    iowait_usec: Some(prev_iowait),
                    irq_usec: Some(prev_irq),
                    softirq_usec: Some(prev_softirq),
                    stolen_usec: Some(prev_stolen),
                    guest_usec: _,
                    guest_nice_usec: _,
                },
                procfs::CpuStat {
                    user_usec: Some(curr_user),
                    nice_usec: Some(curr_nice),
                    system_usec: Some(curr_system),
                    idle_usec: Some(curr_idle),
                    iowait_usec: Some(curr_iowait),
                    irq_usec: Some(curr_irq),
                    softirq_usec: Some(curr_softirq),
                    stolen_usec: Some(curr_stolen),
                    guest_usec: _,
                    guest_nice_usec: _,
                },
            ) => {
                let idle_usec = curr_idle - prev_idle;
                let iowait_usec = curr_iowait - prev_iowait;
                let user_usec = curr_user - prev_user;
                let system_usec = curr_system - prev_system;
                let nice_usec = curr_nice - prev_nice;
                let irq_usec = curr_irq - prev_irq;
                let softirq_usec = curr_softirq - prev_softirq;
                let stolen_usec = curr_stolen - prev_stolen;

                let busy_usec =
                    user_usec + system_usec + nice_usec + irq_usec + softirq_usec + stolen_usec;
                let total_usec = idle_usec + busy_usec + iowait_usec;
                busy_usec as f64 / total_usec as f64
            }
            _ => {
                bail!("Some procfs stats are not populated!");
            }
        };

        self.prev_total_cpu = total_cpu;
        Ok(busy)
    }

    fn read_bpf_stats(&mut self) -> Result<Vec<u64>> {
        let mut maps = self.skel.maps_mut();
        let stats_map = maps.stats();
        let mut stats: Vec<u64> = Vec::new();
        let zero_vec = vec![vec![0u8; stats_map.value_size() as usize]; self.nr_cpus];

        for stat in 0..atropos_sys::stat_idx_ATROPOS_NR_STATS {
            let cpu_stat_vec = stats_map
                .lookup_percpu(&(stat as u32).to_ne_bytes(), libbpf_rs::MapFlags::ANY)
                .with_context(|| format!("Failed to lookup stat {}", stat))?
                .expect("per-cpu stat should exist");
            let sum = cpu_stat_vec
                .iter()
                .map(|val| {
                    u64::from_ne_bytes(
                        val.as_slice()
                            .try_into()
                            .expect("Invalid value length in stat map"),
                    )
                })
                .sum();
            stats_map
                .update_percpu(
                    &(stat as u32).to_ne_bytes(),
                    &zero_vec,
                    libbpf_rs::MapFlags::ANY,
                )
                .context("Failed to zero stat")?;
            stats.push(sum);
        }
        Ok(stats)
    }

    fn report(
        &self,
        stats: &Vec<u64>,
        cpu_busy: f64,
        processing_dur: Duration,
        load_avg: f64,
        dom_loads: &Vec<f64>,
        imbal: &Vec<f64>,
    ) {
        let stat = |idx| stats[idx as usize];
        let total = stat(atropos_sys::stat_idx_ATROPOS_STAT_WAKE_SYNC)
            + stat(atropos_sys::stat_idx_ATROPOS_STAT_PREV_IDLE)
            + stat(atropos_sys::stat_idx_ATROPOS_STAT_PINNED)
            + stat(atropos_sys::stat_idx_ATROPOS_STAT_DIRECT_DISPATCH)
            + stat(atropos_sys::stat_idx_ATROPOS_STAT_DSQ_DISPATCH)
            + stat(atropos_sys::stat_idx_ATROPOS_STAT_GREEDY)
            + stat(atropos_sys::stat_idx_ATROPOS_STAT_LAST_TASK);

        info!(
            "cpu={:6.1} load_avg={:7.1} bal={} task_err={} lb_data_err={} proc={:?}ms",
            cpu_busy * 100.0,
            load_avg,
            stats[atropos_sys::stat_idx_ATROPOS_STAT_LOAD_BALANCE as usize],
            stats[atropos_sys::stat_idx_ATROPOS_STAT_TASK_GET_ERR as usize],
            self.nr_lb_data_errors,
            processing_dur.as_millis(),
        );

        let stat_pct = |idx| stat(idx) as f64 / total as f64 * 100.0;

        info!(
            "tot={:6} wsync={:4.1} prev_idle={:4.1} pin={:4.1} dir={:4.1} dq={:4.1} greedy={:4.1}",
            total,
            stat_pct(atropos_sys::stat_idx_ATROPOS_STAT_WAKE_SYNC),
            stat_pct(atropos_sys::stat_idx_ATROPOS_STAT_PREV_IDLE),
            stat_pct(atropos_sys::stat_idx_ATROPOS_STAT_PINNED),
            stat_pct(atropos_sys::stat_idx_ATROPOS_STAT_DIRECT_DISPATCH),
            stat_pct(atropos_sys::stat_idx_ATROPOS_STAT_DSQ_DISPATCH),
            stat_pct(atropos_sys::stat_idx_ATROPOS_STAT_GREEDY),
        );

        for i in 0..self.nr_doms {
            info!(
                "DOM[{:02}] load={:7.1} to_pull={:7.1} to_push={:7.1}",
                i,
                dom_loads[i],
                if imbal[i] < 0.0 { -imbal[i] } else { 0.0 },
                if imbal[i] > 0.0 { imbal[i] } else { 0.0 },
            );
        }
    }

    fn step(&mut self) -> Result<()> {
        let started_at = std::time::SystemTime::now();
        let bpf_stats = self.read_bpf_stats()?;
        let cpu_busy = self.get_cpu_busy()?;

        let mut lb = LoadBalancer::new(
            self.skel.maps_mut(),
            &mut self.task_loads,
            self.nr_doms,
            self.load_decay_factor,
            &mut self.nr_lb_data_errors,
        );

        lb.read_task_loads(started_at.duration_since(self.prev_at)?)?;
        lb.calculate_dom_load_balance()?;

        if self.balance_load {
            lb.load_balance()?;
        }

        // Extract fields needed for reporting and drop lb to release
        // mutable borrows.
        let (load_avg, dom_loads, imbal) = (lb.load_avg, lb.dom_loads, lb.imbal);

        self.report(
            &bpf_stats,
            cpu_busy,
            std::time::SystemTime::now().duration_since(started_at)?,
            load_avg,
            &dom_loads,
            &imbal,
        );

        self.prev_at = started_at;
        Ok(())
    }

    fn read_bpf_exit_type(&mut self) -> i32 {
        unsafe { std::ptr::read_volatile(&self.skel.bss().exit_type as *const _) }
    }

    fn report_bpf_exit_type(&mut self) -> Result<()> {
        // Report msg if EXT_OPS_EXIT_ERROR.
        match self.read_bpf_exit_type() {
            0 => Ok(()),
            etype if etype == 2 => {
                let cstr = unsafe { CStr::from_ptr(self.skel.bss().exit_msg.as_ptr() as *const _) };
                let msg = cstr
                    .to_str()
                    .context("Failed to convert exit msg to string")
                    .unwrap();
                bail!("BPF exit_type={} msg={}", etype, msg);
            }
            etype => {
                info!("BPF exit_type={}", etype);
                Ok(())
            }
        }
    }
}

impl<'a> Drop for Scheduler<'a> {
    fn drop(&mut self) {
        if let Some(struct_ops) = self.struct_ops.take() {
            drop(struct_ops);
        }
    }
}

fn main() -> Result<()> {
    let opts = Opts::parse();

    let llv = match opts.verbose {
        0 => simplelog::LevelFilter::Info,
        1 => simplelog::LevelFilter::Debug,
        _ => simplelog::LevelFilter::Trace,
    };
    let mut lcfg = simplelog::ConfigBuilder::new();
    lcfg.set_time_level(simplelog::LevelFilter::Error)
        .set_location_level(simplelog::LevelFilter::Off)
        .set_target_level(simplelog::LevelFilter::Off)
        .set_thread_level(simplelog::LevelFilter::Off);
    simplelog::TermLogger::init(
        llv,
        lcfg.build(),
        simplelog::TerminalMode::Stderr,
        simplelog::ColorChoice::Auto,
    )?;

    let shutdown = Arc::new(AtomicBool::new(false));
    let shutdown_clone = shutdown.clone();
    ctrlc::set_handler(move || {
        shutdown_clone.store(true, Ordering::Relaxed);
    })
    .context("Error setting Ctrl-C handler")?;

    let mut sched = Scheduler::init(&opts)?;

    while !shutdown.load(Ordering::Relaxed) && sched.read_bpf_exit_type() == 0 {
        std::thread::sleep(Duration::from_secs_f64(opts.interval));
        sched.step()?;
    }

    sched.report_bpf_exit_type()
}
