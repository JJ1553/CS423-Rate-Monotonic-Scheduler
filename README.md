**CS423 Spring 2026 MP2 README**
Josh Jenks
NetID: JaJenks2
Email: jajenks2@illinois.edu

**Overview**
This MP implements a rate monotonic scheduler as a Linux kernel module for Linux 5.15.165. The module provides a procfs interface at /proc/mp2/status that allows user processes to register, yield, and deregister periodic real time tasks. The scheduler tracks registered tasks, performs admission control, dispatches the highest priority READY task according to rate monotonic scheduling, and uses per task timers to release tasks at the beginning of each period.

**Procfs Interface**
The module creates:
- /proc/mp2
- /proc/mp2/status

Supported write commands:
- R,PID,PERIOD,COMPUTATION
- Y,PID
- D,PID

Read format:
- pid: period, computation

**Task Representation**
Each registered task is stored in a custom struct mp2_task containing:
- struct task_struct *task
- struct timer_list wakeup_timer
- struct list_head list
- unsigned long period_ms
- unsigned long computation_ms
- unsigned long next_period_jiffies
- enum mp2_state state

Task states:
- MP2_STATE_RUNNING
- MP2_STATE_READY
- MP2_STATE_SLEEPING

All registered tasks are stored in a kernel linked list protected by mp2_lock.

**Registration**
When a process writes R,PID,PERIOD,COMPUTATION:
1. The proc write handler parses and validates the command.
2. The Linux task_struct is found using the provided helper in mp2_given.h.
3. Admission control is checked before insertion.
4. A new mp2_task is allocated from a slab cache.
5. The task is initialized in MP2_STATE_SLEEPING.
6. The task is added to the global task list.

Duplicate registrations are rejected.

**Admission Control**
Admission control uses the utilization based check required by the MP2 corrected guide:

sum(Ci / Pi) <= 0.693

This is implemented with fixed point integer arithmetic by scaling each utilization term by 1000 and comparing the total against 693.

**Yield Handling**
When a task writes Y,PID:
1. The task is found in the registered task list.
2. Its state is changed to MP2_STATE_SLEEPING.
3. The next release time is computed using next_period_jiffies.
4. The per task timer is armed with mod_timer().
5. The dispatcher thread is awakened.
6. Since yield runs in the task’s own context, the task sets its state to TASK_UNINTERRUPTIBLE and calls schedule() to sleep until its next release.

**Wakeup Timer**
Each registered task owns a timer. When the timer fires:
1. The task is recovered from the timer_list with from_timer().
2. If the task is sleeping, its MP2 state is changed to MP2_STATE_READY.
3. The dispatcher thread is awakened.

This handles the SLEEPING -> READY transition.

**Dispatcher Thread**
A kernel thread is created during module initialization and stopped during module unload.

The dispatcher:
1. Sleeps in TASK_INTERRUPTIBLE until awakened.
2. Scans the task list for the READY task with the shortest period.
3. Uses the global mp2_current pointer to determine what is currently running.
4. If needed, promotes the chosen task to SCHED_FIFO priority 99 and wakes it.
5. If preemption is needed, the old task is returned to SCHED_NORMAL and the higher priority task is scheduled.

This handles:
- READY -> RUNNING
- RUNNING -> READY (preemption)
- scheduling when current is NULL or SLEEPING

**Scheduling Policy**
The scheduler uses Rate Monotonic Scheduling:
- shorter period = higher priority

The READY task with the smallest period is always chosen by the dispatcher.

**Synchronization**
The registered task list and scheduler bookkeeping are protected with mp2_lock.
The task state field uses READ_ONCE/WRITE_ONCE where appropriate because timer callbacks, dispatcher logic, and proc write handlers may all access it concurrently.

**Memory Management**
The module creates a slab cache for struct mp2_task objects during initialization and destroys it during unload. Registered tasks are allocated from this cache and freed on deregistration or module exit.

**User Application**
The user application:
1. Registers itself with /proc/mp2/status.
2. Reads /proc/mp2/status to verify admission.
3. Sends an initial yield.
4. Runs a factorial based busy work loop for a fixed number of jobs.
5. Prints wakeup time and processing time for each job.
6. Yields after each job.
7. Deregisters when finished.

The user application accepts:
- period in ms
- computation time in ms
- number of jobs

**Testing Performed**
The implementation was tested with:
- single task execution
- two task accepted admission control case
- two task rejected admission control case
- tasks with different periods to verify RMS priority behavior
- three concurrent tasks
- clean deregistration
- clean module unload
- checking that /proc/mp2/status is empty after all tasks complete

