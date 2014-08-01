/*
 * spi_master.c
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
 * @file spi_master.c
 * @author Pieter Agten (pieter.agten@gmail.com)
 * @date 6 Jun 2013
 */

#include "spi_master.h"
#include "core/crc16.h"
#include "core/events.h"
#include "core/process.h"
#include "core/spi_common.h"
#include "core/timer.h"
#include "hal/gpio.h"
#include "hal/spi.h"
#include "util/bit.h"
#include "util/log.h"

struct spim_trx {
  uint8_t flags;
  uint8_t ss_mask;
  volatile uint8_t *ss_port;
  process* p;
  struct spim_trx* next;
};


PROCESS(spim_trx_process);

static spim_trx* trx_queue_head;
static spim_trx* trx_queue_tail;

#define trx_q_hd_simple ((spim_trx_simple*)trx_queue_head)
#define trx_q_hd_llp    ((spim_trx_llp*)trx_queue_head)

#define RX_DELAY_REMAINING_MASK  0x0F
#define TRX_QUEUED_BIT           7
#define TRX_IN_TRANSMISSION_BIT  6
#define TRX_USE_LLP_BIT          5

#define LLP_TX_DELAY  (30.0 * CLOCK_USEC)
#define LLP_RX_DELAY  (40.0 * CLOCK_USEC)

void spim_init(void)
{
  trx_queue_head = NULL;
  trx_queue_tail = NULL;

  SPI_SET_PIN_DIRS_MASTER();
  SPI_SET_ROLE_MASTER();
  SPI_SET_DATA_ORDER_MSB();
  SPI_SET_MODE(0,0);
  SPI_SET_CLOCK_RATE_DIV_4();
  SPI_TC_INTERRUPT_DISABLE();
  SPI_ENABLE();

  process_start(&spim_trx_process);
}


void spim_trx_init(spim_trx* trx)
{
  trx->flags = 0;
}


spim_trx_set_simple_status
spim_trx_set_simple(spim_trx_simple* trx, uint8_t ss_pin,
		    volatile uint8_t* ss_port, uint8_t tx_size,
		    uint8_t* tx_buf, uint8_t rx_size, uint8_t* rx_buf,
		    process* p)
{
  if (tx_buf == NULL && tx_size > 0) {
    return SPIM_TRX_SIMPLE_TX_BUF_IS_NULL;
  }
  if (rx_buf == NULL && rx_size > 0) {
    return SPIM_TRX_SIMPLE_RX_BUF_IS_NULL;
  }

  trx->flags = 0;
  trx->ss_mask = bv8(ss_pin & 0x07);
  trx->ss_port = ss_port;
  trx->tx_size = tx_size;
  trx->tx_buf = tx_buf;
  trx->rx_size = rx_size;
  trx->rx_buf = rx_buf;
  trx->p = p;
  return SPIM_TRX_SIMPLE_OK;
}


spim_trx_set_llp_status
spim_trx_set_llp(spim_trx_llp* trx, uint8_t ss_pin, volatile uint8_t* ss_port,
		 uint8_t tx_type, uint8_t tx_size, uint8_t* tx_buf,
		 uint8_t rx_max, uint8_t* rx_buf, process* p)
{
  if (tx_buf == NULL && tx_size > 0) {
    return SPIM_TRX_LLP_TX_BUF_IS_NULL;
  }
  if (rx_buf == NULL && rx_max > 0) {
    return SPIM_TRX_LLP_RX_BUF_IS_NULL;
  }

  trx->flags_rx_delay_remaining = _BV(TRX_USE_LLP_BIT) | MAX_RX_DELAY;
  trx->ss_mask = bv8(ss_pin & 0x07);
  trx->ss_port = ss_port;
  trx->tx_type = tx_type;
  trx->tx_size = tx_size;
  trx->tx_buf = tx_buf;
  trx->rx_size = rx_max;
  trx->rx_buf = rx_buf;
  trx->p = p;
  return SPIM_TRX_LLP_OK;
}



inline
bool spim_trx_is_in_transmission(spim_trx* trx)
{
  return trx->flags & _BV(TRX_IN_TRANSMISSION_BIT);
}

static inline
void trx_set_in_transmission(spim_trx* trx, bool v)
{
  if (v) {
    trx->flags |= _BV(TRX_IN_TRANSMISSION_BIT);
  } else {
    trx->flags &= ~_BV(TRX_IN_TRANSMISSION_BIT);    
  }
}

inline
bool spim_trx_is_queued(spim_trx* trx)
{
  return trx->flags & _BV(TRX_QUEUED_BIT);
}

static inline
void trx_set_queued(spim_trx* trx, bool v)
{
  if (v) {
    trx->flags |= _BV(TRX_QUEUED_BIT);
  } else {
    trx->flags &= ~_BV(TRX_QUEUED_BIT);    
  }
}

static inline
uint8_t get_rx_delay_remaining(spim_trx_llp* trx)
{
  return trx->flags_rx_delay_remaining & RX_DELAY_REMAINING_MASK;
}

/**
 * It is only allowed to call this function if get_rx_delay_remaining(trx)
 * is greater than 0!
 */
static inline
void decrement_rx_delay_remaining(spim_trx_llp* trx)
{
  trx->flags_rx_delay_remaining -= 1;
}

spim_trx_queue_status
spim_trx_queue(spim_trx* trx)
{
  if (spim_trx_is_queued(trx)) {
    return SPIM_TRX_QUEUE_ALREADY_QUEUED;
  }

  if (trx_queue_tail == NULL) {
    // Queue is empty
    trx_queue_head = trx;
  } else {
    // Append to queue
    trx_queue_tail->next = trx;
  }
  trx_queue_tail = trx;
  trx->next = NULL;
  trx_set_queued(trx, true);
  return SPIM_TRX_QUEUE_OK;
}
     

static inline
void tx_byte(uint8_t byte)
{
  //  do {
    SPI_SET_DATA_REG(byte);
    //  } while (IS_SPI_WRITE_COLLISION_FLAG_SET()); //Problem: this might clear the SPIF flag!
}

static inline
void tx_dummy_byte()
{
  tx_byte(0);
}

#define DEBUG0 B,0

static inline
void wait_for_tx_complete()
{
  while (! IS_SPI_INTERRUPT_FLAG_SET()) {
    TGL_PIN(DEBUG0);
  }
}

static inline
uint8_t read_response_byte()
{
  return SPI_GET_DATA_REG();
}

static inline
void shift_trx_queue(void)
{
  trx_queue_head = trx_queue_head->next;
  if (trx_queue_head == NULL) {
    trx_queue_tail = NULL;
  }
}

static
void end_transfer(process_event_t ev)
{
  if (trx_queue_head->p != NULL) {
    process_post_event(trx_queue_head->p, ev, (process_data_t)trx_queue_head);
  }

  // Make the slave select pin high
  *(trx_queue_head->ss_port) |= trx_queue_head->ss_mask;

  // Update transfer status
  trx_set_in_transmission(trx_queue_head, false);
  trx_set_queued(trx_queue_head, false);
  
  // Shift transfer queue for next transfer
  shift_trx_queue();
}

static inline
bool is_error_response_type(uint8_t type)
{
  return type > TYPE_RX_PROCESSING;
}



PROCESS_THREAD(spim_trx_process)
{
  PROCESS_BEGIN();
  static timer trx_timer;
  static uint8_t tx_counter;
  static uint8_t rx_counter;
  static crc16 crc;
  static crc16 rx_crc; 

  while (true) {
  start:
    PROCESS_YIELD();
    // Wait until there's something in the queue
    PROCESS_WAIT_WHILE(trx_queue_head == NULL);

    // Update transfer status
    trx_set_in_transmission(trx_queue_head, true);

    // Start transfer by pulling the slave select pin low
    *(trx_queue_head->ss_port) &= ~(trx_queue_head->ss_mask);
    //    TGL_PIN(DEBUG0);
    if (trx_queue_head->flags & _BV(TRX_USE_LLP_BIT)) {
      // Link-layer protocol

      // Send first header byte (message type id)
      tx_byte(trx_q_hd_llp->tx_type);
      timer_set(&trx_timer, CLK_AT_LEAST(LLP_TX_DELAY));
      crc16_init(&crc);
      crc16_update(&crc, trx_q_hd_llp->tx_type);
      crc16_update(&crc, trx_q_hd_llp->tx_size);

      // Send second header byte (message size)
      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      tx_byte(trx_q_hd_llp->tx_size);
      timer_restart(&trx_timer);

      // Send message bytes
      tx_counter = 0;
      while (tx_counter < trx_q_hd_llp->tx_size) {
	PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
	if (read_response_byte() != TYPE_RX_PROCESSING) {
	  end_transfer(SPIM_TRX_ERR_SLAVE_NOT_READY);
	  goto start;
	}
	tx_byte(trx_q_hd_llp->tx_buf[tx_counter]);
	timer_restart(&trx_timer);
	crc16_update(&crc, trx_q_hd_llp->tx_buf[tx_counter]);
	tx_counter += 1;
      }
      
      // Send CRC footer bytes
      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      if (read_response_byte() != TYPE_RX_PROCESSING) {
	end_transfer(SPIM_TRX_ERR_SLAVE_NOT_READY);	
      }
      tx_byte((uint8_t)(crc >> 8));
      timer_restart(&trx_timer);
      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      if (read_response_byte() != TYPE_RX_PROCESSING) {
	end_transfer(SPIM_TRX_ERR_SLAVE_NOT_READY);
	goto start;
      }
      tx_byte((uint8_t)(crc & 0x00FF));
      timer_set(&trx_timer, CLK_AT_LEAST(LLP_RX_DELAY));

      // Wait for response
      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      tx_dummy_byte();
      timer_restart(&trx_timer);
      wait_for_tx_complete();
      while (get_rx_delay_remaining(trx_q_hd_llp) > 0 &&
	     read_response_byte() == TYPE_RX_PROCESSING) {
	PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
	tx_dummy_byte();
	timer_restart(&trx_timer);	
	decrement_rx_delay_remaining(trx_q_hd_llp);
	wait_for_tx_complete();
      }
      
      if (read_response_byte() == TYPE_RX_PROCESSING) {
	// Response is taking too long, abort the transfer
	end_transfer(SPIM_TRX_ERR_NO_RESPONSE);
	goto start;
      }

      // Receive response header (first byte has already been received)
      crc16_init(&crc);
      trx_q_hd_llp->rx_type = read_response_byte();
      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      tx_dummy_byte(); // for the size byte
      timer_restart(&trx_timer); 
      if (is_error_response_type(trx_q_hd_llp->rx_type)) {
	// Error on slave side, abort the transfer
	end_transfer(SPIM_TRX_ERR_SLAVE);
	goto start;
      }
      crc16_update(&crc, trx_q_hd_llp->rx_type);

      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      uint8_t size = read_response_byte();
      tx_dummy_byte(); // for the first payload or footer byte
      timer_restart(&trx_timer); 
      if (size > trx_q_hd_llp->rx_size) {
	// rx_buf is too small for the response, abort the transfer
	end_transfer(SPIM_TRX_ERR_RESPONSE_TOO_LARGE);
	goto start;
      }
      trx_q_hd_llp->rx_size = size;
      crc16_update(&crc, trx_q_hd_llp->rx_size);
      
      // Receive response payload
      rx_counter = 0;
      while (rx_counter < trx_q_hd_llp->rx_size) {
	PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
	trx_q_hd_llp->rx_buf[rx_counter] = read_response_byte();
	tx_dummy_byte();
	timer_restart(&trx_timer); 
	crc16_update(&crc, trx_q_hd_llp->rx_buf[rx_counter]);
	rx_counter += 1;
      }
      
      // Receive response footer (first byte has already been received)
      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      rx_crc = ((crc16)read_response_byte()) << 8;
      tx_dummy_byte();
      timer_restart(&trx_timer); 
      PROCESS_WAIT_UNTIL(timer_expired(&trx_timer));
      rx_crc |= read_response_byte();
      if (! crc16_equal(&crc, &rx_crc)) {
	// CRC failure, abort transfer
	end_transfer(SPIM_TRX_ERR_RESPONSE_CRC_FAILURE);
	goto start;
      }
    } else {
      // Simple transfer
      tx_counter = 0;
      rx_counter = 0;
      while (tx_counter < trx_q_hd_simple->tx_size) {
	tx_byte(trx_q_hd_simple->tx_buf[tx_counter]);
	tx_counter += 1;
	wait_for_tx_complete();
	if (rx_counter < trx_q_hd_simple->rx_size) {
	  trx_q_hd_simple->rx_buf[rx_counter] = read_response_byte();
	  rx_counter += 1;
	}
      }

      while (rx_counter < trx_q_hd_simple->rx_size) {
	tx_dummy_byte();
	wait_for_tx_complete();	
	trx_q_hd_simple->rx_buf[rx_counter] = read_response_byte();
	rx_counter += 1;
      }
    }
    // End current transfer
    end_transfer(SPIM_TRX_COMPLETED_SUCCESSFULLY);
  }

  PROCESS_END();
}
