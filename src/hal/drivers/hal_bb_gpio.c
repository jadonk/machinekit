/********************************************************************
* Description:  hal_bb_gpio.c
*               Driver for the BeagleBone GPIO pins
*
* Author: Ian McMahon <imcmahon@prototechnical.com>
* License: GPL Version 2
* Copyright (c) 2013.
*
********************************************************************/


#include "rtapi.h"		
#include "rtapi_app.h"	

#include "hal.h"	

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>

#include "beaglebone_gpio.h"

#if !defined(BUILD_SYS_USER_DSO) 
#error "This driver is for usermode threads only"
#endif
#if !defined(TARGET_PLATFORM_BEAGLEBONE)
#error "This driver is for the BeagleBone platform only"
#endif

#define MODNAME "hal_bb_gpio"

MODULE_AUTHOR("Ian McMahon");
MODULE_DESCRIPTION("Driver for BeagleBone GPIO pins");
MODULE_LICENSE("GPL");

#define PORTS 			 2
#define PINS_PER_PORT 46

typedef struct {
	hal_bit_t* led_pins[4];
	hal_bit_t* input_pins[PINS_PER_PORT]; // array of pointers to bits
	hal_bit_t* output_pins[PINS_PER_PORT]; // array of pointers to bits
} port_data_t;

static port_data_t *port_data;

static const char *modname = MODNAME;

static void write_port(void *arg, long period);
static void read_port(void *arg, long period);

static off_t start_addr_for_port(int port);
static void configure_pin(bb_gpio_pin *pin, char mode);

static int comp_id; 
static int num_ports;

static char *user_leds;
RTAPI_MP_STRING(user_leds, "user leds, comma separated.  0-3");

static char *input_pins;
RTAPI_MP_STRING(input_pins, "input pins, comma separated.  P8 pins add 100, P9 pins add 200");

static char *output_pins;
RTAPI_MP_STRING(output_pins, "output pins, comma separated.  P8 pins add 100, P9 pins add 200");

void configure_control_module() {
	int fd = open("/dev/mem", O_RDWR);

	control_module = mmap(0, CONTROL_MODULE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CONTROL_MODULE_START_ADDR);

	if(control_module == MAP_FAILED) {
		perror("Unable to map Control Module");
		exit(1);
	}

	close(fd);
}

void configure_gpio_port(int n) {
	int fd = open("/dev/mem", O_RDWR);

	gpio_ports[n] = hal_malloc(sizeof(bb_gpio_port));

	gpio_ports[n]->gpio_addr = mmap(0, GPIO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, start_addr_for_port(n));

	if(gpio_ports[n]->gpio_addr == MAP_FAILED) {
		perror("Unable to map GPIO");
		exit(1);
	}

	gpio_ports[n]->oe_reg = gpio_ports[n]->gpio_addr + GPIO_OE;
	gpio_ports[n]->setdataout_reg = gpio_ports[n]->gpio_addr + GPIO_SETDATAOUT;
	gpio_ports[n]->clrdataout_reg = gpio_ports[n]->gpio_addr + GPIO_CLEARDATAOUT;

	close(fd);
}

int rtapi_app_main(void) {
	char name[HAL_NAME_LEN + 1];
	int n, retval;
	char *data, *token;

	num_ports = 1;
	n = 0; // port number... only one for now

	// init driver
	comp_id = hal_init(modname);
	if(comp_id < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: hal_init() failed\n", modname);
		return -1;
	}

	// allocate port memory
	port_data = hal_malloc(num_ports * sizeof(port_data_t));
	if(port_data == 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: hal_malloc() failed\n", modname);
		hal_exit(comp_id);
		return -1;
	}

	// map control module memory
	configure_control_module();

	// configure userleds
	data = user_leds;
	while((token = strtok(data, ",")) != NULL) {
		int led = strtol(token, NULL, 10);

		data = NULL;

		// add hal pin
		retval = hal_pin_bit_newf(HAL_IN, &(port_data[0].led_pins[led]), comp_id, "bb_gpio.0.userled%d", led);

		if(retval < 0) {
			rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: port 0, userled %d could not export pin, err: %d\n", modname, led, retval);
			hal_exit(comp_id);
			return -1;
		}

		int gpio_num = user_led_gpio_pins[led].port_num;
		// configure gpio port if necessary
		if(gpio_ports[gpio_num] == NULL) {
			configure_gpio_port(gpio_num);
		}

		user_led_gpio_pins[led].port = gpio_ports[gpio_num];

		configure_pin(&user_led_gpio_pins[led], 'O');
	}

	// configure input pins
	data = input_pins;
	while((token = strtok(data, ",")) != NULL) {
		int pin = strtol(token, NULL, 10);

		data = NULL; // after the first call, subsequent calls to strtok need to be on NULL

		retval = hal_pin_bit_newf(HAL_IN, &(port_data[n].input_pins[pin]), comp_id, "bb_gpio.%d.in-%02d", n, pin);

		if(retval < 0) {
			rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: port %d, pin %02d could not export pin, err: %d\n", modname, n, pin, retval);
			hal_exit(comp_id);
			return -1;
		}
	}

	// configure output pins
	data = output_pins;
	while((token = strtok(data, ",")) != NULL) {
		int pin = strtol(token, NULL, 10);

		data = NULL; // after the first call, subsequent calls to strtok need to be on NULL

		retval = hal_pin_bit_newf(HAL_OUT, &(port_data[n].output_pins[pin]), comp_id, "bb_gpio.%d.in-%02d", n, pin);

		if(retval < 0) {
			rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: port %d, pin %02d could not export pin, err: %d\n", modname, n, pin, retval);
			hal_exit(comp_id);
			return -1;
		}
	}

	// export functions
	rtapi_snprintf(name, sizeof(name), "bb_gpio.write");
	retval = hal_export_funct(name, write_port, port_data, 0, 0, comp_id);
	if(retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: port %d write funct export failed\n", modname, n);
		hal_exit(comp_id);
		return -1;
	}
	
	rtapi_snprintf(name, sizeof(name), "bb_gpio.read");
	retval = hal_export_funct(name, read_port, port_data, 0, 0, comp_id);
	if(retval < 0) {
		rtapi_print_msg(RTAPI_MSG_ERR, "%s: ERROR: port %d read funct export failed\n", modname, n);
		hal_exit(comp_id);
		return -1;
	}

	rtapi_print_msg(RTAPI_MSG_INFO, "%s: installed driver\n", modname);

	hal_ready(comp_id);

	rtapi_print("port_data is %p\n", port_data);

	return 0;
}

void rtapi_app_exit(void) {

	hal_exit(comp_id);
}

static void write_port(void *arg, long period) {
	int i;
	port_data_t *port = (port_data_t *)arg;

	//rtapi_print("arg is %p\n", arg);

	// set userled states
	for(i=0; i<4; i++) {
		//rtapi_print("\tled addr is %p value is %d\n", port->led_pins[i], *port->led_pins[i]);
		bb_gpio_pin pin = user_led_gpio_pins[i];
		if(*port->led_pins[i] == 0)
			*(pin.port->clrdataout_reg) = (1 << pin.pin_num);
		else
			*(pin.port->setdataout_reg) = (1 << pin.pin_num);
	}
}


static void read_port(void *arg, long period) {

}



off_t start_addr_for_port(int port) {
	switch(port) {
		case 0:
			return GPIO0_START_ADDR;
			break;
		case 1:
			return GPIO1_START_ADDR;
			break;
		case 2:
			return GPIO2_START_ADDR;
			break;
		case 3:
			return GPIO3_START_ADDR;
			break;
		default:
			return -1;
			break;
	}
}


void configure_pin(bb_gpio_pin *pin, char mode) {
	volatile unsigned int *control_reg = control_module + pin->control_offset;
	switch(mode) {
		case 'O':
			*(pin->port->oe_reg) &= ~(1 << pin->pin_num); // 0 in OE is output enable
			*control_reg = PIN_MODE7 | PIN_PULLUD_DISABLED | PIN_RX_DISABLED;
			break;
		case 'I':
			*(pin->port->oe_reg) |= (1 << pin->pin_num); // 1 in OE is input
			*control_reg = PIN_MODE7 | PIN_PULLUD_DISABLED | PIN_RX_ENABLED;
			break;
		case 'U':
			*(pin->port->oe_reg) |= (1 << pin->pin_num); // 1 in OE is input
			*control_reg = PIN_MODE7 | PIN_PULLUD_ENABLED | PIN_PULLUP | PIN_RX_ENABLED;
			break;
		case 'D':
			*(pin->port->oe_reg) |= (1 << pin->pin_num); // 1 in OE is input
			*control_reg = PIN_MODE7 | PIN_PULLUD_ENABLED | PIN_PULLDOWN | PIN_RX_ENABLED;
			break;
		default:
			break;
	}
}