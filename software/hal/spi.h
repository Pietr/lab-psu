#ifndef SPI_H
#define SPI_H

#include <avr/io.h>

#define DDR_SPI  DDRB
#define DD_SCK   5
#define DD_MISO  4
#define DD_MOSI  3

#define SPI_SET_PIN_DIRS_MASTER  \
  DDR_SPI |= (_BV(DD_MOSI) | _BV(DD_SCK)); \
  DDR_SPI &= ~_BV(DD_MISO)
#define SPI_SET_ROLE_MASTER      SPCR |= _BV(MSTR)
#define SPI_SET_DATA_ORDER_LSB   SPCR |= _BV(DORD)
#define SPI_SET_DATA_ORDER_MSB   SPCR &= ~_BV(DORD)
#define SPI_SET_MODE(cpol,cpha)  \
  SPCR = cpol ? SPCR | _BV(CPOL) : SPCR & ~_BV(CPOL); \
  SPCR = cpha ? SPCR | _BV(CPHA) : SPCR & ~_BV(CPHA)
#define SPI_SET_CLOCK_RATE_div_4 \
  SPCR &= ~(_BV(SPR1) | _BV(SPR0)); \
  SPSR &= ~_BV(SPI2X)
#define SPI_ENABLE               SPCR |= _BV(SPE)


#define SET_SPI_DATA_REG(x)    SPDR = x
#define GET_SPI_DATA_REG       SPDR
#define SET_SPI_STATUS_REG(x)  SPSR = x
#define GET_SPI_STATUS_REG     SPSR

#define SPI_INTERRUPT_FLAG_SET (SPSR & _BV(SPIF))

#endif