#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "rp.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdbool.h>
#include <setjmp.h>
#include <signal.h>

#include "axi_header.h"

// --- Hardware Constants ---
#define AXI_GPIO_ADDRESS  0x41200000
#define RANGE             64000
#define IIC_RANGE         64000
#define PORT              5000
#define DESIRED_SAMPLE_RATE_HZ 100
#define CTR_CLK_RATE      125000000
#define HEADER_SIZE       22

// --- Discovery Logic Structures ---
typedef struct {
    char name[16];
    void *axi_map;
    uint8_t i2c_addr;
    int num_channels;
    uint8_t data_reg_start;
    bool active;
    bool split_read;
    bool is_spi; // NEW: Flag to tell run_stream which protocol to use
} SensorInstance;

typedef struct {
    void *axi_gpio_map;
    volatile uint32_t *gpio_counter;
    volatile uint32_t *gpio_reset;

    SensorInstance sensors[6]; // UPDATED: 4 I2C slots + 2 SPI slots
    int active_sensor_count;
    int total_channels;
} HardwareContext;

// Global jump buffer for the watchdog
static sigjmp_buf watchdog_bucket;

// Signal handler that triggers when the AXI bus hangs
void watchdog_handler(int sig) {
    siglongjmp(watchdog_bucket, 1);
}

// --- Helper: Sensor Identification ---
static void identify_and_add_sensor(HardwareContext *ctx, void *map, uint8_t id, uint8_t addr, bool is_spi) {
    SensorInstance *s = &ctx->sensors[ctx->active_sensor_count];
    s->axi_map = map;
    s->i2c_addr = addr;
    s->is_spi = is_spi;
    s->active = true;

    if (id == 0x68 && !is_spi) { // MPU6050
        strcpy(s->name, "MPU6050");
        s->split_read = true;
        s->num_channels = 6;
        s->data_reg_start = 0x3B;
        axi_iic_write_byte(map, addr, 0x6B, 0x80);
        usleep(10000); // 10ms Reset delay
        axi_iic_write_byte(map, addr, 0x6B, 0x01);
        usleep(5000);  // 5ms Wake delay
    }
    else if (id == 0xEA) { // ICM20948
        strcpy(s->name, "ICM20948");
        s->split_read = false;
        s->num_channels = 9;
        s->data_reg_start = 0x2D;

        if (is_spi) {
            axi_spi_write(map, 0x7F, 0x00); // Bank 0
            axi_spi_write(map, 0x06, 0x01); // Wake
            usleep(10000);
        } else {
            axi_iic_write_byte(map, addr, 0x7F, 0x00); // Bank 0
            axi_iic_write_byte(map, addr, 0x06, 0x01); // Wake
            usleep(10000);
        }
    }
    else if (id == 0xA0 && !is_spi) { // BNO055
        strcpy(s->name, "BNO055");
        s->split_read = false;
        s->num_channels = 9;
        s->data_reg_start = 0x08;
        axi_iic_write_byte(map, addr, 0x3D, 0x0C); // NDOF Mode
        usleep(20000);
    }

    ctx->total_channels += s->num_channels;
    ctx->active_sensor_count++;
}

// --- Init Hardware (AXI Sweep) ---
static int init_hardware(HardwareContext *ctx) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Failed to open /dev/mem");
        return -1;
    }

    // Register the handler for the SIGALRM signal
    signal(SIGALRM, watchdog_handler);

    // Map the GPIO for the hardware timer/counter
    ctx->axi_gpio_map = mmap(NULL, RANGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AXI_GPIO_ADDRESS);
    if (ctx->axi_gpio_map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    ctx->gpio_counter = (volatile uint32_t *)(ctx->axi_gpio_map + 0x0);
    ctx->gpio_reset   = (volatile uint32_t *)(ctx->axi_gpio_map + 0x8);

    uint32_t i2c_bases[] = {0x41600000, 0x41610000, 0x41620000, 0x41630000};
    ctx->active_sensor_count = 0;
    ctx->total_channels = 0;

    for (int i = 0; i < 4; i++) {
        printf("Probing AXI I2C Slot %d at 0x%08X...\n", i, i2c_bases[i]);
        fflush(stdout);

        void* map = mmap(NULL, IIC_RANGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, i2c_bases[i]);
        if (map == MAP_FAILED) continue;

        uint8_t id_mpu = 0, id_icm = 0, id_bno = 0;
        bool sensor_found = false;

        // --- PHASE 1: Probe for 0x68 (MPU / ICM) ---
        if (sigsetjmp(watchdog_bucket, 1) == 0) {
            alarm(1); // 1-second fuse for 0x68
            axi_iic_initialize(map);

            // This will hang if 0x68 is empty, triggering the else block below
            axi_iic_write_byte(map, 0x68, 0x7F, 0x00);
            usleep(1000);
            axi_iic_read_n_bytes(map, 0x68, 0x75, &id_mpu, 1);
            axi_iic_read_n_bytes(map, 0x68, 0x00, &id_icm, 1);
            alarm(0); // Success, turn off alarm

            if (id_mpu == 0x68) {
                printf("  -> Found MPU6050!\n");
                identify_and_add_sensor(ctx, map, id_mpu, 0x68, false);
                sensor_found = true;
            } else if (id_icm == 0xEA) {
                printf("  -> Found ICM20948!\n");
                identify_and_add_sensor(ctx, map, id_icm, 0x68, false);
                sensor_found = true;
            }
        } else {
            alarm(0); // Clear the alarm and safely proceed to Phase 2.
        }

        if (sensor_found) continue; // If we found one, move to the next physical slot!

        // --- PHASE 2: Probe for 0x28 (BNO055) ---
        // We only reach this code if Phase 1 triggered a hang (meaning no 0x68 device)
        if (sigsetjmp(watchdog_bucket, 1) == 0) {
            alarm(1);

            axi_iic_initialize(map);
            // Probe BNO055
            axi_iic_read_n_bytes(map, 0x28, 0x00, &id_bno, 1);
            alarm(0); // Success, turn off alarm

            if (id_bno == 0xA0) {
                printf("  -> Found BNO055!\n");
                identify_and_add_sensor(ctx, map, id_bno, 0x28, false);
            } else {
                printf("  -> Slot %d is completely empty.\n", i);
                munmap(map, IIC_RANGE);
            }
        } else {
            alarm(0);
            printf("  -> Slot %d is empty (Bus hung on all addresses).\n", i);
            munmap(map, IIC_RANGE);
        }
    }
    // --- PHASE 3: SPI SENSOR DISCOVERY ---
    printf("\n--- Starting SPI Discovery ---\n");
    fflush(stdout);

    uint32_t spi_bases[] = {0x41E00000, 0x41E10000};
    int num_spi_slots = sizeof(spi_bases) / sizeof(spi_bases[0]);

    for (int i = 0; i < num_spi_slots; i++) {
        printf("Probing AXI SPI Slot %d at 0x%08X... ", i, spi_bases[i]);
        fflush(stdout);

        void* map = mmap(NULL, IIC_RANGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, spi_bases[i]);
        if (map == MAP_FAILED) {
            printf("[!] mmap failed.\n");
            continue;
        }

        axi_spi_initialize(map);

        uint8_t id_icm = 0;
        axi_spi_write(map, 0x7F, 0x00); // Force Bank 0
        usleep(1000);
        axi_spi_read(map, 0x00, &id_icm, 1); // Read WHO_AM_I

        if (id_icm == 0xEA) {
            printf("Found ICM20948 on SPI! (ID: 0x%02X)\n", id_icm);
            identify_and_add_sensor(ctx, map, id_icm, 0x00, true);
        } else {
            printf("Empty (Returned 0x%02X)\n", id_icm);
            munmap(map, IIC_RANGE);
        }
    }

    close(fd);
    printf("--- Discovery Complete (Active Sensors: %d) ---\n", ctx->active_sensor_count);
    return 0;
}

// --- Streaming Logic ---
static int run_stream(int client_fd, HardwareContext *ctx, FILE *log_file) {
    uint32_t ticks_per_sample = CTR_CLK_RATE / DESIRED_SAMPLE_RATE_HZ;
    int bytes_per_frame = ctx->total_channels * 2;
    uint8_t *packet = malloc(HEADER_SIZE + bytes_per_frame);
    int16_t *data_buffer = malloc(bytes_per_frame);
    char cmd[32];
    bool record = false;

    int32_t offset = 0, bpb = bytes_per_frame, elm = 2, ns = 1;
    int16_t dtype = 3;
    memcpy(packet + 0,  &offset, 4);
    memcpy(packet + 4,  &bpb, 4);
    memcpy(packet + 8,  &dtype, 2);
    memcpy(packet + 10, &elm, 4);
    memcpy(packet + 14, &ctx->total_channels, 4);
    memcpy(packet + 18, &ns, 4);

    *ctx->gpio_reset = 1; usleep(1); *ctx->gpio_reset = 0;
    uint32_t last_counter = *ctx->gpio_counter;

    while (1) {
        uint32_t now = *ctx->gpio_counter;
        if ((now - last_counter) < ticks_per_sample) {
            int n = recv(client_fd, cmd, sizeof(cmd) - 1, MSG_DONTWAIT);
            if (n > 0) {
                cmd[n] = '\0';
                if (strstr(cmd, "STOP")) { free(packet); free(data_buffer); return 0; }
                if (strstr(cmd, "RECORD ON")) record = true;
                if (strstr(cmd, "RECORD OFF")) record = false;
            } else if (n == 0) { free(packet); free(data_buffer); return -1; }
            usleep(100); continue;
        }
        last_counter += ticks_per_sample;

        memset(data_buffer, 0, bytes_per_frame);

        int current_byte_offset = 0;
        for (int i = 0; i < ctx->active_sensor_count; i++) {
            SensorInstance *s = &ctx->sensors[i];
            uint8_t *dest = ((uint8_t*)data_buffer) + current_byte_offset;

            if(s->is_spi) {
                axi_spi_read(s->axi_map, s->data_reg_start, dest, s->num_channels * 2);
            }

           else if (s->split_read) {
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, 0x3B, dest, 6);
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, 0x43, dest + 6, 6);
            } else {
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, s->data_reg_start, dest, s->num_channels * 2);
            }

            for (int j = 0; j < s->num_channels; j++) {
                uint8_t *pair = &dest[j * 2];
                uint8_t temp = pair[0];
                pair[0] = pair[1];
                pair[1] = temp;
            }

            current_byte_offset += (s->num_channels * 2);
        }

        if (record && log_file != NULL) {
            fwrite(data_buffer, 1, bytes_per_frame, log_file);
            static int flush_ctr = 0;
            if (++flush_ctr >= 100) { fflush(log_file); flush_ctr = 0; }
        }

        memcpy(packet + HEADER_SIZE, data_buffer, bytes_per_frame);
        if (send(client_fd, packet, HEADER_SIZE + bytes_per_frame, 0) <= 0) break;
    }

    free(packet); free(data_buffer);
    return 0;
}


int main(void) {
    HardwareContext ctx = {0};
    printf(" Starting Server \n");
    if (init_hardware(&ctx) < 0) {
        fprintf(stderr, "Error: Hardware initialization failed!\n");
        return 1;
    }
    printf("Hardware initialized. Active Sensors: %d, Total Channels: %d\n",ctx.active_sensor_count, ctx.total_channels);

    int server_fd, client_fd, opt = 1;
    struct sockaddr_in servaddr = {0};
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed"); // This will tell you if Port 5000 is busy
        return 1;
    }

    listen(server_fd, 1);
    printf("Server is listening on Port %d. Waiting for client...\n", PORT);

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        char buffer[64];
        while (1) {
            memset(buffer, 0, sizeof(buffer));
            int n = read(client_fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) break;
            buffer[n] = '\0';

            if (strstr(buffer, "REDPITAYA")) {
                char msg[64];
                sprintf(msg, "OK CHANNELS:%d\n", ctx.total_channels);
                write(client_fd, msg, strlen(msg));
            }
            else if (strstr(buffer, "START")) {
                system("rw");
                time_t rawtime; struct tm *timeinfo;
                char time_str[20]; char filename[128];
                time(&rawtime);
                timeinfo = localtime(&rawtime);
                strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", timeinfo);
                sprintf(filename, "/root/Measurements/recording_%s.bin", time_str);

                FILE *fp = fopen(filename, "wb");
                if (fp == NULL) {
                    perror("Failed to open file");
                    write(client_fd, "ERROR_FILE\n", 11);
                    continue;
                }
                write(client_fd, "STARTED\n", 8);
                if (run_stream(client_fd, &ctx, fp) < 0) { fclose(fp); break; }
                fclose(fp);
                system("sync");
                write(client_fd, "STOPPED\n", 8);
            }
        }
        close(client_fd);
    }
    return 0;
}
