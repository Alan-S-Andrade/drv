// SPDX-License-Identifier: MIT
// Copyright (c) 2023 University of Washington

#include <DrvAPI.hpp>
#include <string>
#include <cstdint>
#include <inttypes.h>
#include <deque>
#include "common.h"
#include "task.h"

using namespace DrvAPI;

/* shorthand for l1sp */
template <typename T>
using StaticL1SP = DrvAPIGlobalL1SP<T>;

/* shorthand for l2sp */
template <typename T>
using StaticL2SP = DrvAPIGlobalL2SP<T>;

/* a core's task queue */
struct task_queue {
    std::deque<task*> deque;
    std::mutex mtx;
    void push(task *t) {
        std::lock_guard<std::mutex> lock(mtx);
        deque.push_back(t);
    }
    task *pop() {
        std::lock_guard<std::mutex> lock(mtx);
        if (deque.empty()) {
            return nullptr;
        }
        task *t = deque.front();
        deque.pop_front();
        return t;
    }
};

static constexpr int64_t QUEUE_UNINIT = 0;
static constexpr int64_t QUEUE_INIT_IN_PROGRESS = 1;
static constexpr int64_t QUEUE_INIT = 2;

/* allocate one of these pointers on every core's l1 scratchpad */
StaticL1SP<int64_t> queue_initialized; //!< set if initialized
StaticL1SP<task_queue*> this_cores_task_queue; //!< pointer to this core's task queue

/* allocate on of these per pod */
StaticL2SP<int64_t> terminate; //!< time for the thread to quit


/**
 * execute this task on a specific core
 */
void execute_on(uint32_t pxn, uint32_t pod, uint32_t core, task* t) {
    DrvAPIVAddress queue_vaddr = static_cast<DrvAPIAddress>(&this_cores_task_queue);
    queue_vaddr.global() = true;
    queue_vaddr.l2_not_l1() = false;
    queue_vaddr.pxn() = pxn;
    queue_vaddr.pod() = pod;
    queue_vaddr.core_y() = core >> 3;
    queue_vaddr.core_x() = core & 0x7;
    DrvAPIPointer<task_queue*> queue_absolute_addr = queue_vaddr.encode();
    task_queue *tq = *queue_absolute_addr;
    tq->push(t);
}


/* every thread on every core in the system will call this function */
int Start(int argc, char *argv[])
{
    DrvAPIMemoryAllocatorInit();
    
    // check initialized
    if (atomic_cas(&queue_initialized,
                   QUEUE_UNINIT,
                   QUEUE_INIT_IN_PROGRESS) == QUEUE_UNINIT) {
        // initialize
        this_cores_task_queue = new task_queue;
        if (myPodId() == 0
            && myCoreId() == 0) {
            // only core 0 will run the main function
            task_queue *tq = this_cores_task_queue;
            tq->push(newTask([argc, argv](){                
                pandoMain(argc, argv);
                terminate = 1;
            }));
        }
        // indicate that initialization is complete
        queue_initialized = QUEUE_INIT;
    }
    
    while (queue_initialized != QUEUE_INIT) {
        // wait for initialization to complete
        nop(1000);
    }

    while (terminate != 1) {
        task_queue *tq = this_cores_task_queue;
        task *t = tq->pop();
        if (t == nullptr) {
            nop(1000);
            continue;
        }
        t->execute();
        delete t;
    }
    return 0;    
}

declare_drv_api_main(Start);
