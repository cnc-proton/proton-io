# Source file name
SRC = main.c
# Output file name (without extension)
TARGET = modbus_cni

# Tools
CC = sdcc
PACK = packihx
PROGRAMMER = minipro

# Compiler flags (8051 architecture)
CFLAGS = -mmcs51

# Default build target (run `make`)
all: $(TARGET).hex

# Create .hex from .ihx
$(TARGET).hex: $(TARGET).ihx
	$(PACK) $(TARGET).ihx > $(TARGET).hex

# Compile .ihx from .c
$(TARGET).ihx: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET).ihx

# Remove build artifacts
clean:
	rm -f *.asm *.lst *.mem *.rel *.rst *.sym *.ihx *.hex *.map *.lk

# Flash the MCU in one step (requires programmer adapter)
flash: $(TARGET).hex
	$(PROGRAMMER) -p AT89S52 -i -w $(TARGET).hex
