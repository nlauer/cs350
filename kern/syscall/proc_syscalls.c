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

/* this implementation of sys__exit does not do anything with the exit code */
/* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {
    struct addrspace *as;
    struct proc *p = curproc;
    /* for now, just include this to keep the compiler from complaining about
     an unused variable */
    (void)exitcode;
    
    DEBUG(DB_PROC,"Syscall: _exit(%d)\n",exitcode);
    
    proc_child_exited(p, p->pid, exitcode);
    lock_acquire(p->exitLock);
    cv_broadcast(p->exitCv, p->exitLock);
    DEBUG(DB_PROC, "We broadcasted the cv\n");
    lock_release(p->exitLock);
    
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
    DEBUG(DB_PROC, "Exiting: %u\n", p->pid);
    proc_destroy(p);
    
    thread_exit();
    /* thread_exit() does not return, so we should never get here */
    panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
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

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
            userptr_t status,
            int options,
            pid_t *retval)
{
    int exitstatus;
    int result;
    
    if (options != 0) {
        DEBUG(DB_PROC, "EINVAL\n");
        return(EINVAL); // We don't support options, return EINVAL
    }
    
#if OPT_A2
    if (!proc_exists(curproc, pid)) { // check with the PID list if the proc exists, if not then return ESRCH
        DEBUG(DB_PROC, "ESRCH\n");
        return (ESRCH);
    }
    
    if (!proc_is_child(curproc, pid)) { // the parent is not interested in the child, return ECHILD
        DEBUG(DB_PROC, "ECHILD\n");
        return (ECHILD);
    }
    
    spinlock_acquire(&curproc->p_lock);
    if (proc_has_child_exited(curproc, pid) == 1) { // the child process already exited, and we are holding its exitcode
        DEBUG(DB_PROC, "has exited already\n");
        exitstatus = proc_child_exit_code(curproc, pid);
        spinlock_release(&curproc->p_lock);
    }  else { // the process has not yet exited, we should wait for it to exit to get its status code
        DEBUG(DB_PROC, "%u has not exited, must wait\n", pid);
        
        struct proc *childProc = NULL;
        for (unsigned i = 0; i < array_num(curproc->childrenProcesses); i++) {
            DEBUG(DB_PROC, "Looking for the child\n");
            struct proc *child = array_get(curproc->childrenProcesses, i);
            if (child->pid == pid) {
                childProc = child;
                DEBUG(DB_PROC, "We found the child\n");
                break;
            }
        }
        spinlock_release(&curproc->p_lock);
        
        KASSERT(childProc != NULL);
        KASSERT(childProc->exitLock != NULL);
        KASSERT(childProc->exitCv != NULL);
        
        struct lock *childProcLock = childProc->exitLock;
       
        lock_acquire(childProcLock);
        DEBUG(DB_PROC, "We acquired the lock\n");
        while(proc_has_child_exited(curproc, pid) != 1) {
            cv_wait(childProc->exitCv, childProcLock);
            DEBUG(DB_PROC, "We have awaken from the cv\n");
        }
        exitstatus = proc_child_exit_code(curproc, pid);
        lock_release(childProcLock);
        DEBUG(DB_PROC, "We released the lock\n");
    }
    
    exitstatus = _MKWAIT_EXIT(exitstatus);
#else
    /* for now, just pretend the exitstatus is 0 */
    exitstatus = 0;
#endif
    
    result = copyout((void *)&exitstatus,status,sizeof(int));
    if (result) {
        return(result); // the copy to status failed, return EFAULT
    }
    *retval = pid;
    return(0);
}

#if OPT_A2
void
fork_entrypoint(void *childTrapFrame,
                unsigned long unusednum)
{
    (void)unusednum;
    struct trapframe *heapTrapFrame = childTrapFrame;
    struct trapframe stackTrapFrame;
    stackTrapFrame = *heapTrapFrame; // copy the childTrapFrame onto the current process's stack
    stackTrapFrame.tf_v0 = 0; // The child should return 0 from the fork call, so set its return value
    stackTrapFrame.tf_epc += 4; // Move the pc forward so we aren't stuck in the same spot always
    stackTrapFrame.tf_a3 = 0;
    kfree(childTrapFrame);
    mips_usermode(&stackTrapFrame);
    
    panic("fork_entrypoint returned\n");
}

int
sys_fork(struct trapframe *parentTrapFrame,
         pid_t *retval)
{
    struct proc *parent = curproc;
    struct proc *child = proc_create_runprogram("child");
    if (child == NULL) {
        //TODO: more stuff here, fix error code returned
        return(-1);
    }
    
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
        panic("FAILED TO FORK");
    }
    
    *retval = child->pid;
    return(0);
}
#endif

