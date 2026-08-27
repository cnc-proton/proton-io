#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <hal.h>
#include <rtapi_math.h>

// --- Обход конфликта заголовков Linux для кастомных скоростей ---
#ifndef BOTHER
#define BOTHER 0010000
#endif

struct termios2 {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[19];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

#ifndef TCGETS2
#define TCGETS2 _IOR('T', 0x2A, struct termios2)
#endif
#ifndef TCSETS2
#define TCSETS2 _IOW('T', 0x2B, struct termios2)
#endif
// -----------------------------------------------------------------

// --- Глобальные переменные ---
int hal_comp_id;
int keep_running = 1;
int wd_ticks = 10;
int tx_delay = 10;
int timeout_ms = 30;
int cycle_delay_us = 1000;

#define MAX_BOARDS 32
#define MAX_IO_BITS 32

// 1. Структура HAL-пинов (ОБЯЗАНА ЖИТЬ В SHARED MEMORY)
struct hal_pins {
    hal_bit_t *online;
    hal_bit_t *in[MAX_IO_BITS];
    hal_bit_t *in_not[MAX_IO_BITS];
    hal_bit_t *out[MAX_IO_BITS];
};

// 2. Локальная структура программы (Живет в обычной памяти)
struct board_data {
    int active;
    int type;
    int in_count;
    int out_count;
    struct hal_pins *pins; // Указатель на разделяемую память
    int error_count;
} boards[MAX_BOARDS];

// --- CRC16 Modbus ---
uint16_t crc16(uint8_t *buffer, uint16_t length) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= buffer[i];
        for (uint16_t j = 0; j < 8; j++) {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else { crc >>= 1; }
        }
    }
    return crc;
}

// --- Настройка UART на 750000 бод ---
int setup_serial(const char *device) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) return -1;
    
    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio) < 0) { close(fd); return -1; }
    tio.c_cflag &= ~CBAUD;
    tio.c_cflag |= BOTHER;
    tio.c_ispeed = 750000;
    tio.c_ospeed = 750000;
    tio.c_cflag |= (CS8 | CREAD | CLOCAL);
    tio.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK);
    tio.c_lflag = 0;
    tio.c_oflag = 0;
    if (ioctl(fd, TCSETS2, &tio) < 0) { close(fd); return -1; }
    
    tcflush(fd, TCIFLUSH);
    return fd;
}

// --- Отправка и чтение Modbus ---
// --- Отправка и чтение Modbus (С ЗАЩИТОЙ ОТ ФАНТОМНЫХ БАЙТОВ) ---
int modbus_transaction(int fd, uint8_t *tx_buf, int tx_len, uint8_t *rx_buf, int expected_rx_len) {
    tcflush(fd, TCIFLUSH);
    if (write(fd, tx_buf, tx_len) != tx_len) return 0;

    int total_read = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint8_t temp_buf[64]; // Буфер с запасом под мусор

    // Читаем с запасом до 5 байт мусора (expected_rx_len + 5)
    while (total_read < expected_rx_len + 5) {
        if (poll(&pfd, 1, timeout_ms) > 0) {
            int n = read(fd, temp_buf + total_read, sizeof(temp_buf) - total_read);
            if (n > 0) {
                total_read += n;
                
                // Скользящее окно: ищем начало валидного кадра в мусоре
                for (int i = 0; i <= total_read - expected_rx_len; i++) {
                    // Пакет должен начинаться с нашего адреса и кода функции
                    if (temp_buf[i] == tx_buf[0] && temp_buf[i+1] == tx_buf[1]) {
                        uint16_t calc_crc = crc16(temp_buf + i, expected_rx_len - 2);
                        uint16_t rx_crc = temp_buf[i + expected_rx_len - 2] | (temp_buf[i + expected_rx_len - 1] << 8);
                        
                        if (calc_crc == rx_crc) {
                            // CRC сошлась! Копируем чистый пакет и выходим с победой
                            memcpy(rx_buf, temp_buf + i, expected_rx_len);
                            return 1;
                        }
                    }
                }
            } else break;
        } else {
            break; // Таймаут
        }
    }
    return 0; // Пакет не найден
}

int write_register(int fd, uint8_t addr, uint16_t reg, uint16_t val) {
    uint8_t tx[8] = {addr, 0x06, reg >> 8, reg & 0xFF, val >> 8, val & 0xFF, 0, 0};
    uint16_t crc = crc16(tx, 6);
    tx[6] = crc & 0xFF; tx[7] = crc >> 8;
    uint8_t rx[8];
    return modbus_transaction(fd, tx, 8, rx, 8);
}

int read_registers(int fd, uint8_t addr, uint16_t reg, uint16_t count, uint16_t *out_data) {
    uint8_t tx[8] = {addr, 0x03, reg >> 8, reg & 0xFF, count >> 8, count & 0xFF, 0, 0};
    uint16_t crc = crc16(tx, 6);
    tx[6] = crc & 0xFF; tx[7] = crc >> 8;
    int rx_len = 5 + (count * 2);
    uint8_t rx[32];
    
    if (modbus_transaction(fd, tx, 8, rx, rx_len)) {
        for (int i = 0; i < count; i++) out_data[i] = (rx[3 + i*2] << 8) | rx[4 + i*2];
        return 1;
    }
    return 0;
}

int create_board_pins(int addr) {
    int retval;

    boards[addr].pins = hal_malloc(sizeof(struct hal_pins));
    if (boards[addr].pins == NULL) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Нет HAL shared memory для модуля ID=%d\n", addr);
        return -ENOMEM;
    }
    memset(boards[addr].pins, 0, sizeof(struct hal_pins));

    retval = hal_pin_bit_newf(HAL_OUT, &(boards[addr].pins->online), hal_comp_id,
                              "proton_io.board-%02d.online", addr);
    if (retval < 0) return retval;
    *(boards[addr].pins->online) = 1;

    for (int p = 0; p < boards[addr].in_count; p++) {
        retval = hal_pin_bit_newf(HAL_OUT, &(boards[addr].pins->in[p]), hal_comp_id,
                                  "proton_io.board-%02d.in-%02d", addr, p);
        if (retval < 0) return retval;
        *(boards[addr].pins->in[p]) = 0;
        retval = hal_pin_bit_newf(HAL_OUT, &(boards[addr].pins->in_not[p]), hal_comp_id,
                                  "proton_io.board-%02d.in-%02d-not", addr, p);
        if (retval < 0) return retval;
        *(boards[addr].pins->in_not[p]) = 1;
    }

    for (int p = 0; p < boards[addr].out_count; p++) {
        retval = hal_pin_bit_newf(HAL_IN, &(boards[addr].pins->out[p]), hal_comp_id,
                                  "proton_io.board-%02d.out-%02d", addr, p);
        if (retval < 0) return retval;
        *(boards[addr].pins->out[p]) = 0;
    }

    return 0;
}

void clear_board_inputs(int addr) {
    for (int p = 0; p < boards[addr].in_count; p++) {
        *(boards[addr].pins->in[p]) = 0;
        *(boards[addr].pins->in_not[p]) = 1;
    }
}

void exit_handler() { keep_running = 0; }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-wd") == 0 && i + 1 < argc) wd_ticks = atoi(argv[++i]);
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) tx_delay = atoi(argv[++i]);
        if (strcmp(argv[i], "-to") == 0 && i + 1 < argc) timeout_ms = atoi(argv[++i]);
        if (strcmp(argv[i], "-cycle-us") == 0 && i + 1 < argc) cycle_delay_us = atoi(argv[++i]);
    }
    if (timeout_ms < 1) timeout_ms = 1;
    if (cycle_delay_us < 0) cycle_delay_us = 0;

    hal_comp_id = hal_init("proton_io");
    if (hal_comp_id < 0) return -1;

    int fd = setup_serial("/dev/ttyUSB0");
    if (fd < 0) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Не удалось открыть /dev/ttyUSB0\n");
        hal_exit(hal_comp_id);
        return -1;
    }

    rtapi_print_msg(RTAPI_MSG_INFO, "Proton IO: Сканирование шины... (WD=%d, DELAY=%d, TO=%dms, CYCLE=%dus)\n",
                    wd_ticks, tx_delay, timeout_ms, cycle_delay_us);

    int boards_found = 0;
    for (int addr = 1; addr < MAX_BOARDS; addr++) {
        uint16_t reg_data[4];
        if (read_registers(fd, addr, 0, 4, reg_data)) {
            int retval;
            int type = reg_data[3];
            
            if (type == 1) { boards[addr].in_count = 8; boards[addr].out_count = 16; }
            else if (type == 2) { boards[addr].in_count = 8; boards[addr].out_count = 8; }
            else if (type == 3) { boards[addr].in_count = 16; boards[addr].out_count = 8; }
            else {
                rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Модуль ID=%d вернул неизвестный тип %d, пропуск\n", addr, type);
                continue;
            }

            boards[addr].active = 1;
            boards[addr].type = type;

            boards_found++;
            rtapi_print_msg(RTAPI_MSG_INFO, "Proton IO: Найден модуль ID=%d (Тип: %d)\n", addr, boards[addr].type);

            write_register(fd, addr, 2, wd_ticks); usleep(2000);
            write_register(fd, addr, 3, tx_delay); usleep(2000);

            retval = create_board_pins(addr);
            if (retval < 0) {
                rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Ошибка создания HAL-пинов для ID=%d: %d\n", addr, retval);
                close(fd);
                hal_exit(hal_comp_id);
                return -1;
            }
        }
    }

    if (boards_found == 0) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Модули не найдены!\n");
        close(fd); hal_exit(hal_comp_id); return -1;
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Не удалось заблокировать память: %s\n", strerror(errno));
    }

    struct sched_param param = { .sched_priority = 90 };
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Не удалось включить SCHED_FIFO: %s\n", strerror(errno));
    }

    hal_ready(hal_comp_id); 

    while (keep_running) {
        for (int addr = 1; addr < MAX_BOARDS; addr++) {
            if (!boards[addr].active) continue;

            uint16_t in_val[1] = {0};
            int read_ok = read_registers(fd, addr, 0, 1, in_val);
            int comm_ok = read_ok;

            if (read_ok) {
                for (int p = 0; p < boards[addr].in_count; p++) {
                    int bit_state = (in_val[0] & (1 << p)) ? 1 : 0;
                    *(boards[addr].pins->in[p]) = bit_state;
                    *(boards[addr].pins->in_not[p]) = !bit_state;
                }
            } else {
                // Не держим последнее состояние входов при потере чтения с модуля.
                clear_board_inputs(addr);
            }

            uint16_t out_val = 0;
            // Если входы не читаются, связь с модулем сомнительна: гасим выходы безопасным нулем.
            if (read_ok && boards[addr].error_count <= 5) {
                for (int p = 0; p < boards[addr].out_count; p++) {
                    if (*(boards[addr].pins->out[p])) out_val |= (1u << p);
                }
            }

            if (cycle_delay_us > 0) usleep(cycle_delay_us);

            if (!write_register(fd, addr, 1, out_val)) {
                comm_ok = 0;
            }

            if (comm_ok) {
                if (*(boards[addr].pins->online) == 0) {
                    rtapi_print_msg(RTAPI_MSG_INFO, "Proton IO: Связь с модулем ID=%d восстановлена\n", addr);
                }
                boards[addr].error_count = 0;
                *(boards[addr].pins->online) = 1;
            } else {
                boards[addr].error_count++;
                if (boards[addr].error_count > 5) {
                    if (*(boards[addr].pins->online) != 0) {
                        rtapi_print_msg(RTAPI_MSG_ERR, "Proton IO: Потеря связи с модулем ID=%d\n", addr);
                    }
                    *(boards[addr].pins->online) = 0;
                }
            }
        }
    }

    close(fd);
    hal_exit(hal_comp_id);
    return 0;
}
