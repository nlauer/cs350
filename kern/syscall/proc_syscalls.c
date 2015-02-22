#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include "opt-A2.h"
#include <machine/trapframe.h>

void sys__exit(int exitcode) {
    struct addrspace *as;
    struct proc *p = curproc;
    
    DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);
    
#if OPT_A2
    proc_child_exited(p->pid, exitcode); // Mark this process as having exited, and save its exit code
    lock_acquire(p->exitLock);
    cv_broadcast(p->exitCv, p->exitLock); // Wake any procs that called waitpid for the exiting pid
    lock_release(p->exitLock);
#endif
    
    KASSERT(curproc->p_addrspace != NULL);
    as_deactivate();
    /*
     * clear p_addrspace before calling as_destroy. Otherwise if
     * as_destroy sleeps (which is quite possible) when we
     * come back we'll be calling as_activate on a
     * half-destroyed address space. This tends to be
     * messily fatal.
     */
    as = curproc_setas(NULL);
    as_destroy(as);
    
    /* detach this thread from its process */
    /* note: curproc cannot be used after this call */
    proc_remthread(curthread);
    
    /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
    proc_destroy(p);
    
    thread_exit();
    /* thread_exit() does not return, so we should never get here */
    panic("return from thread_exit in sys_exit\n");
}

int sys_getpid(pid_t *retval)
{
#if OPT_A2
    *retval = curproc->pid;
    return(0);
#else
    /* for now, this is just a stub that always returns a PID of 1 */
    /* you need to fix this to make it work properly */
    *retval = 1;
    return(0);
#endif
}

int sys_waitpid(pid_t pid,
                userptr_t status,
                int options,
                pid_t *retval)
{
    int exitstatus;
    int result;
    
    if (options != 0) {
        DEBUG(DB_PROC, "EINVAL\n");
        *retval = -1;
        return(EINVAL); // We don't support options, return EINVAL
    }
    
#if OPT_A2
    if (!proc_exists(pid)) { // check with the PID list if the proc exists, if not then return ESRCH
        DEBUG(DB_PROC, "ESRCH\n");
        *retval = -1;
        return (ESRCH);
    }
    
    if (!proc_is_child(curproc, pid)) { // the parent is not interested in the child, return ECHILD
        DEBUG(DB_PROC, "ECHILD\n");
        *retval = -1;
        return (ECHILD);
    }
    
    spinlock_acquire(&curproc->p_lock);
    if (proc_has_child_exited(pid) == 1) { // the child process already exited, and we are holding its exitcode
        exitstatus = proc_child_exit_code(pid);
        spinlock_release(&curproc->p_lock);
    }  else { // the process has not yet exited, we should wait for it to exit to get its status code
        // Find the child process we are going to wait on so we can get the cv
        struct proc *childProc = NULL;
        for (unsigned i = 0; i < array_num(curproc->childrenProcesses); i++) {
            struct proc *child = array_get(curproc->childrenProcesses, i);
            if (child->pid == pid) {
                childProc = child;
                break;
            }
        }
        spinlock_release(&curproc->p_lock);
        
        KASSERT(childProc != NULL);
        KASSERT(childProc->exitLock != NULL);
        KASSERT(childProc->exitCv != NULL);
        
        // Wait on the CV until the child process has exited and reported its exitcode
        struct lock *childProcLock = childProc->exitLock;
        lock_acquire(childProcLock);
        while(proc_has_child_exited(pid) != 1) {
            cv_wait(childProc->exitCv, childProcLock);
        }
        exitstatus = proc_child_exit_code(pid);
        lock_release(childProcLock);
        
        // the process has already been destroyed, but we needed past it being destroyed. We destory it here
        lock_destroy(childProcLock);
        
    }
    
    exitstatus = _MKWAIT_EXIT(exitstatus);
#else
    /* for now, just pretend the exitstatus is 0 */
    exitstatus = 0;
#endif
    
    result = copyout((void *)&exitstatus,status,sizeof(int));
    if (result) {
        *retval = -1;
        return(result); // the copy to status failed, return EFAULT
    }
    
    // We have successfully called waitpid on a child, we should free that pid now
    proc_free_pid(pid);
    
    *retval = pid;
    return(0);
}

#if OPT_A2
void
fork_entrypoint(void *childTrapFrame, unsigned long unusednum)
{
    (void)unusednum;
    KASSERT(childTrapFrame != NULL);
    
    struct trapframe *heapTrapFrame = childTrapFrame;
    struct trapframe stackTrapFrame;
    stackTrapFrame = *heapTrapFrame; // copy the childTrapFrame onto the current process's stack
    stackTrapFrame.tf_v0 = 0; // The child should return 0 from the fork call, so set its return value
    stackTrapFrame.tf_epc += 4; // Move the pc forward so we aren't stuck in the same spot always
    stackTrapFrame.tf_a3 = 0;
    kfree(childTrapFrame); // Free the child trap frame that was on the heap
    mips_usermode(&stackTrapFrame);
    
    panic("fork_entrypoint returned\n");
}

int sys_fork(struct trapframe *parentTrapFrame, pid_t *retval)
{
    struct proc *parent = curproc;
    struct proc *child = proc_create_runprogram("child");
    if (child == NULL) {
        return(ENPROC); // Unable to create a new process, because the max amount has already been used
    }
    
    // Store the child and its pid onto its parent
    lock_acquire(child->exitLock);
    int *childPid = kmalloc(sizeof(pid_t));
    *childPid = child->pid;
    int ret = array_add(parent->childrenPids, childPid, NULL);
    KASSERT(ret == 0);
    int ret2 = array_add(parent->childrenProcesses, child, NULL);
    KASSERT(ret2 == 0);
    lock_release(child->exitLock);
    
    // Copy Parent's trapframe onto heap
    struct trapframe *childTrapFrame = (struct trapframe*)kmalloc(sizeof(struct trapframe));
    KASSERT(childTrapFrame != NULL);
    *childTrapFrame = *parentTrapFrame;
    
    // Copy the parent's address space to child
    KASSERT(parent->p_addrspace != NULL); /* Parent should have an addrspace */
    KASSERT(child->p_addrspace == NULL); /* Child should not have an addrspace */
    int asCopyReturn = as_copy(parent->p_addrspace, &child->p_addrspace);
    if (asCopyReturn) {
        return asCopyReturn;
    }
    KASSERT(child->p_addrspace != NULL); /* Child should now have an addrspace */
    
    int result = thread_fork("fork", child, fork_entrypoint, childTrapFrame, 0);
    if (result) {
        return result; // Not enough memory to fork, would be returning ENOMEM
    }
    
    *retval = child->pid;
    return(0);
}
#endif

