# Компилятор
CC = gcc

# Флаги компиляции (Оптимизация, предупреждения, пути к заголовочным файлам LinuxCNC)
CFLAGS = -O2 -Wall -DULAPI -I/usr/include/linuxcnc

# Флаги линковщика (Библиотека HAL)
LDFLAGS = -llinuxcnchal

# Имя итогового исполняемого файла
TARGET = proton_io

# Исходные файлы
SRCS = proton_io.c

# Правило по умолчанию (собирает всё)
all: $(TARGET)

# Правило сборки нашего драйвера
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

# Правило для очистки папки от скомпилированного мусора
clean:
	rm -f $(TARGET)

.PHONY: all clean
