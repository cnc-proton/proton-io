CNC PROTON IO Component for LinuxCNC

Highly optimized user-space HAL component for LinuxCNC designed to interface with CNC PROTON expansion modules via high-speed RS485 (Baudrate 460800).
Why CNC PROTON IO?

Standard Modbus components in LinuxCNC can be slow or complex to configure. This driver is hard-coded for speed and simplicity.

    Auto-Discovery: No need to manually define pins. The driver scans the bus and creates pins based on the connected module's hardware ID.

    High Performance: Custom serial implementation bypasses standard overhead, supporting 460,800 bps for low-latency motion control.

    Robustness: Integrated hardware watchdog management and error-retry logic.

Supported Hardware

The driver automatically detects and configures these module types:

    Type 1: 8 Digital Inputs / 16 Digital Outputs

    Type 2: 8 Digital Inputs / 8 Digital Outputs

    Type 3: 16 Digital Inputs / 8 Digital Outputs

Installation
Prerequisites

Make sure you have LinuxCNC development headers:
Bash

sudo apt-get install linuxcnc-dev

Build and Install
Bash

git clone https://github.com/cnc-proton/proton-io.git
cd proton-io
make
sudo make install

HAL Configuration

Add the following line to your .hal file to load the driver:
Kodo fragmentas

loadusr -W proton_io -wd 10 -p 5

Parameters:

    -wd [ticks]: Watchdog timeout (default: 10). Sets how fast the outputs should turn off if communication is lost.

    -p [ms]: Transmission delay/polling interval (default: 10ms).

Auto-Generated Pins

Once loaded, the driver creates pins following this structure:

    proton_io.board-XX.online: (Bit, Out) True if module is responding.

    proton_io.board-XX.in-YY: (Bit, Out) State of digital input.

    proton_io.board-XX.in-YY-not: (Bit, Out) Inverted state of digital input.

    proton_io.board-XX.out-YY: (Bit, In) Connect your HAL signals here to drive physical outputs.

License

[GPLv2 or later (Standard for LinuxCNC components)]
