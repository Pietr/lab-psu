/*
 * process.c
 *
 * Copyright 2014 Pieter Agten
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
 * @file process.c
 * @author Pieter Agten <pieter.agten@gmail.com>
 * @date 8 feb 2014
 *
 * This file implements processes, based on protothreads. It is inspired by the
 * processes implementation of Contiki.
 *
 * @see http://dunkels.com/adam/pt
 * @see http://www.contiki-os.org
 */

#include <stdlib.h>

#include "util/atomic.h"
#include "process.h"

// Should preferably be a power of 2
#define PROCESS_CONF_EVENT_QUEUE_SIZE 16

static process* process_list_head;
static process* next_process;

struct event {
  process* p;
  process_event_t ev;
  process_data_t data;
};

static struct event event_queue[PROCESS_CONF_EVENT_QUEUE_SIZE];
static uint8_t event_queue_count;
static uint8_t event_queue_first;

void process_init(void)
{
  process_list_head = NULL;
  next_process = NULL;

  event_queue_count = 0;
  event_queue_first = 0;
}

static bool process_list_contains(process* p)
{
  process* proc = process_list_head;
  while (proc != NULL) {
    if (proc == p) {
      return true;
    }
  }
  return false;
}


process_start_status process_start(process* p)
{
  if (process_list_contains(p)) {
    return PROCESS_START_ALREADY_STARTED;
  }

  // Add p to the process list
  p->next = process_list_head;
  process_list_head = p;

  // Initialize the protothread
  PT_INIT(&p->pt);

  // Send INIT event
  process_post_event(p, PROCESS_EVENT_INIT, PROCESS_DATA_NULL);

  return PROCESS_START_OK;
}

process_stop_status process_stop(process* p)
{
  process** proc = &process_list_head;
  while (*proc != NULL) {
    if (*proc == p) {
      *proc = (*proc)->next;
      return PROCESS_STOP_OK;
    }
  }
  return PROCESS_STOP_NOT_STARTED;
}

process_post_event_status
process_post_event(process* p, process_event_t ev, process_data_t data)
{
  uint8_t i;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (event_queue_count == PROCESS_CONF_EVENT_QUEUE_SIZE) {
      return PROCESS_POST_EVENT_QUEUE_FULL;
    }

    i = (event_queue_first + event_queue_count) % PROCESS_CONF_EVENT_QUEUE_SIZE;
    event_queue_count += 1;  
  }
  event_queue[i].p = p;
  event_queue[i].ev = ev;
  event_queue[i].data = data;
  
  return PROCESS_POST_EVENT_OK;
}


void process_execute(void)
{
  if (event_queue_count == 0) {
    // No events to process
    return;
  }

  // Process one event
  struct event e = event_queue[event_queue_first];
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { 
    event_queue_count -= 1;
    event_queue_first = (event_queue_first + 1) % PROCESS_CONF_EVENT_QUEUE_SIZE;
  }
  e.p->thread(e.p, e.ev, e.data);
}