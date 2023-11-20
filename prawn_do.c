#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/structs/clocks.h"



#include "prawn_do.pio.h"
#include "serial.h"

#define LED_PIN 25
// output pins to use, much match pio
#define OUTPUT_PIN_BASE 0
#define OUTPUT_WIDTH 16
// mask which bits we are using
uint32_t output_mask = (OUTPUT_WIDTH - 1) << OUTPUT_PIN_BASE;

#define MAX_DO_CMDS 60000
uint32_t do_cmds[MAX_DO_CMDS];
uint32_t do_cmd_count = 0;


#define SERIAL_BUFFER_SIZE 256
char serial_buf[SERIAL_BUFFER_SIZE];

// STATUS flag
int status;
#define STOPPED 0
#define TRANSITION_TO_RUNNING 1
#define RUNNING 2
#define ABORT_REQUESTED 3
#define ABORTING 4
#define ABORTED 5
#define TRANSITION_TO_STOP 6

#define INTERNAL 0
#define EXTERNAL 1
int clk_status = INTERNAL;
unsigned short debug = 0;
const char ver[6] = "1.0.0";

// Mutex for status
static mutex_t status_mutex;

// Thread safe functions for getting/setting status
int get_status()
{
	mutex_enter_blocking(&status_mutex);
	int status_copy = status;
	mutex_exit(&status_mutex);
	return status_copy;
}

void set_status(int new_status)
{
	mutex_enter_blocking(&status_mutex);
	status = new_status;
	mutex_exit(&status_mutex);
}

void configure_gpio()
{
	gpio_init_mask(output_mask);
	gpio_set_dir_out_masked(output_mask);
}

/*
  Start pio state machine

  This function resets the pio state machine,
  then sets up direct memory access (dma) from the pio to do_cmds.
  Finally, it starts the state machine, which will then run the pio program 
  independently of the CPU.

  This function is inspired by the logic_analyser_arm function on page 46
  of the Raspberry Pi Pico C/C++ SDK manual (except for output, rather than 
  input).
 */
void start_sm(PIO pio, uint sm, uint dma_chan, uint offset, uint hwstart){
	pio_sm_set_enabled(pio, sm, false);

	// Clearing the FIFOs and restarting the state machine to prevent old
	// instructions from persisting into future runs
	pio_sm_clear_fifos(pio, sm);
	pio_sm_restart(pio, sm);
	// send initial wait command (preceeds DMA transfer)
	pio_sm_put_blocking(pio, sm, hwstart);
	// Explicitly jump to reset program to the start
	pio_sm_exec(pio, sm, pio_encode_jmp(offset));

	// Create dma configuration object
	dma_channel_config dma_config = dma_channel_get_default_config(dma_chan);
	// Automatically increment read address as pio pulls data
	channel_config_set_read_increment(&dma_config, true);
	// Don't increment write address (pio should not write anyway)
	channel_config_set_write_increment(&dma_config, false);
	// Set data transfer request signal to the one pio uses
	channel_config_set_dreq(&dma_config, pio_get_dreq(pio, sm, true));
	// Start dma with the selected channel, generated config
	dma_channel_configure(dma_chan, &dma_config,
						  &pio->txf[sm], // write address is fifo for this pio 
										 // and state machine
						  do_cmds, // read address is do_cmds
						  do_cmd_count, // read a total of do_cmd_count entries
						  true); // trigger (start) immediately

	// Actually start state machine
	pio_sm_set_enabled(pio, sm, true);
}
/*
  Stop pio state machine

  This function stops dma, stops the pio state machine,
  and clears the transfer fifos of the state machine.
 */
void stop_sm(PIO pio, uint sm, uint dma_chan){
	dma_channel_abort(dma_chan);
	pio_sm_set_enabled(pio, sm, false);
	pio_sm_clear_fifos(pio, sm);
}

/* Measure system frequencies
From https://github.com/raspberrypi/pico-examples under BSD-3-Clause License
*/
void measure_freqs(void)
{
    uint f_pll_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_SYS_CLKSRC_PRIMARY);
    uint f_pll_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_PLL_USB_CLKSRC_PRIMARY);
    uint f_rosc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_ROSC_CLKSRC);
    uint f_clk_sys = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    uint f_clk_peri = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_PERI);
    uint f_clk_usb = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB);
    uint f_clk_adc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_ADC);
    uint f_clk_rtc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_RTC);

    printf("pll_sys = %dkHz\n", f_pll_sys);
    printf("pll_usb = %dkHz\n", f_pll_usb);
    printf("rosc = %dkHz\n", f_rosc);
    printf("clk_sys = %dkHz\n", f_clk_sys);
    printf("clk_peri = %dkHz\n", f_clk_peri);
    printf("clk_usb = %dkHz\n", f_clk_usb);
    printf("clk_adc = %dkHz\n", f_clk_adc);
    printf("clk_rtc = %dkHz\n", f_clk_rtc);
}

/* Resusitation function that restarts the clock internally if there are any 
   issues with syncing the external clock or invalid changes to the clock
*/
void clk_resus(void) {
	// Restarting the internal clock at 100 MHz
	set_sys_clock_khz(100000, false);

	// Remove the clock syncing functionality
	gpio_set_function(20, GPIO_FUNC_NULL);

	clk_status = INTERNAL;

	// Restart the serial communication
	stdio_init_all();

	// Notify the user that the system clock has been restarted
	printf("System Clock Resus'd\n");
}



void core1_entry() {
	// Setup PIO
	PIO pio = pio0;
	uint sm = pio_claim_unused_sm(pio, true);
	uint dma_chan = dma_claim_unused_channel(true);
	uint offset = pio_add_program(pio, &prawn_do_program); // load prawn_do PIO 
														   // program
	// initialize prawn_do PIO program on chosen PIO and state machine at 
	// required offset
	pio_sm_config pio_config = prawn_do_program_init(pio, sm, 1.f, offset);

	// signal core1 ready for commands
	multicore_fifo_push_blocking(0);

	while(1){
		// wait for message from main core
		uint32_t hwstart = multicore_fifo_pop_blocking();

		set_status(TRANSITION_TO_RUNNING);
		if(debug){
			printf("hwstart: %d\n", hwstart);
		}
		start_sm(pio, sm, dma_chan, offset, hwstart);
		set_status(RUNNING);

		// can save IRQ PIO instruction by using the following check instead
		//while ((dma_channel_is_busy(dma_chan) // checks if dma finished
		//        || pio_sm_is_tx_fifo_empty(pio, sm)) // ensure fifo is empty once dma finishes
		//	     && get_status() != ABORT_REQUESTED) // breaks if Abort requested
		while (!pio_interrupt_get(pio, sm) // breaks if PIO program reaches end
			   && get_status() != ABORT_REQUESTED // breaks if Abort requested
			   ){
			// tight loop checking for run completion
			// exits if program signals IRQ (at end) or abort requested
			continue;
		}
		// ensure interrupt is cleared
		pio_interrupt_clear(pio, sm);

		if(debug){
			printf("Tight execution loop ended\n");
			uint8_t pc = pio_sm_get_pc(pio, sm);
			printf("Program ended at instr %d\n", pc-offset);
		}

		if(get_status() == ABORT_REQUESTED){
			set_status(ABORTING);
			stop_sm(pio, sm, dma_chan);
			set_status(ABORTED);
			if(debug){
				printf("Aborted execution\n");
			}
		}
		else{
			set_status(TRANSITION_TO_STOP);
			stop_sm(pio, sm, dma_chan);
			set_status(STOPPED);
			if(debug){
				printf("Execution stopped\n");
			}
		}
		if(debug){
			printf("Core1 loop ended\n");
		}

	}
}
int main(){

	// initialize status mutex
	mutex_init(&status_mutex);
	
	// Setup serial
	stdio_init_all();

	// Initialize clock functions
	clocks_init();

	// By default, set the system clock to 100 MHz
	set_sys_clock_khz(100000, false);

	// Allow the clock to be restarted in case of any errors
	clocks_enable_resus(&clk_resus);

	// Turn on onboard LED (to indicate device is starting)
	gpio_init(LED_PIN);
	gpio_set_dir(LED_PIN, GPIO_OUT);
	gpio_put(LED_PIN, 1);

	// Finish startup
	printf("Prawn Digital Output online\n");
	gpio_put(LED_PIN, 0);

	multicore_launch_core1(core1_entry);
    multicore_fifo_pop_blocking();

	// Set status to off
	set_status(STOPPED);


	while(1){
		
		// Prompt for user command
		// PIO runs independently, so CPU spends most of its time waiting here
		printf("> ");
		gpio_put(LED_PIN, 1); // turn on LED while waiting for user
		unsigned int buf_len = readline(serial_buf, SERIAL_BUFFER_SIZE);
		gpio_put(LED_PIN, 0);
		int local_status = get_status();

		// Check for command validity, all are at least three characters long
		if(buf_len < 3){
			printf("Invalid command: %s\n", serial_buf);
			continue;
		}

		// // These commands are allowed during buffered execution
		if(strncmp(serial_buf, "ver", 3) == 0) {
			printf("Version: %s\n", ver);
		}
		// Status command: return running status
		else if(strncmp(serial_buf, "sts", 3) == 0){
			printf("run-status:%d clock-status:%d\n", local_status, clk_status);
		}
		// Enable debug mode
		else if (strncmp(serial_buf, "deb", 3) == 0) {
			debug = 1;
		}
		// Disable debug mode
		else if (strncmp(serial_buf, "ndb", 3) == 0) {
			debug = 0;
		}
		// Abort command: stop run by stopping state machine
		else if(strncmp(serial_buf, "abt", 3) == 0){
			if(local_status == RUNNING || local_status == TRANSITION_TO_RUNNING){
				set_status(ABORT_REQUESTED);
			}
			else {
				printf("Can only abort when status is 1 or 2\n");
			}
		}

		// // These commands can only happen in manual mode
		else if (local_status != ABORTED && local_status != STOPPED){
			printf("Cannot execute command %s during buffered execution.", serial_buf);
		}
		
		// Clear command: empty the buffered outputs
		else if(strncmp(serial_buf, "cls", 3) == 0){
			do_cmd_count = 0;
		}
		// Run command: start state machine
		else if(strncmp(serial_buf, "run", 3) == 0){
			multicore_fifo_push_blocking(1);
		}
		// Software start: start state machine without waiting for trigger
		else if(strncmp(serial_buf, "swr", 3) == 0){
			multicore_fifo_push_blocking(0);
		}
		// Manual update of outputs
		else if(strncmp(serial_buf, "man", 3) == 0){
			unsigned int manual_state;
			int parsed = sscanf(serial_buf, "%*s %x", &manual_state);
			if(parsed != 1){
				printf("invalid request\n");
			}
			else{
				configure_gpio();
				gpio_put_masked(output_mask, manual_state);
			}
		}
		// Get current output state
		else if(strncmp(serial_buf, "gto", 3) == 0){
			unsigned int all_state = gpio_get_all();
			unsigned int manual_state = (output_mask & all_state) >> OUTPUT_PIN_BASE;
			printf("%x\n", manual_state);
		}
		// Add command: read in hexadecimal integers separated by newlines, 
		// append to command array
		else if(strncmp(serial_buf, "add", 3) == 0){
			
			while(do_cmd_count < MAX_DO_CMDS-3){
				uint32_t output;
				uint32_t reps;
				unsigned short num_inputs = 0;

				do {
				// Read in the command provided by the user
				// FORMAT: <output> <reps> <REPS = 0: Full Stop(0) or Indefinite Wait(1)>
				buf_len = readline(serial_buf, SERIAL_BUFFER_SIZE);

				// Check if the user inputted "end", and if so, exit add mode
				if(buf_len >= 3){
					if(strncmp(serial_buf, "end", 3) == 0){
						break;
					}
				}

				// Read the input provided in the serial buffer into the 
				// output, reps, and wait_num variables. Also storing the return
				// value of sscanf (number of variables successfully read in)
				// to determine if the user wants to program a stop/wait
				num_inputs = sscanf(serial_buf, "%x %x", &output, &reps);

				} while (num_inputs < 2);

				if(strncmp(serial_buf, "end", 3) == 0){
					break;
				}

				//DEBUG MODE:
				// Printing to the user what the program received as input
				// for the output, reps, and optionally wait if the user inputted
				// that
				if (debug) {
					printf("Output: %x\n", output);
					printf("Number of Reps: %d\n", reps);

					if (reps == 0){
						printf("Wait\n");
					}
				}

				// confirm output is valid
				if(output & ~output_mask){
					printf("Invalid output specification %x\n", output);
				}

				// Reading in the 16-bit word to output to the pins
				do_cmds[do_cmd_count] = output;
				do_cmd_count++;

				do_cmds[do_cmd_count] = reps;

				// If reps is not zero, this adjusts them from the number
				// of 10ns reps to reps adding onto the base 50 ns pulse
				// width
				if (do_cmds[do_cmd_count] != 0) {
					do_cmds[do_cmd_count] -= 4;
				}
				do_cmd_count++;

				
			}
			if(do_cmd_count == MAX_DO_CMDS-1){
				printf("Too many DO commands (%d). Please use resources more efficiently or increase MAX_DO_CMDS and recompile.\n", MAX_DO_CMDS);
			}
		}
		// Dump command: print the currently loaded buffered outputs
		else if(strncmp(serial_buf, "dmp", 3) == 0){
			// Dump
			for(int i = 0; do_cmd_count > 0 && i < do_cmd_count - 1; i++){
				// Printing out the output word
				printf("do_cmd: %04x\n", do_cmds[i]);
				i++;

				// Either printing out the number of reps, or if the number
				// of reps equals zero printing out whether it is a full stop
				// or an indefinite wait
				if (do_cmds[i] == 0){
					printf("\tWait\n");
				}
				else {
					printf("\tnumber of reps: %d\n", do_cmds[i]+4);
				}
				
			}
		}
		else if (strncmp(serial_buf, "clk", 3) == 0){
			if (strncmp(serial_buf + 4, "ext", 3) == 0) {
				// Sync the clock with the input from gpio pin 20
				clock_configure_gpin(clk_sys, 20, 100000000, 100000000);
				clk_status = EXTERNAL;
			} else if (strncmp(serial_buf + 4, "int", 3) == 0) {
				// Set the internal clock back to 100 MHz
				set_sys_clock_khz(100000, false);

				// Remove the clock sync from pin 20
				gpio_set_function(20, GPIO_FUNC_NULL);

				clk_status = INTERNAL;
			} else if (strncmp(serial_buf + 4, "set", 3) == 0) {
				unsigned int clock_freq;
				// Read in the clock frequency requested
				sscanf(serial_buf + 8, "%d", &clock_freq);
				if (clk_status == INTERNAL) {
					// If the clock is internal, set that clock to the requested
					// frequency
					set_sys_clock_khz(clock_freq / 1000, false);
				} else {
					// If the clock is external, set the expected and requested
					// frequency to the inputted clock frequency
					clock_configure_gpin(clk_sys, 20, clock_freq, clock_freq);
				}
			}
		}
		// Editing the current command with the instruction provided by the
		// user 
		// FORMAT: <output> <reps> <REPS = 0: Full Stop(0) or Indefinite Wait(1)>
		else if (strncmp(serial_buf, "edt", 3) == 0) {
			if (do_cmd_count > 0) {
				uint32_t output;
				uint32_t reps;
				unsigned short num_inputs;
			
				do {
				// Reading in an instruction from the user serial input
				readline(serial_buf, SERIAL_BUFFER_SIZE);

				// Storing the input from the user into the respective output,
				// reps, and waits variables to be stored in memory
				num_inputs = sscanf(serial_buf, "%x %x", &output, &reps);

				} while (num_inputs < 2);
				// Immediately replacing the output and reps stored for the
				// last sequence with the newly inputted values
				do_cmds[do_cmd_count - 2] = output;
				do_cmds[do_cmd_count - 1] = reps;

			}
		
		}
		// Printing out the latest digital output command added to the current 
		// running program
		else if (strncmp(serial_buf, "cur", 3) == 0) {
				printf("Output: %x\n", do_cmds[do_cmd_count - 2]);
				printf("Reps: %d\n", do_cmds[do_cmd_count - 1] + 4);
				if(do_cmds[do_cmd_count - 1] == 0){
					printf("Wait\n");
				}
		}
		// Measure system frequencies
		else if(strncmp(serial_buf, "frq", 3) == 0) {
			measure_freqs();
		}
		else{
			printf("Invalid command: %s\n", serial_buf);
		}
	}
}
