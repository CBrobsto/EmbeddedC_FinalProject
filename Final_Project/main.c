/*************************************************************************
* main.c
*
* This is the main entry point for the program which implements a cyclic executive
* design pattern. This file also contains a FSM for parsing HTTP requests.
*
* Author:   Cody Brobston
* Date:     12/06/2019
* SER 486 -- Final Project
*
* Copyright(C) 2018, Arizona State University
* All rights reserved
*
*/

/** Includes */
#include "config.h"
#include "delay.h"
#include "dhcp.h"
#include "led.h"
#include "log.h"
#include "rtc.h"
#include "spi.h"
#include "uart.h"
#include "vpd.h"
#include "temp.h"
#include "socket.h"
#include "alarm.h"
#include "wdt.h"
#include "tempfsm.h"
#include "eeprom.h"
#include "ntp.h"
#include "w51.h"
#include "signature.h"
#include "util.h"

/** 'Magic numbers' */
#define HTTP_PORT       8080	/* TCP port for HTTP */
#define SERVER_SOCKET   0

/** FSM states */
#define EMPTY       0
#define REQUEST     1
#define HEADER      2
#define END         3

/** API calls */
#define NONE        0
#define GET         1
#define PUT         2
#define DELETE      3

// scope for helper functions, updated in main loop
int current_temperature = 75;

// initial FSM state
unsigned char protocolState = EMPTY;

// initial API call state
unsigned char apiCall = NONE;

// forward declarations
void parseRecvBuffer();
void getCall();
void putCall();
void deleteCall();

/**********************************
 * main(void)
 *
 * Implements a cyclic executive design pattern.
 *
 * arguments:
 *   none
 *
 * returns:
 *   0 if successful, non-zero if abnormal error
 *   (should never reach the end of the program)
 *
 * changes:
 *   @see included header files. Notable include:
 *      uart.h, led.h, w51.h, eeprom.h, wdt.h
 */
int main(void)
{
	/* Initialize the hardware devices */
	uart_init();
	led_init();
	vpd_init();
	config_init();
	log_init();
	rtc_init();
	spi_init();
	temp_init();
	W5x_init();
	tempfsm_init();

    /* sign the assignment
    * Asurite is the first part of your asu email (before the @asu.edu
    */
    signature_set("cody","brobston","cbrobsto");

    /* configure the W51xx ethernet controller prior to DHCP */
    unsigned char blank_addr[] = {0,0,0,0};
    W5x_config(vpd.mac_address, blank_addr, blank_addr, blank_addr);

    /* loop until a dhcp address has been gotten */
    while (!dhcp_start(vpd.mac_address, 60000UL, 4000UL)) {}
    uart_writestr("local ip: ");uart_writeip(dhcp_getLocalIp());

    /* configure the MAC, TCP, subnet and gateway addresses for the Ethernet controller*/
    W5x_config(vpd.mac_address, dhcp_getLocalIp(), dhcp_getGatewayIp(), dhcp_getSubnetMask());

	/* add a log record for EVENT_TIMESET prior to synchronizing with network time */
	log_add_record(EVENT_TIMESET);

    /* synchronize with network time */
    ntp_sync_network_time(5);

    /* add a log record for EVENT_NEWTIME now that time has been synchronized */
    log_add_record(EVENT_NEWTIME);

    /* start the watchdog timer */
    wdt_init();

    /* log the EVENT STARTUP and send and ALARM to the Master Controller */
    log_add_record(EVENT_STARTUP);
    alarm_send(EVENT_STARTUP);

    /* request start of test if 'T' key pressed - You may run up to 3 tests per
     * day.  Results will be e-mailed to you at the address asurite@asu.edu
     */
    check_for_test_start();

    /* start the first temperature reading and wait 5 seconds before reading again
    * this long delay ensures that the temperature spike during startup does not
    * trigger any false alarms.
    */
    temp_start();
    delay_set(1, 5000);

    while (1) {
        /* reset  the watchdog timer every loop */
        wdt_reset();

        /* update the LED blink state */
        led_update();

        /* if the temperature sensor delay is done, update the current temperature
        * from the temperature sensor, update the temperature sensor finite state
        * machine (which provides hysteresis) and send any temperature sensor
        * alarms (from FSM update).
        */
        if (delay_isdone(1)) {
            /* read the temperature sensor */
            current_temperature = temp_get();
            uart_writedec32(current_temperature);

            if (socket_is_established(SERVER_SOCKET)) {
                uart_writestr("\tsocket established\r\n");
            } else if (socket_is_listening(SERVER_SOCKET)) {
                uart_writestr("\tsocket listening\r\n");
            } else if (socket_is_closed(SERVER_SOCKET)) {
                uart_writestr("\tsocket closed\r\n");
            } else {
                uart_writestr("\tsocket status unknown\r\n");
            }

            /* update the temperature fsm and send any alarms associated with it */
            tempfsm_update(current_temperature,
                           config.hi_alarm,
                           config.hi_warn,
                           config.lo_alarm,
                           config.lo_warn);

            /* restart the temperature sensor delay to trigger in 1 second */
            delay_set(1, 1000);
            temp_start();

        } if (socket_is_closed(SERVER_SOCKET)) {
            /* if socket is closed, open it in passive (listen) mode */
            socket_open(SERVER_SOCKET, HTTP_PORT);
            socket_listen(SERVER_SOCKET);
        }
        /* if there is input to process */
        if (socket_received_line(SERVER_SOCKET)) {
            /* parse and process any pending commands */
            while (socket_received_line(SERVER_SOCKET)) {
               parseRecvBuffer();
            }

            switch (apiCall) {
                case NONE:
                    break;
                case GET:
                    getCall();
                    break;
                case PUT:
                    putCall();
                    break;
                case DELETE:
                    deleteCall();
                    break;
                default:
                    break;
            }

        // disconnect after responding to HTTP request
        socket_disconnect(SERVER_SOCKET);

        } else {
          /* update any pending log write backs */
          log_update();

          /* update any pending config write backs */
          config_update();
       }
    }
	return 0;
}

#define _writeColon         socket_writechar(SERVER_SOCKET, ':')
#define _writeComma         socket_writechar(SERVER_SOCKET, ',')
#define _writeCloseBracket  socket_writechar(SERVER_SOCKET, '}')

/**********************************
 * getCall(void)
 *
 * Helper function to carry out the response for a GET API call.
 *
 * arguments:
 *   none
 *
 * returns:
 *   none
 *
 * changes:
 *   ethernet buffer
 */
void getCall() {

    /** Header */
    socket_writestr(SERVER_SOCKET, "HTTP/1.1 200 OK");
    socket_writestr(SERVER_SOCKET, "Content-Type: application/vnd.api+json");
    socket_writestr(SERVER_SOCKET, "Connection: close\r\n");
    socket_writestr(SERVER_SOCKET, "\r\n");

    /** JSON Payload @see: API documentation */
    socket_writechar(SERVER_SOCKET, '{'); socket_writequotedstring(SERVER_SOCKET, "vpd");
    socket_writestr(SERVER_SOCKET, ":{"); socket_writequotedstring(SERVER_SOCKET, "model");
    _writeColon; socket_writequotedstring(SERVER_SOCKET, vpd.model); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "manufacturer"); _writeColon;
    socket_writequotedstring(SERVER_SOCKET, vpd.manufacturer); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "serial_number"); _writeColon;
    socket_writequotedstring(SERVER_SOCKET, vpd.serial_number); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "manufacture_date"); _writeColon;
    socket_writedate(SERVER_SOCKET, vpd.manufacture_date); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "mac_address"); _writeColon;
    socket_write_macaddress(SERVER_SOCKET, vpd.mac_address); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "country_code"); _writeColon;
    socket_writequotedstring(SERVER_SOCKET, vpd.country_of_origin);
    _writeCloseBracket; // vpd object
    _writeComma; socket_writequotedstring(SERVER_SOCKET, "tcrit_hi"); _writeColon;
    socket_writedec32(SERVER_SOCKET, config.hi_alarm); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "twarn_hi"); _writeColon;
    socket_writedec32(SERVER_SOCKET, config.hi_warn);
    _writeComma; socket_writequotedstring(SERVER_SOCKET, "tcrit_lo"); _writeColon;
    socket_writedec32(SERVER_SOCKET, config.lo_alarm); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "twarn_lo"); _writeColon;
    socket_writedec32(SERVER_SOCKET, config.lo_warn); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "temperature"); _writeColon;
    socket_writedec32(SERVER_SOCKET, current_temperature); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "state"); _writeColon;
    socket_writequotedstring(SERVER_SOCKET, "NORMAL"); _writeComma;
    socket_writequotedstring(SERVER_SOCKET, "log"); socket_writestr(SERVER_SOCKET, ":[");

    for (int i=0; i<log_get_num_entries(); i++) { // with comma
        unsigned long time;
        unsigned char eventnum;
        log_get_record(i, &time, &eventnum);
        socket_writechar(SERVER_SOCKET, '{'); socket_writequotedstring(SERVER_SOCKET, "timestamp"); _writeColon;
        socket_writedate(SERVER_SOCKET, time); _writeComma;
        socket_writequotedstring(SERVER_SOCKET, "event"); _writeColon;
        socket_writedec32(SERVER_SOCKET, eventnum); socket_writechar(SERVER_SOCKET, '}');
        if (i<log_get_num_entries()-1) {
            _writeComma;
        }
    }
    socket_writechar(SERVER_SOCKET, ']');
    _writeCloseBracket; // end of JSON
    socket_writestr(SERVER_SOCKET, "\r\n"); // end of response
}

/**********************************
 * putCall(void)
 *
 * Helper function to carry out the response for a PUT API call.
 *
 * arguments:
 *   none
 *
 * returns:
 *   none
 *
 * changes:
 *   ethernet buffer
 */
void putCall() {
    uart_writestr("PUT command ...\r\n");
}

/**********************************
 * deleteCall(void)
 *
 * Helper function to carry out the response for a DELETE API call.
 *
 * arguments:
 *   none
 *
 * returns:
 *   none
 *
 * changes:
 *   ethernet buffer
 */
void deleteCall() {
    uart_writestr("DELETE command ...\r\n");
}

/**********************************
 * parseRecvBuffer(void)
 *
 * Helper function to carry implement the FSM
 *  and respond to HTTP requests.
 *
 * arguments:
 *   none
 *
 * returns:
 *   none
 *
 * changes:
 *   ethernet buffer
 */
void parseRecvBuffer() {

    // boolean flag for finding API call in recv buffer
    unsigned char found = 0;

    /** FSM */
    switch (protocolState) {

        // Empty recv buffer, only increment when full line is received
        case EMPTY:
            if (socket_received_line(SERVER_SOCKET)) {
                apiCall = NONE;
                protocolState = REQUEST;
            }
            break;

        case REQUEST:
            // search for API call and flush each line until found
            found = 0;
            while (!found) { // TODO: check for next line
                if (socket_recv_compare(SERVER_SOCKET, "GET ")) {
                    apiCall = GET;
                    found = 1;
                } else if (socket_recv_compare(SERVER_SOCKET, "PUT ")) {
                    apiCall = PUT;
                    found = 1;
                } else if (socket_recv_compare(SERVER_SOCKET, "DELETE ")) {
                    apiCall = DELETE;
                    found = 1;
                }
                socket_flush_line(SERVER_SOCKET);
            }
            protocolState = HEADER;
            break;

        case HEADER:
            while (!socket_is_blank_line(SERVER_SOCKET)) {
                socket_flush_line(SERVER_SOCKET); // flush header contents
            }
            socket_flush_line(SERVER_SOCKET); // flush empty line
            if (socket_recv_available(SERVER_SOCKET)>0) {
                protocolState = END;
            } else {
                protocolState = EMPTY;
            }
            break;

        case END:
            while (!socket_is_blank_line(SERVER_SOCKET)) {
                socket_flush_line(SERVER_SOCKET); // flush body contents
            }
            socket_flush_line(SERVER_SOCKET); // flush empty line
            protocolState = EMPTY;
            break;

        // should never be entered
        default:
            uart_writestr("ERR: Default case entered\r\n");
            break;
    }
}
