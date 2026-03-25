#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include "mp2_given.h"

// !!!!!!!!!!!!! IMPORTANT !!!!!!!!!!!!!
// Please put your name and email here
MODULE_AUTHOR("Josh Jenks <jajenks2@illinois.edu>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 0
#define ADMISSION_THRESHOLD 693 

enum mp2_state {
	MP2_STATE_RUNNING,
	MP2_STATE_READY,
	MP2_STATE_SLEEPING,
};

// Task control block structure for each registered task
struct mp2_task {
	struct task_struct *task;
	struct timer_list wakeup_timer;
	struct list_head list;

	unsigned long period_ms;
	unsigned long computation_ms;
	unsigned long next_period_jiffies;

	enum mp2_state state;
};


static struct proc_dir_entry *mp2_dir;
static struct proc_dir_entry *mp2_status;

static LIST_HEAD(mp2_task_list);
static DEFINE_MUTEX(mp2_lock);

static struct kmem_cache *mp2_task_cache;
static struct task_struct *mp2_dispatcher_task;
static struct mp2_task *mp2_current;




/* 
	Timer callback, wakeup handler for periodic tasks
	Transitions task from SLEEPING -> READY when period expires
	Wakes dispatcher thread to make scheduling decision
*/
static void mp2_wakeup_timer(struct timer_list *t)
{
	struct mp2_task *wakeup_task;

	wakeup_task = from_timer(wakeup_task, t, wakeup_timer);

	if (READ_ONCE(wakeup_task->state) == MP2_STATE_SLEEPING)
		WRITE_ONCE(wakeup_task->state, MP2_STATE_READY);

	if (mp2_dispatcher_task)
		wake_up_process(mp2_dispatcher_task);

#ifdef DEBUG
	pr_info("MP2 timer fired: pid=%d now READY\n", wakeup_task->task->pid);
#endif
}


/* 
	Find highest priority (shortest period) READY task
	Called under mp2_lock context
	Returns NULL if no READY tasks
*/
static struct mp2_task *mp2_pick_next_ready(void)
{
	struct mp2_task *curr_task;
	struct mp2_task *best_task = NULL;

	list_for_each_entry(curr_task, &mp2_task_list, list) {
		if (READ_ONCE(curr_task->state) != MP2_STATE_READY)
			continue;

		if (!best_task || curr_task->period_ms < best_task->period_ms)
			best_task = curr_task;
	}

	return best_task;
}

/* 
	Search task list for entry matching pid
	Called under mp2_lock context
	Returns task pointer or NULL if not found
*/
static struct mp2_task *mp2_find_task(pid_t pid)
{
	struct mp2_task *curr_task;

	list_for_each_entry(curr_task, &mp2_task_list, list) {
		if (curr_task->task->pid == pid)
			return curr_task;
	}

	return NULL;
}

/* 
	Set task to SCHED_FIFO priority 99 (highest priority)
	Ensures task runs as soon as possible when woken
*/
static void mp2_set_task_fifo(struct task_struct *task)
{
	struct sched_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_policy = SCHED_FIFO;
	attr.sched_priority = 99;

	sched_setattr_nocheck(task, &attr);
}

/* 
	Reset task to SCHED_NORMAL (default scheduling)
	Used when preempting a task from RUNNING state
 */
static void mp2_set_task_normal(struct task_struct *task)
{
	struct sched_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_policy = SCHED_NORMAL;
	attr.sched_priority = 0;

	sched_setattr_nocheck(task, &attr);
}

/* 
	RMS dispatching thread:  the main scheduler
	Wakes on signal, picks highest priority READY task, makes context switch decisions
	Handles 3 cases: no current task, current task SLEEPING, current task RUNNING
	Uses SCHED_FIFO/SCHED_NORMAL to enforce preemption
*/
static int mp2_dispatcher(void *data)
{
	struct mp2_task *next_task;
	struct mp2_task *old_task;

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		if (kthread_should_stop())
			break;

		mutex_lock(&mp2_lock);

		next_task = mp2_pick_next_ready();
		
		//Case 1: no current task
		if (mp2_current == NULL) {
			if (next_task) {
				WRITE_ONCE(next_task->state, MP2_STATE_RUNNING);
				mp2_current = next_task;

				mp2_set_task_fifo(next_task->task);
				wake_up_process(next_task->task);

				#ifdef DEBUG
					pr_info("MP2 dispatcher: scheduled pid=%d (no current task)\n",  next_task->task->pid);
				#endif
			}

			mutex_unlock(&mp2_lock);
			continue;
		}

		/*
		  Case 2: current task is SLEEPING
		  If there is a READY task, run it
		*/
		if (READ_ONCE(mp2_current->state) == MP2_STATE_SLEEPING) {
			if (next_task) {
				WRITE_ONCE(next_task->state, MP2_STATE_RUNNING);
				mp2_current = next_task;

				mp2_set_task_fifo(next_task->task);
				wake_up_process(next_task->task);

				#ifdef DEBUG
					pr_info("MP2 dispatcher: switched from sleeping current to pid=%d\n", next_task->task->pid);
				#endif
			} else {
				mp2_current = NULL;
				#ifdef DEBUG
					pr_info("MP2 dispatcher: no READY task, current cleared\n");
				#endif
			}

			mutex_unlock(&mp2_lock);
			continue;
		}

		/*
		  Case 3: current task is RUNNING
		  Preempt only if the chosen READY task has shorter period.
		*/
		if (READ_ONCE(mp2_current->state) == MP2_STATE_RUNNING) {
			if (next_task && next_task->period_ms < mp2_current->period_ms) {

				old_task = mp2_current;

				WRITE_ONCE(old_task->state, MP2_STATE_READY);
				WRITE_ONCE(next_task->state, MP2_STATE_RUNNING);
				mp2_current = next_task;

				mp2_set_task_normal(old_task->task);
				mp2_set_task_fifo(next_task->task);
				wake_up_process(next_task->task);

				#ifdef DEBUG
					pr_info("MP2 dispatcher: preempt pid=%d -> pid=%d\n", old_task->task->pid, next_task->task->pid);
				#endif
			}
			
			//else: leave current task running
			mutex_unlock(&mp2_lock);
			continue;
		}

		mutex_unlock(&mp2_lock);
	}

	__set_current_state(TASK_RUNNING);
	return 0;
}

/* Show function for the /proc/mp2/status file, print task information into the seq file buffer */
static int mp2_status_show(struct seq_file *m, void *v) {
	struct mp2_task *curr_task;

	mutex_lock(&mp2_lock);
		list_for_each_entry(curr_task, &mp2_task_list, list) {
			seq_printf(m, "%d: %lu, %lu\n", curr_task->task->pid, curr_task->period_ms, curr_task->computation_ms); 
		}
	mutex_unlock(&mp2_lock);

	return 0;
}

/* Open function for the /proc/mp2/status file */
static int mp2_status_open(struct inode *inode, struct file *file) {
	return single_open(file, mp2_status_show, NULL);
}

/* 
	Proc write callback: handle R (register), Y (yield), D (deregister) commands.
	R: admission control check, allocate and insert task.
	Y: set task SLEEPING, arm wakeup timer, wake dispatcher.
	D: remove task, cleanup timer and memory.
*/
static ssize_t mp2_status_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *ppos)
{
	char *buf;
	size_t len;
	pid_t pid;
	unsigned long period_ms, computation_ms;
	int parsed;
	struct mp2_task *new_task;
	struct task_struct *linux_task;
	struct mp2_task *task_to_remove;
	struct mp2_task *yield_task;
	unsigned long next_release;
	unsigned long total_util;
	struct mp2_task *curr_task;

	if (count == 0)
		return 0;

	buf = memdup_user_nul(buffer, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	len = strnlen(buf, count + 1);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	switch (buf[0]) {

		case 'R':
		
			parsed = sscanf(buf, "R,%d,%lu,%lu", &pid, &period_ms, &computation_ms);
			if (parsed != 3) {
				kfree(buf);
				return -EINVAL;
			}

			if (pid <= 0 || period_ms == 0 || computation_ms == 0 || computation_ms > period_ms) {
				kfree(buf);
				return -EINVAL;
			}

			#ifdef DEBUG
				pr_info("MP2 register request: pid=%d period=%lu computation=%lu\n", pid, period_ms, computation_ms);
			#endif
			
			linux_task = find_task_by_pid(pid);
			if (!linux_task) {
				kfree(buf);
				return -ESRCH;
			}

			mutex_lock(&mp2_lock);

			if (mp2_find_task(pid)) {
				mutex_unlock(&mp2_lock);
				kfree(buf);
				return -EEXIST;
			}

			total_util = (computation_ms * 1000) / period_ms;

			list_for_each_entry(curr_task, &mp2_task_list, list) {
				total_util += (curr_task->computation_ms * 1000) /
					      curr_task->period_ms;
			}

			#ifdef DEBUG
				pr_info("MP2 admission check: util=%lu threshold=%d\n", total_util, ADMISSION_THRESHOLD);
			#endif

			if (total_util > ADMISSION_THRESHOLD) {
				mutex_unlock(&mp2_lock);
				kfree(buf);
				return -EINVAL;
			}

			mutex_unlock(&mp2_lock);

			new_task = kmem_cache_alloc(mp2_task_cache, GFP_KERNEL);
			if (!new_task) {
				kfree(buf);
				return -ENOMEM;
			}

			new_task->task = linux_task;
			new_task->period_ms = period_ms;
			new_task->computation_ms = computation_ms;
			new_task->next_period_jiffies = 0;
			new_task->state = MP2_STATE_SLEEPING;
			timer_setup(&new_task->wakeup_timer, mp2_wakeup_timer, 0);
			INIT_LIST_HEAD(&new_task->list);

			mutex_lock(&mp2_lock);

			if (mp2_find_task(pid)) {
				mutex_unlock(&mp2_lock);
				kmem_cache_free(mp2_task_cache, new_task);
				kfree(buf);
				return -EEXIST;
			}

			list_add_tail(&new_task->list, &mp2_task_list);

			mutex_unlock(&mp2_lock);
		break;

		case 'Y':

			parsed = sscanf(buf, "Y,%d", &pid);
			if (parsed != 1 || pid <= 0) {
				kfree(buf);
				return -EINVAL;
			}

			// The yielding task should be the caller itself 
			if (pid != current->pid) {
				kfree(buf);
				return -EINVAL;
			}

			mutex_lock(&mp2_lock);

			yield_task = mp2_find_task(pid);
			if (!yield_task) {
				mutex_unlock(&mp2_lock);
				kfree(buf);
				return -ESRCH;
			}

			// Yield should only happen from the currently running RT task, or sleeping in the first case
			if (READ_ONCE(yield_task->state) != MP2_STATE_RUNNING && READ_ONCE(yield_task->state) != MP2_STATE_SLEEPING) {
				mutex_unlock(&mp2_lock);
				kfree(buf);
				return -EINVAL;
			}

			WRITE_ONCE(yield_task->state, MP2_STATE_SLEEPING);

			if (yield_task->next_period_jiffies == 0)
				yield_task->next_period_jiffies =
					jiffies + msecs_to_jiffies(yield_task->period_ms);
			else
				yield_task->next_period_jiffies +=
					msecs_to_jiffies(yield_task->period_ms);

			next_release = yield_task->next_period_jiffies;
			mod_timer(&yield_task->wakeup_timer, yield_task->next_period_jiffies);
			
			mutex_unlock(&mp2_lock);

			if (mp2_dispatcher_task)
				wake_up_process(mp2_dispatcher_task);

			#ifdef DEBUG
				pr_info("MP2 yield: pid=%d sleeping until jiffies=%lu\n", pid, next_release);
			#endif

			kfree(buf);

			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule();

			__set_current_state(TASK_RUNNING);
			return count;
		break;

		case 'D':
			parsed = sscanf(buf, "D,%d", &pid);
			if (parsed != 1 || pid <= 0) {
				kfree(buf);
				return -EINVAL;
			}

			mutex_lock(&mp2_lock);
			task_to_remove = mp2_find_task(pid);
			if (!task_to_remove) {
				mutex_unlock(&mp2_lock);
				kfree(buf);
				return -ESRCH;
			}
			if (mp2_current == task_to_remove)
				mp2_current = NULL;

			list_del(&task_to_remove->list);
			mutex_unlock(&mp2_lock);

			del_timer_sync(&task_to_remove->wakeup_timer);
			kmem_cache_free(mp2_task_cache, task_to_remove);
			
			#ifdef DEBUG
				pr_info("MP2 deregistered: pid=%d\n", pid);
			#endif
		break;

		default:
			kfree(buf);
			return -EINVAL;
	}

	kfree(buf);
	return count;
}


static const struct proc_ops mp2_proc_ops = {
	.proc_open		= mp2_status_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
	.proc_write		= mp2_status_write,
};

// mp2_init - Called when module is loaded
int __init mp2_init(void)
{
#ifdef DEBUG
	printk(KERN_ALERT "MP2 MODULE LOADING\n");
#endif

	//create the /proc/mp2 directory
	mp2_dir = proc_mkdir("mp2", NULL); 
	if (!mp2_dir)
		return -ENOMEM;
	
	//create the /proc/mp2/status file with read/write permissions for all users and associate it with the defined proc_ops
	mp2_status = proc_create("status", 0666, mp2_dir, &mp2_proc_ops); 
	if (!mp2_status) {
		proc_remove(mp2_dir);
		return -ENOMEM;
	}

	mp2_task_cache = KMEM_CACHE(mp2_task, 0); //creates a slab cache for mp2_task structures. (0) means no special flags are used
	if (!mp2_task_cache) {
		proc_remove(mp2_status);
		proc_remove(mp2_dir);
		return -ENOMEM;
	}

	mp2_current = NULL;
	mp2_dispatcher_task = kthread_run(mp2_dispatcher, NULL, "mp2_dispatcher");
	if (IS_ERR(mp2_dispatcher_task)) {
		mp2_dispatcher_task = NULL;
		kmem_cache_destroy(mp2_task_cache);
		proc_remove(mp2_status);
		proc_remove(mp2_dir);
		return -ENOMEM;
	}

	printk(KERN_ALERT "MP2 MODULE LOADED\n");
	return 0;
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void)
{
	struct mp2_task *curr_task;

#ifdef DEBUG
	printk(KERN_ALERT "MP2 MODULE UNLOADING\n");
#endif

	if (mp2_dispatcher_task)
		kthread_stop(mp2_dispatcher_task);

	while (1) {
		mutex_lock(&mp2_lock);

		if (list_empty(&mp2_task_list)) {
			mutex_unlock(&mp2_lock);
			break;
		}

		curr_task = list_first_entry(&mp2_task_list, struct mp2_task, list);
		list_del(&curr_task->list);

		if (mp2_current == curr_task)
			mp2_current = NULL;

		mutex_unlock(&mp2_lock);

		del_timer_sync(&curr_task->wakeup_timer);
		kmem_cache_free(mp2_task_cache, curr_task);
	}

	if (mp2_task_cache)
		kmem_cache_destroy(mp2_task_cache);

	if (mp2_status)
		proc_remove(mp2_status);
	if (mp2_dir)
		proc_remove(mp2_dir);


	printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);
