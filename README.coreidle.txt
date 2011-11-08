COREIDLE framework

2011/11/08
Heechul Yun

Linux kernel's load balancer evenly distributes load among available
cores even when the total load is light. It is generally good for
performance (not always true though due to cache and turbo-boost). In
terms of energy perspective, however, it is not ideal because it
prevents cores from entering deep power saving states (e.g., c6, c7
c-states).

# related techniques
sched_mc/smt framework was intended to address the problem. When a
process  fork/exec/wake-up in a domain with SD_POWERSAVING_BALANCE
(i.e., CPU domain) enabled, it tries to consolidates tasks in the
scheduling domain. There are, however, several drawbacks in the
current implementation.  First, its consolidataion is per domain
basis. If, for example, all cpus  are in the same domain as in
Cortex-A9, the load will spread to all cpus  which effectively no
consolication at all [1]. Furthermore, policy and mechanism is not
clearly separated. Therefore it is difficult to change consolidation
policy. 

Another way to idle core is using hotplug framework which allow
offline a specified core from rest of the Linux kernel. The problem is
that hotplug is very costly (order of 100 msecs ~ 1 sec) because all
tasks,  even actively running tasks, on that core must be migrated
immediately [2]. Therefore, it only can be used infrequently which may
cause performance issues due to slow adaptation to the load change.

Finally, cpuset mechanism can be used to control which cpus to be 
used to handle a given tasks as demonstrated in [3]. This provides 
a very flexible mechanism to userspace such that users can dynamically 
control number of active cpus based on application specific needs. 
However, to implement an efficient user level power management scheme, 
one must have deep information of underlying hardware and kernel 
details such as CPU topology, kernel load statistics which is not 
readily available or change quickly as per kernel development progress.

# approach
In this work, we are proposing coreidle framework such that idling
cores as much as possible without impacting performance. The mechanism
is implemented on top of existing cpuset framework. The key data
structure is sched_coreidle_mask, a bitmask indicating cores desired
to be idle. The mechanism enforces cpus in the sched_coreidle_mask not
to be selected as target cpus in case of fork/exec/wakeup as well as
periodic load balancing operations. A policy governor set the value of
sched_coreidle_mask, therefore determines which cores to be idle. 
We currently provides two sample governors: (1) ondemand and
(2)userspace.  The ondemand governor monitors system load, moving
average filtered value, and determine the number. The usersapce
governor simply provide interface to userspace such that user can
decide which cores to be idle.  

# evaluation
The following evaluation is performed on a Intel 2nd gen corei5 based
notebook  which has two physical cores with hyper threading. Therefore
total four local cores. The baseline kernel is 3.1.0-rc10. To generate
load we used cyclictest and tbench. C states were monitored using
turbostat tool found in the kernel source tree. We compared (1)
default kernel scheduler, (2) sched_mc/smt=1, and (3) ondemand
scheduler which is part of our cpuidle framework. 

We estimated average power consumption based on c-state power number
in [2] by multiplying the number with collected %time of each c-state
in our experiment. The power nubmer used in the calculation is in
Appendix. 

In all test, we used performance cpufreq governor. Note, however, that
the frequency may still vary due to intel's hardware
turbo-boost. Therefore, our power estimation may underestimate the
actual power consumption in the case of coreidle/ondemand
case. However, in the two test programs, reported average clock by
turbostat does not differ significantly. Nevertheless we plan to
measure the actual system level power consumption using powermeter
which is not available to us at this point. 

A. cyclictest

cyclictest is part of rt-test packages which generate lots of small
workload which constantly wakeup after a certain interval. This type
of workload can demonstrate the need of consolication for saving
power. 

- default scheduler
# cyclictest -y other -t 30 -i 200 
# turbostat trace-cmd record -e sched:sched_switch -e sched:sched_wakeup
^c after 5 sec.
 cr CPU    %c0  GHz  TSC    %c1    %c3    %c6    %c7  %pc2  %pc3  %pc6  %pc7
           7.58 2.31 2.29  70.63  20.37   0.08   1.35  0.00  0.00  0.00  0.00
   0   0   8.66 2.36 2.29  58.51  30.36   0.06   2.40  0.00  0.00  0.00  0.00
   0   1   8.86 2.39 2.29  58.32  30.36   0.06   2.40  0.00  0.00  0.00  0.00
   1   2   5.99 1.99 2.29  83.24  10.38   0.10   0.29  0.00  0.00  0.00  0.00
   1   3   6.80 2.42 2.29  82.43  10.38   0.10   0.29  0.00  0.00  0.00  0.00

- sched_mc=1, sched_smt=1
# echo 1 > /sys/devices/system/cpu/sched_smt_power_savings 
# echo 1 > /sys/devices/system/cpu/sched_mc_power_savings 
# cyclictest -y other -t 30 -i 200 
# turbostat trace-cmd record -e sched:sched_switch -e sched:sched_wakeup
^c after 5 sec.
 cr CPU    %c0  GHz  TSC    %c1    %c3    %c6    %c7  %pc2  %pc3  %pc6  %pc7
           7.81 2.33 2.29  81.25  10.34   0.03   0.56  0.00  0.00  0.00  0.00
   0   0   9.26 2.33 2.29  68.97  20.62   0.06   1.09  0.00  0.00  0.00  0.00
   0   1  11.51 2.44 2.29  66.73  20.62   0.06   1.09  0.00  0.00  0.00  0.00
   1   2   5.28 2.14 2.29  94.62   0.06   0.00   0.04  0.00  0.00  0.00  0.00
   1   3   5.21 2.25 2.29  94.69   0.06   0.00   0.04  0.00  0.00  0.00  0.00

- coreidle patch. ondemand governor.
# echo ondemand 250 > /sys/kernel/debug/powersaving
# cyclictest -y other -t 30 -i 200 
# turbostat trace-cmd record -e sched:sched_switch -e sched:sched_wakeup
^c after 5 sec.
 cr CPU    %c0  GHz  TSC    %c1    %c3    %c6    %c7  %pc2  %pc3  %pc6  %pc7
           7.03 2.46 2.29  43.44   0.54   0.00  48.99  0.00  0.00  0.00  0.00
   0   0  26.11 2.46 2.29  73.89   0.00   0.00   0.00  0.00  0.00  0.00  0.00
   0   1   1.65 2.59 2.29  98.35   0.00   0.00   0.00  0.00  0.00  0.00  0.00
   1   2   0.28 2.33 2.29   0.66   1.07   0.00  97.99  0.00  0.00  0.00  0.00
   1   3   0.07 2.45 2.29   0.87   1.07   0.00  97.99  0.00  0.00  0.00  0.00

- power estimation

Estimated power consumption is as follows. As expected, due to the increase in c7 
state in coreidle/ondemand play an important role in saving power consumption. 
   
   load balancer         power(W/s)
-----------------------------------
   default      	 62.79 W/s
   sched_mc/smt		 64.02 W/s
   coreidle/ondemand	 43.79 W/s

B. tbench

Tbench is throuput oriented program which send/receive lots of small packet 
over unix domain socket. Since it report performance, we now can compare 
performance/power metric between different load balancing schemes. 

- default scheduler 
# turbostat tbench -t 20 1
Throughput 225.554 MB/sec  1 clients  1 procs  max_latency=9.904 ms
 cr CPU    %c0  GHz  TSC    %c1    %c3    %c6    %c7  %pc2  %pc3  %pc6  %pc7
          36.78 2.86 2.29  19.91   1.19   0.21  41.91  0.00  0.00  0.00  0.00
   0   0  24.47 2.84 2.29  15.53   1.86   0.32  57.82  0.00  0.00  0.00  0.00
   0   1  23.82 2.85 2.29  16.18   1.86   0.32  57.82  0.00  0.00  0.00  0.00
   1   2  49.59 2.86 2.29  23.80   0.52   0.09  25.99  0.00  0.00  0.00  0.00
   1   3  49.25 2.86 2.29  24.14   0.52   0.09  25.99  0.00  0.00  0.00  0.00

- sched_mc/smt=1

# turbostat tbench -t 20 1
Throughput 224.272 MB/sec  1 clients  1 procs  max_latency=6.202 ms
 cr CPU    %c0  GHz  TSC    %c1    %c3    %c6    %c7  %pc2  %pc3  %pc6  %pc7
          36.41 2.86 2.29  19.03   0.89   0.19  43.47  0.00  0.00  0.00  0.00
   0   0  55.83 2.87 2.29  26.06   0.35   0.07  17.69  0.00  0.00  0.00  0.00
   0   1  55.24 2.87 2.29  26.65   0.35   0.07  17.69  0.00  0.00  0.00  0.00
   1   2  16.82 2.84 2.29  12.18   1.44   0.32  69.25  0.00  0.00  0.00  0.00
   1   3  17.77 2.83 2.29  11.22   1.44   0.32  69.25  0.00  0.00  0.00  0.00

- coreidle/ondemand
# echo ondemand 250 > /sys/kernel/debug/powersaving
# turbostat tbench -t 20 1
Throughput 275.24 MB/sec  1 clients  1 procs  max_latency=6.689 ms
 cr CPU    %c0  GHz  TSC    %c1    %c3    %c6    %c7  %pc2  %pc3  %pc6  %pc7
          29.95 2.87 2.29  23.95   0.54   0.08  45.47  0.00  0.00  0.00  0.00
   0   0  89.02 2.88 2.29  10.64   0.01   0.00   0.32  0.00  0.00  0.00  0.00
   0   1  24.72 2.88 2.29  74.94   0.01   0.00   0.32  0.00  0.00  0.00  0.00
   1   2   4.66 2.69 2.29   3.48   1.07   0.17  90.62  0.00  0.00  0.00  0.00
   1   3   1.40 2.69 2.29   6.74   1.07   0.17  90.62  0.00  0.00  0.00  0.00

- power/performance estimation

  load balancer	    power(W/s)	thoughput(MB/s)	 MB/w
-----------------------------------------------------
  default	    54.68	225.55		 4.12
  sched_mc/smt	    53.94	224.27		 4.16
  coreidle/ondemand 51.43	275.24		 5.35

# summary & conclusion

Coreidle framework is designed to provide flexible and low cost
solution for energy aware load balancing mechanism. In the tested
applications, it reduces power without sacrificing performance.

# references

[1] https://wiki.linaro.org/WorkingGroups/PowerManagement/Specs/sched_mc
[2] https://wiki.linaro.org/WorkingGroups/PowerManagement/Doc/Hotplug
[3] https://wiki.linaro.org/WorkingGroups/PowerManagement/Doc/Cpuset
[4] http://203.143.174.140/publications/papers/LeSueur:msc.pdf


# appendix.
   -------------------------------
   mode       c0    c1   c3   c7 
   power(W)   90    63	 55   20 
   -------------------------------
   Table. power consumption for each c-state [2]

