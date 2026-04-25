# Имя исходного файла
SRC = main.c
# Имя выходного файла (без расширения)
TARGET = modbus_cni

# Инструменты
CC = sdcc
PACK = packihx
PROGRAMMER = minipro

# Флаги компилятора (указываем архитектуру 8051)
CFLAGS = -mmcs51

# Сборка по умолчанию (если просто написать make)
all: $(TARGET).hex

# Правило создания .hex из .ihx
$(TARGET).hex: $(TARGET).ihx
	$(PACK) $(TARGET).ihx > $(TARGET).hex

# Правило компиляции .ihx из .c
$(TARGET).ihx: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET).ihx

# Команда для очистки папки от временного мусора
clean:
	rm -f *.asm *.lst *.mem *.rel *.rst *.sym *.ihx *.hex *.map *.lk

# Команда для прошивки в один клик (когда спаяешь переходник!)
flash: $(TARGET).hex
	$(PROGRAMMER) -p AT89S52 -i -w $(TARGET).hex

