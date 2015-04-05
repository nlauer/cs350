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
#include <vfs.h>
#include <kern/fcntl.h>
#include "copyinout.h"
#include "limits.h"
#include <machine/trapframe.h>

void sys__exit(int exitcode) {
    struct addrspace *as;
    struct proc *p = curproc;
    
    DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);
    
#if OPT_A2
    spinlock_acquire(&curproc->p_lock);
    for (unsigned i = 0; i < array_num(p->childrenPids); i++) {
        int *pid = array_get(p->childrenPids, i);
        if (proc_has_child_exited(*pid)) {
            // The child has already exited, and now its parent is exiting. Free that child's pid
            spinlock_release(&curproc->p_lock);
            proc_free_pid(*pid);
            spinlock_acquire(&curproc->p_lock);
        } else {
            // Tell the child that it's parent has exited, so it knows it can free its pid when it exits
            struct proc *childProc = NULL;
            for (unsigned i = 0; i < array_num(p->childrenProcesses); i++) {
                struct proc *child = array_get(p->childrenProcesses, i);
                if (child->pid == *pid) {
                    childProc = child;
                    break;
                }
            }
            
            if (childProc != NULL) {
                childProc->hasParentExited = 1;
            }
        }
    }
    spinlock_release(&curproc->p_lock);
    
    proc_child_exited(p->pid, exitcode); // Mark this process as having exited, and save its exit code
    if (p->hasParentExited == 1) {
        proc_free_pid(p->pid);
    }
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

int sys_execv(const char *program, char **args)
{
    if (program == NULL || args == NULL) {
        return EFAULT; // one of the args was an invalid pointer
    }
    
    struct addrspace *as;
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    int result;
    
    // Move the args from the calling stack to the kernel
    char **kernelargs = (char **)kmalloc(sizeof(char**));
    result = copyin((const_userptr_t)args, kernelargs, sizeof(char**));
    if (result) {
        kfree(kernelargs);
        return result;
    }
    int i = 0;
    size_t size;
    size_t totalSize = 0;
    while (args[i] != NULL) {
        kernelargs[i] = (char *)kmalloc(sizeof(char *) * PATH_MAX);
        result = copyinstr((const_userptr_t)args[i], kernelargs[i], PATH_MAX, &size);
        if (result) {
            kfree(kernelargs);
            return result;
        }
        totalSize += size;
        i++;
    }
    kernelargs[i] = NULL;
    if (totalSize > ARG_MAX) {
        kfree(kernelargs);
        return E2BIG;
    }
    
    /* open the program */
    char *program_temp;
    program_temp = kstrdup(program);
    result = vfs_open(program_temp, O_RDONLY, 0, &v);
    kfree(program_temp);
    if (result) {
        kfree(kernelargs);
        return result;
    }
    
    /* Create a new address space. */
    as = as_create();
    if (as == NULL) {
        kfree(kernelargs);
        vfs_close(v);
        return ENOMEM;
    }
    
    /* Switch to it and activate it. */
    struct addrspace *old_addrspace = curproc_setas(as);
    as_destroy(old_addrspace);
    as_activate();
    
    /* Load the executable. */
    result = load_elf(v, &entrypoint);
    if (result) {
        /* p_addrspace will go away when curproc is destroyed */
        kfree(kernelargs);
        vfs_close(v);
        return result;
    }
    
    /* Done with the file now. */
    vfs_close(v);
    
    /* Define the user stack in the address space */
    result = as_define_stack(as, &stackptr);
    if (result) {
        /* p_addrspace will go away when curproc is destroyed */
        kfree(kernelargs);
        return result;
    }
    
    /* Put all args onto user stack */
    i = 0;
    while (kernelargs[i] != NULL) {
        int length = strlen(kernelargs[i]) + 1; // get the length of the arg
        stackptr -= length; // move the stack ptr down the length of the arg we are going to add to it
        
        // copy the arg onto the user stack
        result = copyout((const void *)kernelargs[i], (userptr_t)stackptr, (size_t)length);
        if (result) {
            kfree(kernelargs);
            return result;
        }
        
        kernelargs[i] = (char *)stackptr; // save the user space address for the kernel arg
        i++;
    }
    
    // to start our argv array, move down until we are at a divisible by 4 address
    int divisibleBy4 = stackptr % 4;
    if (divisibleBy4 != 0) {
        stackptr -= divisibleBy4;
    }

    // put argv onto the user stack
    stackptr -= sizeof(char*); // add null terminator for argv
    for (int j = (i - 1); j >= 0; j--) {
        stackptr = stackptr - sizeof(char*); // move the stack ptr down 4 spots for the addr to go in
        result = copyout((const void *) (kernelargs + j), (userptr_t) stackptr, (sizeof(char *)));
        if (result) {
            kfree(kernelargs);
            return result;
        }
    }
    
    kfree(kernelargs);
    
    vaddr_t argvstart = stackptr;
    
    // Make sure the stack starts at an address divisible by 8
    int divisibleBy8 = stackptr % 8;
    if (divisibleBy8 != 0) {
        stackptr -= divisibleBy8;
    }
    
    /* Warp to user mode. */
    enter_new_process(i /*argc*/,
                      (userptr_t)argvstart /*userspace addr of argv*/,
                      stackptr,
                      entrypoint);
    
    /* enter_new_process does not return. */
    panic("enter_new_process returned\n");
    return EINVAL;
}
#endif

