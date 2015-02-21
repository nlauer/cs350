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
    
    DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);
    
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
        return(EINVAL);
    }
    /* for now, just pretend the exitstatus is 0 */
    exitstatus = 0;
    
    
    result = copyout((void *)&exitstatus,status,sizeof(int));
    if (result) {
        return(result);
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
    
    panic("enter_forked_process returned\n");
}

int
sys_fork(struct trapframe *parentTrapFrame,
         pid_t *retval)
{
    DEBUG(DB_SYSCALL, "FORKING");
    struct proc *parent = curproc;
    struct proc *child = proc_create_runprogram("a");
    if (child == NULL) {
        //TODO: more stuff here, fix error code returned
        return(-1);
    }
    
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

