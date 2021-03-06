/*
CS 3013 Project 2 Part 2 
Team 48
Alexandra Bittle - albittle
Jonathan Leitschuh - jlleitchuh
Long Nguyen - lhnguyen

moduleSmuteUnsmite.c - This file handles the kernel side smite and unsmite code
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <asm/current.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <asm/errno.h>
#include <asm/uaccess.h>

unsigned long **sys_call_table;
#define MAXLENGTH 100		/* maximum number of processes stored in the array */

asmlinkage long (*ref_sys_cs3013_syscall2)(void);
asmlinkage long (*ref_sys_cs3013_syscall3)(void);

	/* New definition of new_sys_cs3013_syscall2 to be used for smiting a user */
asmlinkage long new_sys_cs3013_syscall2(unsigned short *target_uid, int *num_pids_smited, int *smited_pids, long *pid_states) {
	int 	ksmited_pids[MAXLENGTH]; //Save an array to keep the pids which have been altered
	long	kpid_states[MAXLENGTH];  //An array to save the previous pid states before smiting

	int myUID = current_uid().val;  /*get uid*/

	if(myUID > 1000){
		printk(KERN_INFO "You must be root to run this command\n");
		return -1;
	}

	/*Check to see if the inputs are correct entered*/
	if(target_uid == NULL){
		return -1;
	}

	if(num_pids_smited == NULL){
		return -1;
	}

	if(smited_pids == NULL){
		return -1;
	}

	if(pid_states == NULL){
		return -1;
	}

	/*A user can't smite themself or root*/
	if (*target_uid == myUID){
		printk(KERN_INFO "You can't smite yoursef!\n");
		return -1;
	} else if(*target_uid < 1000){
		printk(KERN_INFO "You can't smite a user with an id less than 1000 (root)!\n");
		return -1;
	}


	struct task_struct *tsk;
	int knum_pid_smitted = 0;

	/*Iterate through each process to get the info and smite them if fit the target_uid*/
	for_each_process(tsk) {
		unsigned int uid_of_task = tsk->real_cred->uid.val;
		if(uid_of_task == *target_uid){
			if(tsk->state == 0){
				if (knum_pid_smitted < MAXLENGTH){
					printk(KERN_INFO "[SMITE] Name: %s UID: %u PID: %d State: %ld Smiter: %d\n", tsk->comm,  uid_of_task, tsk->pid, tsk->state, myUID);
					ksmited_pids[knum_pid_smitted] = tsk->pid;
					tsk->state = TASK_UNINTERRUPTIBLE;
					kpid_states[knum_pid_smitted] = tsk->state; /*Smite the process before saving its state*/
					knum_pid_smitted++; //Add  to the counter to keep track of the number of pids which have been smited.
				} else break;
			}
		}
	}

	/*copy the pointers to user space*/
	if(copy_to_user(num_pids_smited, &knum_pid_smitted, sizeof(int))){
		return EFAULT;
	}

	if (copy_to_user(smited_pids, ksmited_pids, sizeof(ksmited_pids))){
		return EFAULT;
	}

	if (copy_to_user(pid_states, kpid_states, sizeof(kpid_states))){
		return EFAULT;
	}

	return 0;

}

/* New definition of new_sys_cs3013_syscall3 to be used as unsmite*/
asmlinkage long new_sys_cs3013_syscall3(int *num_pids_smited, int *smited_pids, long *pid_states){
	int 	ksmited_pids[*num_pids_smited];
	long	kpid_states[*num_pids_smited];

	int myUID = current_uid().val;  /* get uid */


	if(myUID > 1000){
		printk(KERN_INFO "You must be root to run this command\n");
		return -1;
	}

  	/* Copy smited_pids to kernel */
	int val;
	if (val = copy_from_user(ksmited_pids, smited_pids, sizeof(ksmited_pids))){
		printk(KERN_INFO "ERROR with coppy from user %d\n", val);
		return EFAULT;
	}
    /* Copy smited_pids */
	if (val = copy_from_user(kpid_states, pid_states, sizeof(kpid_states))){
		printk(KERN_INFO "ERROR with coppy from user %d\n", val);
		return EFAULT;
	}


	/* Check for errors in copying */
	if(num_pids_smited == NULL){
		return -1;
	}

	if(smited_pids == NULL){
		return -1;
	}

	if(pid_states == NULL){
		return -1;
	}

	/* Unsmite the processes from the array */
	/* Iterate through each process and unsmite smited processes*/
	struct task_struct *tsk;
	for_each_process(tsk) {
		int i;
		for (i = 0; i < *num_pids_smited; i++){
			if (tsk->pid == ksmited_pids[i] && tsk->state != TASK_RUNNING){
				int success = wake_up_process(tsk); //Wake up the process. suceess will be 1 if this was successful, 0 if not. 
				if (success){
					printk(KERN_INFO "Wake up sucessful\n");
				}
				else {
					printk(KERN_INFO "Can't wake up \n");
				}
				printk(KERN_INFO "[UNSMITE] Name: %s PID: %d state (now): %ld, Unsmiter: %i\n", tsk->comm, tsk->pid, tsk->state, myUID);
			}
		}
	}
	return 0;
}
static unsigned long **find_sys_call_table(void) {
	unsigned long int offset = PAGE_OFFSET;
	unsigned long **sct;

	while (offset < ULLONG_MAX) {
		sct = (unsigned long **)offset;

		if (sct[__NR_close] == (unsigned long *) sys_close) {
			printk(KERN_INFO "Interceptor: Found syscall table at address: 0x%02lX\n",
				(unsigned long) sct);
			return sct;
		}

		offset += sizeof(void *);
	}

	return NULL;
}

static void disable_page_protection(void) {
  /*
    Control Register 0 (cr0) governs how the CPU operates.

    Bit #16, if set, prevents the CPU from writing to memory marked as
    read only. Well, our system call table meets that description.
    But, we can simply turn off this bit in cr0 to allow us to make
    changes. We read in the current value of the register (32 or 64
    bits wide), and AND that with a value where all bits are 0 except
    the 16th bit (using a negation operation), causing the write_cr0
    value to have the 16th bit cleared (with all other bits staying
    the same. We will thus be able to write to the protected memory.

    It's good to be the kernel!
   */
    write_cr0 (read_cr0 () & (~ 0x10000));
}

static void enable_page_protection(void) {
  /*
   See the above description for cr0. Here, we use an OR to set the
   16th bit to re-enable write protection on the CPU.
  */
   write_cr0 (read_cr0 () | 0x10000);
}

static int __init interceptor_start(void) {
  /* Find the system call table */
	if(!(sys_call_table = find_sys_call_table())) {
    /* Well, that didn't work.
       Cancel the module loading step. */
		return -1;
	}

  /* Store a copy of all the existing functions */
	ref_sys_cs3013_syscall2 = (void *)sys_call_table[__NR_cs3013_syscall2];
	ref_sys_cs3013_syscall3 = (void *)sys_call_table[__NR_cs3013_syscall3];


  /* Replace the existing system calls */
	disable_page_protection();

	sys_call_table[__NR_cs3013_syscall2] = (unsigned long *)new_sys_cs3013_syscall2;
	sys_call_table[__NR_cs3013_syscall3] = (unsigned long *)new_sys_cs3013_syscall3;


	enable_page_protection();

  /* And indicate the load was successful */
	printk(KERN_INFO "Loaded interceptor!\n");

	return 0;
}

static void __exit interceptor_end(void) {
  /* If we don't know what the syscall table is, don't bother. */
	if(!sys_call_table)
		return;

  /* Revert all system calls to what they were before we began. */
	disable_page_protection();
	sys_call_table[__NR_cs3013_syscall2] = (unsigned long *)ref_sys_cs3013_syscall2;
	sys_call_table[__NR_cs3013_syscall3] = (unsigned long *)ref_sys_cs3013_syscall3;

	enable_page_protection();

	printk(KERN_INFO "Unloaded interceptor!\n");
}

MODULE_LICENSE("GPL");
module_init(interceptor_start);
module_exit(interceptor_end);
