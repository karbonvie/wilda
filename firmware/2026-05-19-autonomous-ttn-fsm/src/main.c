// --- BEGIN AUTONOMOUS TTN MAIN.C ---

/* * Zephyr RTOS Core Libraries 
 * These provide the operating system fundamentals, hardware abstraction, 
 * and specific peripheral drivers (UART for serial, GPIO for pins, USB for console).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <stdio.h>

// --- TTN OTAA CREDENTIALS ---
// These are the unique cryptographic keys mapping this specific hardware to The Things Network.
#define NB2_DEVEUI "0000000000000000"
#define NB2_APPEUI "0000000000000000"
#define NB2_APPKEY "00000000000000000000000000000000"

// --- HARDWARE PIN MAPPING ---
// D10 / P0.27 controls the Si9433BDY P-Channel MOSFET to supply 3.3V to the RAK.
// D11 / P0.06 is wired to the RAK's hardware RESET pin.
#define RAK_PWR_PIN 27
#define RAK_RST_PIN 6

// --- ASYNCHRONOUS DATA BUFFER ---
// A Ring Buffer is a circular array in memory. Because UART bytes arrive unpredictably, 
// a hardware interrupt catches them instantly and safely drops them in this "bucket" 
// so the main program can read them later without dropping data.
#define RING_BUF_SIZE 1024
RING_BUF_DECLARE(rak_rx_ringbuf, RING_BUF_SIZE);

// --- DEVICETREE BINDINGS ---
// Zephyr uses a "Devicetree" to map physical hardware to software pointers.
static const struct device *usb_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
static const struct device *rak_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct device *gpio0   = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/*
 * Helper: Print to USB Console
 * Takes a standard C-string and pushes it out the USB port to the PC terminal byte-by-byte.
 */
static void usb_print(const char *str)
{
    while (*str) {
        uart_poll_out(usb_dev, *str++);
    }
}

/*
 * Interrupt Service Routine (ISR): RAK UART Receiver
 * This function is triggered automatically by the CPU hardware the microsecond a byte 
 * arrives from the RAK modem. It moves the byte into the Ring Buffer and exits instantly.
 */
static void rak_isr(const struct device *dev, void *user_data)
{
    uint8_t buffer[64];
    int len;

    uart_irq_update(dev);
    if (uart_irq_rx_ready(dev)) {
        // Read available bytes from the hardware register
        len = uart_fifo_read(dev, buffer, sizeof(buffer));
        if (len > 0) {
            // Push them safely into the circular buffer
            ring_buf_put(&rak_rx_ringbuf, buffer, len);
        }
    }
}

/*
 * Helper: Send Command to RAK
 * Pushes a command string out of the nRF52's TX pin to the RAK's RX pin.
 */
static void rak_send(const char *cmd)
{
    while (*cmd) {
        uart_poll_out(rak_dev, *cmd++);
    }
}

/*
 * The Core Parser: Wait For Response
 * This function prevents the CPU from locking up. It continuously checks the Ring Buffer 
 * for incoming characters, assembling them into a line to see if they match the 'expected' word.
 * If the 'timeout_ms' is reached before a match, it gracefully aborts.
 */
static bool wait_for_response(const char *expected, uint32_t timeout_ms)
{
    uint32_t start_time = k_uptime_get_32();
    char line_buffer[128];
    int buf_idx = 0;
    uint8_t c;

    memset(line_buffer, 0, sizeof(line_buffer));

    // Loop until we run out of time
    while ((k_uptime_get_32() - start_time) < timeout_ms) {
        
        // If a byte is waiting in the Ring Buffer...
        if (ring_buf_get(&rak_rx_ringbuf, &c, 1) == 1) {
            
            // Echo it to the PC so the human can see what the modem is saying
            uart_poll_out(usb_dev, c); 

            // Add the character to our search string
            if (buf_idx < sizeof(line_buffer) - 1) {
                line_buffer[buf_idx++] = (char)c;
                
                // Check if the expected word (e.g., "OK") is inside our assembled string
                if (strstr(line_buffer, expected) != NULL) {
                    return true; // Match found! Exit successfully.
                }
            } else {
                // Buffer is full. Shift the string left by 64 bytes to make room 
                // for new characters without losing the tail end of the data.
                memmove(line_buffer, line_buffer + 64, 64);
                buf_idx = 64;
                memset(line_buffer + buf_idx, 0, 64);
            }
        } else {
            // No bytes available yet. Go to sleep for 5ms to save power and let 
            // other RTOS tasks run, rather than burning CPU cycles.
            k_sleep(K_MSEC(5)); 
        }
    }
    return false; // Time ran out before the expected string arrived.
}

int main(void)
{
    uint32_t dtr = 0;
    char cmd_buf[128]; // Temporary memory for formatting AT commands

    // Check if the Devicetree hardware is actually accessible
    if (!device_is_ready(gpio0) || !device_is_ready(usb_dev) || !device_is_ready(rak_dev)) return -ENODEV;

    // 1. Initial State: Force the RAK modem OFF and held in RESET
    gpio_pin_configure(gpio0, RAK_PWR_PIN, GPIO_OUTPUT_INACTIVE | GPIO_OUTPUT_INIT_HIGH);
    gpio_pin_configure(gpio0, RAK_RST_PIN, GPIO_OUTPUT_ACTIVE);
    gpio_pin_set_raw(gpio0, RAK_RST_PIN, 0); // 0V holds the chip in reset

    // 2. Don't wait for the human to open the USB terminal, do it automatically
    usb_enable(NULL);
    // while (!dtr) {
    //     uart_line_ctrl_get(usb_dev, UART_LINE_CTRL_DTR, &dtr);
    //     k_sleep(K_MSEC(100));
    // }

    usb_print("\r\n[SYS] Host terminal connected. Powering RAK...\r\n");

    // 3. Power ON Sequence
    k_sleep(K_MSEC(100));
    gpio_pin_set_raw(gpio0, RAK_PWR_PIN, 0); // Drop MOSFET gate to 0V (Allows 3.3V to flow)
    k_sleep(K_MSEC(100));
    gpio_pin_set_raw(gpio0, RAK_RST_PIN, 1); // Raise RESET to 3.3V (Modem boots up)

    // 4. Activate the UART Listener
    uart_irq_callback_set(rak_dev, rak_isr);
    uart_irq_rx_enable(rak_dev);

    // Wait 2 seconds for the RAK's bootloader text to finish, then clear out the buffer
    k_sleep(K_SECONDS(2));
    ring_buf_reset(&rak_rx_ringbuf);

    // --- FSM STATE 1: PROVISIONING ---
    // We send basic config commands and give the modem 2 seconds to reply "OK" to each
    usb_print("\r\n[FSM] Configuring Network Parameters...\r\n");
    rak_send("AT+NWM=1\r\n");   wait_for_response("OK", 2000); // Set LoRaWAN Mode
    rak_send("AT+BAND=5\r\n");  wait_for_response("OK", 2000); // Set Region to US915
    rak_send("AT+MASK=0002\r\n"); wait_for_response("OK", 2000); // TTN specific sub-band
    rak_send("AT+NJM=1\r\n");   wait_for_response("OK", 2000); // Set OTAA Join Mode
    rak_send("AT+CLASS=A\r\n"); wait_for_response("OK", 2000); // Set lowest power class

    usb_print("\r\n[FSM] Injecting OTAA Keys...\r\n");
    // snprintf stitches our Keys into the "AT+..." structure dynamically
    snprintf(cmd_buf, sizeof(cmd_buf), "AT+DEVEUI=%s\r\n", NB2_DEVEUI);
    rak_send(cmd_buf); wait_for_response("OK", 2000);

    snprintf(cmd_buf, sizeof(cmd_buf), "AT+APPEUI=%s\r\n", NB2_APPEUI);
    rak_send(cmd_buf); wait_for_response("OK", 2000);

    snprintf(cmd_buf, sizeof(cmd_buf), "AT+APPKEY=%s\r\n", NB2_APPKEY);
    rak_send(cmd_buf); wait_for_response("OK", 2000);

    // --- FSM STATE 2: NETWORK JOIN ---
    usb_print("\r\n[FSM] Initiating OTAA Join Sequence...\r\n");
    rak_send("AT+JOIN=1:0:10:8\r\n");
    
    // An OTAA join requires RF back-and-forth with the gateway. We give it 15 seconds 
    // to detect the specific success string. If it fails, we halt the program to prevent loop abuse.
    if (wait_for_response("+EVT:JOINED", 15000)) {
        usb_print("\r\n[FSM] JOIN SUCCESSFUL.\r\n");
    } else {
        usb_print("\r\n[FSM] JOIN FAILED OR TIMEOUT. Halting.\r\n");
        while(1) { k_sleep(K_SECONDS(1)); }
    }

    // --- FSM STATE 3: AUTONOMOUS LOOP ---
    // The node is now active on the network. We cycle between sleep and transmit.
    while (1) {
        
        // Put the RAK modem into its 1.7uA deep sleep mode. The session is kept in RAM.
        usb_print("\r\n[FSM] Sending AT+SLEEP...\r\n");
        rak_send("AT+SLEEP\r\n");
        wait_for_response("OK", 2000);
        
        // Put the nRF52 to sleep for 30 seconds to simulate waiting for a wildlife event
        usb_print("\r\n[FSM] Modem asleep. RTOS waiting 30 seconds...\r\n");
        k_sleep(K_SECONDS(30)); 

        // --- WAKE SEQUENCE ---
        // Sending a command instantly would corrupt the first byte because the RAK 
        // is asleep. We "knock" by sending a blank AT command, ignore the garbage response, 
        // and wait 50ms for the silicon clocks to spin up.
        usb_print("\r\n[FSM] Sending Wake Knock...\r\n");
        rak_send("AT\r\n");
        wait_for_response("OK", 500); 
        k_sleep(K_MSEC(50)); 

        // --- TRANSMIT SEQUENCE ---
        // Send a fixed 8-byte payload representing Tag ID 1, Wake Event, 1000mV, 2000mV.
        usb_print("\r\n[FSM] Transmitting Fixed Payload...\r\n");
        rak_send("AT+SEND=2:0101010003E807D0\r\n");
        
        // Step A: The local AT parser acknowledges the command syntax is correct
        wait_for_response("OK", 2000);

        // Step B: The MAC layer takes over, transmits the RF, and listens for the TTN 
        // receive windows (which takes several seconds). We wait up to 10 seconds 
        // for the MAC layer to announce it has finished the process.
        if (wait_for_response("+EVT:TX_DONE", 10000)) {
            usb_print("\r\n[FSM] UPLINK COMPLETE.\r\n");
        } else {
            usb_print("\r\n[FSM] UPLINK TIMEOUT.\r\n");
        }
        
        // Loop repeats, putting the modem immediately back to sleep.
    }
    return 0;
}

// --- END AUTONOMOUS TTN MAIN.C ---