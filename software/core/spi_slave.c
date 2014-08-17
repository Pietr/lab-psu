/*
 * spi_slave.c
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
 * @file spi_slave.c
 * @author Pieter Agten <pieter.agten@gmail.com>
 * @date 8 Jul 2014
 *
 * This file implements SPI slave communication using a simple link layer
 * protocol designed for request-response type messages.
 */

#include <stdint.h>
#include <stdlib.h>
#include <util/atomic.h>

#include "spi_slave.h"
#include "core/crc16.h"
#include "core/events.h"
#include "core/process.h"
#include "core/spi_common.h"
#include "hal/gpio.h"
#include "hal/interrupt.h"
#include "hal/spi.h"
#include "util/log.h"

// Note: SPIS_RX_BUF_SIZE must be between 0 and 255
#define SPIS_RX_BUF_SIZE 32

enum spis_trx_status {
  SPIS_TRX_READY,
  SPIS_TRX_RECEIVING_SIZE,
  SPIS_TRX_RECEIVING_PAYLOAD,
  SPIS_TRX_RECEIVING_FOOTER0,
  SPIS_TRX_RECEIVING_FOOTER1,
  SPIS_TRX_WAITING_FOR_CALLBACK,
  SPIS_TRX_SEND_RESPONSE_SIZE,
  SPIS_TRX_SEND_RESPONSE_PAYLOAD,
  SPIS_TRX_SEND_FOOTER0,
  SPIS_TRX_SEND_FOOTER1,
  SPIS_TRX_COMPLETED,
  SPIS_TRX_WAITING_FOR_TRANSFER_TO_END,
  SPIS_TRX_ABORTED_WHILE_WAITING_FOR_CALLBACK
};

/**
 * The SPI transfer data structure.
 */
struct spis_trx {
  uint8_t rx_type;
  uint8_t rx_size;
  uint8_t rx_buf[SPIS_RX_BUF_SIZE];
  crc16 crc;
  uint8_t rx_received;
  uint8_t* tx_buf;
  uint8_t tx_remaining;
  uint8_t error_code_remaining;
  enum spis_trx_status status;
};

static bool transfer_in_progress;
static process* callback;
static struct spis_trx trx;

spis_init_status
spis_init(process* p)
{
  if (p == NULL) {
    return SPIS_INIT_CALLBACK_IS_NULL;
  }
  transfer_in_progress = false;
  callback = p;
  trx.rx_type = 0;
  trx.rx_size = 0;
  trx.crc = 0;
  trx.rx_received = 0;
  trx.tx_buf = NULL;
  trx.tx_remaining = 0;
  trx.error_code_remaining = 0;
  trx.status = SPIS_TRX_READY;

  SPI_SET_PIN_DIRS_SLAVE();
  SPI_SET_ROLE_SLAVE();
  SPI_SET_DATA_ORDER_MSB();
  SPI_SET_MODE(0,0);
  SPI_SET_CLOCK_RATE_DIV_4();
  SPI_SET_DATA_REG(SPI_TYPE_PREPARING_RESPONSE);
  SPI_TC_INTERRUPT_ENABLE();
  PC_INTERRUPT_ENABLE(SPI_SS_PIN);
  SPI_ENABLE();

  return SPIS_INIT_OK;
}

// Note: this function might reset the SPIF flag!
static inline
void set_spi_data_reg(uint8_t value)
{
  do {
    SPI_SET_DATA_REG(value);
  } while (IS_SPI_WRITE_COLLISION_FLAG_SET());
}

#define DEBUG0 C,5

static void
end_transfer(uint8_t response)
{
  set_spi_data_reg(response);
  trx.error_code_remaining = response;
  trx.status = SPIS_TRX_WAITING_FOR_TRANSFER_TO_END;
}

spis_send_response_status
spis_send_response(uint8_t type, uint8_t* payload, uint8_t size)
{
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (trx.status != SPIS_TRX_WAITING_FOR_CALLBACK) {
      if (trx.status == SPIS_TRX_ABORTED_WHILE_WAITING_FOR_CALLBACK) {
	if (transfer_in_progress) {
	  trx.status = SPIS_TRX_WAITING_FOR_TRANSFER_TO_END;
	} else {
	  set_spi_data_reg(SPI_TYPE_PREPARING_RESPONSE);
	  trx.status = SPIS_TRX_READY;
	}
      }
      SPI_CLEAR_FLAGS();
      return SPIS_SEND_RESPONSE_NO_TRX_IN_PROGRESS;
    }

    // Here we know the status is SPIS_TRX_WAITING_FOR_CALLBACK and
    // consequently we know transfer_in_progress == true.
    if (type >= SPI_ERR_TYPE_MIN) {
      end_transfer(SPI_TYPE_ERR_SLAVE_RESPONSE_INVALID);
      return SPIS_SEND_RESPONSE_INVALID_TYPE;
    }
    if (size > 0 && payload == NULL) {
      end_transfer(SPI_TYPE_ERR_SLAVE_RESPONSE_INVALID);
      return SPIS_SEND_RESPONSE_PAYLOAD_IS_NULL;
    }

    set_spi_data_reg(type);
    trx.tx_buf = payload;
    trx.tx_remaining = size;
    trx.status = SPIS_TRX_SEND_RESPONSE_SIZE;
    SPI_CLEAR_FLAGS();
  }
  return SPIS_SEND_RESPONSE_OK;
}

inline
uint8_t spis_get_rx_size(void)
{
  return trx.rx_size;
}

inline
uint8_t* spis_get_rx_buf(void)
{
  return trx.rx_buf;
}


INTERRUPT(PC_INTERRUPT_VECT(SPI_SS_PIN))
{
  transfer_in_progress = (GET_PIN(SPI_SS_PIN) == 0);
  if (! transfer_in_progress) {
    // The SS pin is high: the master is ending the transfer
    set_spi_data_reg(SPI_TYPE_PREPARING_RESPONSE);
    if (trx.status >= SPIS_TRX_WAITING_FOR_CALLBACK &&
	trx.status < SPIS_TRX_COMPLETED) {
      // Transfer was ended prematurely, after notifying the callback that a
      // message was received
      process_post_event(callback, SPIS_RESPONSE_ERROR, PROCESS_DATA_NULL);
      if (trx.status == SPIS_TRX_WAITING_FOR_CALLBACK) {
	set_spi_data_reg(SPI_TYPE_ERR_SLAVE_NOT_READY);
	trx.error_code_remaining = SPI_TYPE_ERR_SLAVE_NOT_READY;
	trx.status = SPIS_TRX_ABORTED_WHILE_WAITING_FOR_CALLBACK;
      } else {
	trx.status = SPIS_TRX_READY;
      }
      LOG_COUNTER_INC(SPIS_TIMEOUT_WAITING_FOR_CALLBACK);
    } else if (trx.status != SPIS_TRX_ABORTED_WHILE_WAITING_FOR_CALLBACK) {
      trx.status = SPIS_TRX_READY;
    }
    SPI_CLEAR_FLAGS();
  }
}

INTERRUPT(SPI_TC_VECT)
{
  uint8_t data = SPI_GET_DATA_REG();
  switch (trx.status) {
  case SPIS_TRX_READY:
    SPI_SET_DATA_REG(SPI_TYPE_PREPARING_RESPONSE);
    // Master has started a new transfer, first byte is the message type
    trx.rx_type = data;
    trx.rx_received = 0;
    crc16_init(&(trx.crc));
    crc16_update(&(trx.crc), trx.rx_type);
    trx.status = SPIS_TRX_RECEIVING_SIZE;
    break;
  case SPIS_TRX_RECEIVING_SIZE:
    // Second byte is the message size
    trx.rx_size = data;
    if (trx.rx_size > SPIS_RX_BUF_SIZE) {
      // Message size too large for receive buffer
      end_transfer(SPI_TYPE_ERR_MESSAGE_TOO_LARGE);
      LOG_COUNTER_INC(SPIS_MESSAGE_TOO_LARGE);
    } else {
      SPI_SET_DATA_REG(SPI_TYPE_PREPARING_RESPONSE);
      crc16_update(&(trx.crc), trx.rx_size);
      trx.status = SPIS_TRX_RECEIVING_PAYLOAD;
    }
    break;
  case SPIS_TRX_RECEIVING_PAYLOAD:
    if (trx.rx_received < trx.rx_size) {
      SPI_SET_DATA_REG(SPI_TYPE_PREPARING_RESPONSE);
      trx.rx_buf[trx.rx_received] = data;
      trx.rx_received += 1;
      crc16_update(&(trx.crc), data);
      break;
    }
    // Drop to SPIS_TRX_RECEIVING_FOOTER0
  case SPIS_TRX_RECEIVING_FOOTER0:
    if ((trx.crc >> 8) != data) {
      end_transfer(SPI_TYPE_ERR_CRC_FAILURE);
      LOG_COUNTER_INC(SPIS_CRC_FAILURE);
      break;
    }
    SPI_SET_DATA_REG(SPI_TYPE_PREPARING_RESPONSE);
    trx.rx_received += 1;
    trx.status = SPIS_TRX_RECEIVING_FOOTER1;
    break;
  case SPIS_TRX_RECEIVING_FOOTER1:
    if ((trx.crc & 0x00FF) != data) {
      end_transfer(SPI_TYPE_ERR_CRC_FAILURE);
      LOG_COUNTER_INC(SPIS_CRC_FAILURE);
      break;
    }
    SPI_SET_DATA_REG(SPI_TYPE_PREPARING_RESPONSE);
    trx.rx_received += 1;
    trx.status = SPIS_TRX_WAITING_FOR_CALLBACK;
    process_post_event(callback, SPIS_MESSAGE_RECEIVED, PROCESS_DATA_NULL);
    break;
  case SPIS_TRX_WAITING_FOR_CALLBACK:
    // Keep sending SPI_TYPE_PREPARING_RESPONSE until the client process sets a
    // response or the transfer times out
    SPI_SET_DATA_REG(SPI_TYPE_PREPARING_RESPONSE);
    break;
  case SPIS_TRX_SEND_RESPONSE_SIZE:
    SPI_SET_DATA_REG(trx.tx_remaining);
    crc16_update(&(trx.crc), trx.tx_remaining);
    if (trx.tx_remaining > 0) {
      trx.status = SPIS_TRX_SEND_RESPONSE_PAYLOAD;
    } else {
      trx.status = SPIS_TRX_SEND_FOOTER0;
    }
    break;
  case SPIS_TRX_SEND_RESPONSE_PAYLOAD:
    SPI_SET_DATA_REG(*(trx.tx_buf));
    trx.tx_buf += 1;
    trx.tx_remaining -= 1;
    if (trx.tx_remaining == 0) {
      trx.status = SPIS_TRX_SEND_FOOTER0;
    }
    break;
  case SPIS_TRX_SEND_FOOTER0:
    SPI_SET_DATA_REG((uint8_t)(trx.crc >> 8));
    trx.status = SPIS_TRX_SEND_FOOTER1;
    break;
  case SPIS_TRX_SEND_FOOTER1:
    SPI_SET_DATA_REG((uint8_t)(trx.crc & 0x00FF));
    trx.status = SPIS_TRX_COMPLETED;
    break;
  case SPIS_TRX_COMPLETED:
    end_transfer(SPI_TYPE_PREPARING_RESPONSE);
    process_post_event(callback, SPIS_RESPONSE_TRANSMITTED,PROCESS_DATA_NULL);
    LOG_COUNTER_INC(SPIS_TRX_COMPLETED);    
    break;
  default:
    // includes case SPIS_TRX_WAITING_FOR_TRANSFER_TO_END
    // includes case SPIS_TRX_ABORTED_WHILE_WAITING_FOR_CALLBACK

    // Keep sending trx.error_code_remaining
    SPI_SET_DATA_REG(trx.error_code_remaining);
    break;
  }
}
