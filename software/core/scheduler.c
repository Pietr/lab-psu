/*
 * scheduler.c
 *
 * Copyright 2013 Pieter Agten
 *
 * This file is part of the lab-psu firmware.
 *
 * The firmware is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The firmware is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the firmware.  If not, see <http://www.gnu.org/licenses/>.
 */


/**
 * @file scheduler.c
 * @author Pieter Agten <pieter.agten@gmail.com>
 * @date 29 jun 2013
 *
 * The scheduler uses three lists to schedule tasks:
 *  1) free list - contains unused task slots
 *  2) waiting list - contains tasks that have been scheduled to execute at a 
 *     later time
 *  3) ready queue - contains tasks that should be executed as soon as possible
 * The waiting and ready queue are sorted according to task execution time.
 * 
 * When the scheduler is asked to schedule a task some time in the future, a 
 * new task slot is populated from the free list and placed in the correct 
 * position in the waiting list. Timer 2 is used to trigger an interrupt when
 * the first task in the waiting list needs to be executed. When this interrupt
 * occurs, the first task in the waiting list (and any other tasks in the
 * waiting list that are to be executed at the same time) is moved to the back
 * of the ready queue and the next interrupt is set to occur when the new head
 * of the waiting list needs to be executed (if any). The main thread is 
 * constantly inspecting the ready queue, executing (and removing) tasks from
 * the head of this queue as soon as possible. The system is 'idle' when the 
 * ready queue is empty.
 *
 * If the scheduler is asked to schedule a task immediately, the task is placed
 * at the back of the ready queue immediately.
 */

#include <stdbool.h>
#include <util/atomic.h>

#include "config.h"
#include "hal/timer2.h"
#include "hal/interrupt.h"
#include "util/math.h"
#include "util/log.h"

#include "scheduler.h"


#ifndef SCHED_TASKS_MAX
#warning "SCHED_TASKS_MAX undefined in config file, using default value"
#define SCHED_TASKS_MAX  8
#endif

#define TMR         timer2
#define TMR_CHANNEL oca

struct task_node
{
  uint16_t tick; // Time at which the task must be executed
  sched_task_t task;
  void* data;
  struct task_node* next;
};

static struct task_node tasks[SCHED_TASKS_MAX];
static struct task_node* free_head;
static struct task_node* waiting_head;
static struct task_node* volatile ready_head;
static struct task_node* ready_tail;

static volatile uint16_t next_interrupt_tick;

static bool scheduler_must_stop;

// Prototypes:
static inline void ready_queue_put(struct task_node* node);


void sched_init(void)
{
  // Reset global vars to default value
  waiting_head = 0;
  ready_head = 0;
  ready_tail = 0;
  next_interrupt_tick = 0;
  scheduler_must_stop = false;

  // Put all task slots in free list
  int i;
  for (i = 0; i < SCHED_TASKS_MAX - 1; ++i) {
    tasks[i].next = &(tasks[i+1]);
  }
  free_head = &(tasks[0]);

  // Set up and enable timer 2
  TMR_INIT(TMR);
  TMR_ENABLE_INTERRUPT(TMR,TMR_CHANNEL);        // Enable the timer interrupt
  TMR_SET_OUTPUT_DISCONNECTED(TMR,TMR_CHANNEL); // Disconnect the timer output
  TMR_SET_MODE(TMR,0);                          // Set normal mode
  TMR_REG(TMR,cntr) = 0;                        // Set counter to 0
  TMR_REG(TMR,TMR_CHANNEL) = TMR_REG_MAX(TMR,TMR_CHANNEL); // Set interrupt tick
  next_interrupt_tick = TMR_REG_MAX(TMR,TMR_CHANNEL);
  TMR_SET_CLOCK(TMR,ps_1024);                   // Start timer with prescaler /1024
}


sched_schedule_status sched_schedule(ticks_t ticks, sched_task_t task, void* data)
{
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (free_head == 0) {
      LOG_ERROR("SCHED: no free task slots in scheduler");
      return SCHED_QUEUE_FULL;
    }
  
    // Remove node from free list
    struct task_node* new_node = free_head;
    free_head = free_head->next;
  
    // Populate the node with data
    new_node->task = task;
    new_node->data = data;
  
    if (ticks == 0) {
      ready_queue_put(new_node);
    } else {
      uint8_t current_timer_value = TMR_REG(TMR, cntr);
      uint8_t ticks_until_next_interrupt = TMR_REG(TMR, TMR_CHANNEL) - current_timer_value;
      uint16_t current_tick = next_interrupt_tick - ticks_until_next_interrupt;

      // Insert node into waiting queue
      struct task_node** node = &waiting_head;
      while (*node != 0 && (*node)->tick - current_tick <= ticks) {
        node = &((*node)->next);
      }
      new_node->tick = current_tick + ticks;
      new_node->next = *node;
      *node = new_node;

      if (ticks < TMR_REG(TMR,TMR_CHANNEL) - current_timer_value) {
        TMR_REG(TMR,TMR_CHANNEL) = current_timer_value + ticks;
        next_interrupt_tick = new_node->tick; 
      }
    }
  }
  return SCHED_OK;
}

// Interrupt routine for OCR2A compare match
// Note: Interrupts are disabled in this routine
// Global vars accessed:
//  - next_interrupt_tick
//  - waiting_head
//  - ready_queue_put: ready_head, ready_tail
//  - TMR_REG(TMR, TMR_CHANNEL)
//  - TMR_REG(TMR, cntr)
INTERRUPT(TMR_INTERRUPT_VECT(TMR,TMR_CHANNEL))
{
  uint16_t current_tick = next_interrupt_tick;

  struct task_node** node = &waiting_head;
  while (*node != 0 && (*node)->tick <= current_tick) {
    // Remember node
    struct task_node* node0 = *node;

    // Remove from waiting list
    *node = (*node)->next;

    // Add to run queue
    ready_queue_put(node0);
  }
  // Set-up timer for next event
  uint8_t ticks_until_next_isr;
  if (*node == 0) {
    ticks_until_next_isr = TMR_REG_MAX(TMR, TMR_CHANNEL); // +1?
  } else {
    ticks_until_next_isr = MIN((*node)->tick - current_tick, TMR_REG_MAX(TMR, TMR_CHANNEL)); 
  }
  TMR_REG(TMR, TMR_CHANNEL) = TMR_REG(TMR, cntr) + ticks_until_next_isr;
  next_interrupt_tick += ticks_until_next_isr;
}


sched_exec_status sched_exec(void)
{
  struct task_node* first;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    first = ready_head;
  }
  if (first != 0) { 
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      // Remove first task from ready queue
      ready_head = first->next;
      if (ready_head == 0) ready_tail = 0;
    }

    // Execute the task
    first->task(first->data);
      
    // Add it back to the free list
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      first->next = free_head;    
      free_head = first;
    }
    return SCHED_TASK_EXECUTED;
  }
  
  // System is idle
  return SCHED_IDLE; 
}


// Add to end of ready queue
static inline void ready_queue_put(struct task_node* node)
{
  if (ready_tail == 0) {
    ready_head = ready_tail = node;
  } else {
    ready_tail->next = node;
    ready_tail = node;
  }
  node->next = 0;
}