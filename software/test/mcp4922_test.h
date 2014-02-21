/*
 * mcp4922_test.h
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
 * @file mcp4922_test.h
 * @author Pieter Agten <pieter.agten@gmail.com>
 * @date 21 feb 2014
 *
 * Unit tests for the MCP4922 driver.
 */

#ifndef MCP4922_TEST_H
#define MCP4922_TEST_H

#include <check.h>

Suite *mcp4922_suite(void);

#endif
