# Raspberry Pi 2B UART0 Linux Driver

A Linux kernel platform driver for the UART0 serial interface on the Raspberry Pi 2 Model B. It provides a userspace interface for transmitting and receiving serial data through /dev/uart0.

The driver uses interrupt-driven reception, a circular buffer, and IOCTL commands for managing a transmitted-character counter.
Features

    Platform: Raspberry Pi 2 Model B
    Language: C
    Interface: UART0
    Baud rate: 115200
    Receive handling: Interrupt-driven
    Buffering: Circular receive buffer
    Userspace interface: /dev/uart0
    IOCTL support: Reset and retrieve the transmitted-character counter
    Build system: Linux kernel external module using make

## Repository Structure

```text
.
├── feserial.c       # UART0 kernel driver
├── Makefile         # External kernel module build rules
└── README.md        # Project documentation
```

The source filename shown above assumes the driver is saved as feserial.c, matching the Makefile.

## How It Works

The driver registers as a Linux platform driver and matches devices with the device-tree compatible string rtrk,serial.

During initialization, it:

    Maps the UART registers into kernel memory.
    Retrieves and registers the UART interrupt handler.
    Enables runtime power management and the UART clock.
    Configures the UART for 115200 baud.
    Enables UART transmission, reception, and receive interrupts.
    Registers a miscellaneous character device named uart0.

Incoming data is collected by the interrupt handler and stored in a circular buffer. Userspace applications can read received data and transmit data through /dev/uart0.

## Requirements

    Raspberry Pi 2 Model B
    Linux kernel headers matching the running kernel
    Build tools, including make and a compatible C compiler
    A device-tree configuration exposing a platform device compatible with rtrk,serial
    A serial device or USB-to-TTL adapter for external communication, if required

## Building

Install the build tools and kernel headers appropriate for your Raspberry Pi OS installation.

Build the module from the repository directory:

make

The Makefile uses the kernel build directory at:

/lib/modules/$(uname -r)/build

If compilation succeeds, the build produces feserial.ko.

To remove generated build files:

make clean

## Device Tree Configuration

The driver matches the following compatible string:

compatible = "rtrk,serial";

A matching device-tree node must supply the correct memory region, interrupt, and clock resources for the UART hardware.

Note: Adding the compatible string alone does not configure the Raspberry Pi UART for this driver. The device tree must expose a matching platform device, and the UART must not already be claimed by another driver.

## Loading the Module

Load the compiled module:

sudo insmod feserial.ko

Inspect kernel messages:

dmesg | tail -n 50

Check whether the device was registered:

ls -l /dev/uart0

Remove the module when finished:

sudo rmmod feserial

## Using the Device
### Receiving data

Read incoming serial data:

cat /dev/uart0

The driver's read operation waits for received data when the circular buffer is empty.
Transmitting data

Send a message through UART0:

echo "Hello from Raspberry Pi 2B" > /dev/uart0

The driver appends a carriage return (\r) after each newline (\n) during transmission.
### IOCTL Commands

The driver defines two IOCTL command values:
SERIAL_RESET_COUNTER = 0 -	Reset the transmitted-character counter.
SERIAL_GET_COUNTER	= 1 -	Retrieve the transmitted-character counter.

The counter tracks transmitted bytes, including automatically appended carriage returns. Userspace applications must use the corresponding command definitions and pass a valid userspace pointer when retrieving the counter.

## Hardware Notes

    UART0 is typically routed to GPIO14 (TXD) and GPIO15 (RXD) when configured for the appropriate alternate function.
    Use 3.3 V TTL serial signaling. Do not connect RS-232 voltage levels directly to Raspberry Pi GPIO pins.
    Ensure the UART pin configuration and device-tree resources match the target hardware.
    Disable or resolve conflicts with the serial console and other UART drivers before using this driver.
