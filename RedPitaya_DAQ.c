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

#include <netinet/tcp.h>

#include "axi_header.h"

// --- Hardware Constants ---
#define AXI_GPIO_ADDRESS  0x41200000
#define RANGE             64000
#define IIC_RANGE         64000
#define PORT              5000
#define DESIRED_SAMPLE_RATE_HZ 1000
#define CTR_CLK_RATE      125000000
#define HEADER_SIZE       22
#define UDP_PORT  55001

#define BUF_SAMPLES 1000 // Flushes to SD card every 1,000 samples
#define CHUNK_SAMPLES 100

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
    int sample_frequency;
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
    else if (id == 0x71) { // MPU9250
    strcpy(s->name, "MPU9250");
    s->split_read = false;
    s->num_channels = 9;
    s->data_reg_start = 0x3B;

    if (is_spi) {
        axi_spi_write(map, 0x6B, 0x01); // Wake up Accel/Gyro
        usleep(5000);
        printf("  -> MPU9250 (SPI) initialized (6-axis only for now).\n");
    } else {
        // --- I2C PATH ---
        axi_iic_write_byte(map, addr, 0x6B, 0x01); // Wake
        usleep(5000);

        // Disable I2C Master and Enable Bypass to see the Magnetometer (0x0C)
        axi_iic_write_byte(map, addr, 0x6A, 0x00);
        axi_iic_write_byte(map, addr, 0x37, 0x02);
        usleep(5000);

        // This call is safe here because we are actually on an I2C bus
        axi_iic_write_byte(map, 0x0C, 0x0A, 0x16);
        printf("  -> MPU9250 (I2C) initialized (9-axis enabled).\n");
    }
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
            axi_iic_write_byte(map, addr, 0x7F, 0x00); // Select Bank 0
            axi_iic_write_byte(map, addr, 0x06, 0x01); // Wake
            usleep(10000);

            // Disable I2C Master (Register 0x03 on Bank 0)
            axi_iic_write_byte(map, addr, 0x03, 0x00);

            // Enable I2C Bypass (INT_PIN_CFG is Register 0x0F on the ICM)
            axi_iic_write_byte(map, addr, 0x0F, 0x02);
            usleep(5000);

            // 1. Force Power-Down Mode First
            axi_iic_write_byte(map, 0x0C, 0x31, 0x00);
            usleep(5000);

            // 2. Set to Continuous Measurement Mode 4 (100 Hz)
            // (0x08 = 100Hz, 0x06 = 50Hz, 0x04 = 20Hz, 0x02 = 10Hz)
            axi_iic_write_byte(map, 0x0C, 0x31, 0x08);
            usleep(5000);

            printf("  -> ICM20948 (I2C) initialized (9-axis enabled).\n");
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
    ctx->sample_frequency = 1000;

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
            else if (id_mpu == 0x71) {
                printf("  -> Found MPU9250!\n");
                identify_and_add_sensor(ctx, map, id_mpu, 0x68, false);
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
        uint8_t id_val = 0;

        axi_spi_read(map, 0x75, &id_val, 1);

        if (id_val == 0x71) {
            printf("Found MPU9250 on SPI! (ID: 0x%02X)\n", id_val);
            identify_and_add_sensor(ctx, map, id_val, 0x00, true);
        }
        else {
            axi_spi_write(map, 0x7F, 0x00);
            usleep(1000);
            axi_spi_read(map, 0x00, &id_val, 1);

            if (id_val == 0xEA) {
                printf("Found ICM20948 on SPI! (ID: 0x%02X)\n", id_val);
                identify_and_add_sensor(ctx, map, id_val, 0x00, true);
            } else {
                printf("Empty (Returned 0x%02X at 0x00 and 0x75)\n", id_val);
                munmap(map, IIC_RANGE);
            }
        }
    }

    close(fd);
    printf("--- Discovery Complete (Active Sensors: %d) ---\n", ctx->active_sensor_count);
    return 0;
}

// --- Streaming Logic --
static int run_stream(int client_fd, int udp_fd, struct sockaddr_in *client_udp_addr, HardwareContext *ctx) {
    uint32_t ticks_per_sample = CTR_CLK_RATE / ctx->sample_frequency;
    int bytes_per_payload = ctx->total_channels * 2;
    int bytes_per_packet = HEADER_SIZE + bytes_per_payload;
    int chunk_size_bytes = bytes_per_packet * CHUNK_SAMPLES; // e.g., 40 * 100 = 4000 bytes

    // --- Memory Allocations ---
    uint8_t *chunk_buffer = malloc(chunk_size_bytes);
    int16_t *frame_buffer = malloc(bytes_per_payload);
    struct timespec *timestamps = malloc(CHUNK_SAMPLES * sizeof(struct timespec));

    char cmd[32];
    bool record = false;
    int buf_idx = 0;
    uint32_t sample_number = 0;

    // --- Header Template Setup ---
    // We create a template header that we will stamp onto every single sample in the chunk
    uint8_t header_template[HEADER_SIZE];
    int32_t offset = 0, bpb = bytes_per_payload, elm = 2, ns = 1;
    int16_t dtype = 3;

    memcpy(header_template + 0,  &offset, 4);
    memcpy(header_template + 4,  &bpb, 4);
    memcpy(header_template + 8,  &dtype, 2);
    memcpy(header_template + 10, &elm, 4);
    memcpy(header_template + 14, &ctx->total_channels, 4);
    memcpy(header_template + 18, &ns, 4);

    *ctx->gpio_reset = 1; usleep(1); *ctx->gpio_reset = 0;
    uint32_t last_counter = *ctx->gpio_counter;

    FILE *fp = NULL;

    while (1) {
        uint32_t now = *ctx->gpio_counter;
        if ((now - last_counter) < ticks_per_sample) {
            int n = recv(client_fd, cmd, sizeof(cmd) - 1, MSG_DONTWAIT);
            if (n > 0) {
                cmd[n] = '\0';
                if (strstr(cmd, "STOP")) {
                    /*
                    if (record && fp != NULL && buf_idx > 0) {
                        fwrite(chunk_buffer, 1, buf_idx * bytes_per_packet, fp);
                        fflush(fp);
                    }*/
                    if (record && fp != NULL) { fclose(fp); fp = NULL; }
                    free(chunk_buffer); free(frame_buffer);free(timestamps);
                    return 0;
                }
                if (strstr(cmd, "RECORD ON")) {
                        system("rw");
                        time_t rawtime; struct tm *timeinfo;
                        char time_str[20]; char filename[128];
                        time(&rawtime);
                        timeinfo = localtime(&rawtime);
                        strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", timeinfo);
                        sprintf(filename, "/root/Measurements/recording_%s.bin", time_str);

                        fp = fopen(filename, "wb");
                        if (fp == NULL) {
                        perror("Failed to open file");
                        write(client_fd, "ERROR_FILE\n", 11);
                        continue;
                        }
                        record = true;
                        }
                if (strstr(cmd, "RECORD OFF")) {
                    record = false;
                    /*if (fp != NULL && buf_idx > 0) {
                        fwrite(chunk_buffer, 1, buf_idx * bytes_per_packet, fp);
                        fflush(fp);
                        buf_idx = 0;
                    }*/
                    if (fp != NULL) { fclose(fp); fp = NULL; }
                }
            } else if (n == 0) {
                /*if (record && fp != NULL && buf_idx > 0) {
                    fwrite(chunk_buffer, 1, buf_idx * bytes_per_packet, fp);
                }*/
                free(chunk_buffer); free(frame_buffer); free(timestamps);
                return -1;
            }
            usleep(100); continue;
        }
        last_counter += ticks_per_sample;

        clock_gettime(CLOCK_REALTIME, &timestamps[buf_idx]);

        memset(frame_buffer, 0, bytes_per_payload);
        int current_byte_offset = 0;

        for (int i = 0; i < ctx->active_sensor_count; i++) {
            SensorInstance *s = &ctx->sensors[i];
            int16_t *channel_out = (int16_t*)(((uint8_t*)frame_buffer) + current_byte_offset);

            if (strcmp(s->name, "MPU9250") == 0) {
                uint8_t raw[12];
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, 0x3B, raw, 12);

                uint8_t mag_raw[7];
                axi_iic_read_n_bytes(s->axi_map, 0x0C, 0x03, mag_raw, 7);

                for (int j = 0; j < 6; j++) {
                    int16_t val = (int16_t)((raw[j*2] << 8) | raw[j*2 + 1]);
                    channel_out[j] = val;
                }

                for (int j = 0; j < 3; j++) {
                    int16_t val = (int16_t)(mag_raw[j*2] | (mag_raw[j*2 + 1] << 8));
                    int32_t amplified = (int32_t)val * 16;
                    if (amplified > 32767) amplified = 32767;
                    if (amplified < -32768) amplified = -32768;
                    channel_out[j+6] = (int16_t)amplified;
                }
            }
            else if (strcmp(s->name, "ICM20948") == 0) {
                // 1. Read Accel & Gyro into temp array
                uint8_t raw[12];
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, 0x2D, raw, 12);

                // 2. Reconstruct Accel & Gyro (Big-Endian)
                for (int j = 0; j < 6; j++) {
                    int16_t val = (int16_t)((raw[j*2] << 8) | raw[j*2 + 1]);
                    channel_out[j] = val;
                }

                // 3. Magnetometer (AK09916) Hardware Synchronization
                uint8_t st1 = 0;
                axi_iic_read_n_bytes(s->axi_map, 0x0C, 0x10, &st1, 1);

                // Declare static to hold the last valid reading across high-speed loops
                static int16_t last_mag[3] = {0};

                // Bit 0 of ST1 is the Data Ready (DRDY) flag
                if (st1 & 0x01) {
                    // Read 8 bytes starting at 0x11 (Mag X, Y, Z + Reserved + ST2)
                    uint8_t mag_raw[8];
                    axi_iic_read_n_bytes(s->axi_map, 0x0C, 0x11, mag_raw, 8);

                    // Reconstruct Magnetometer (Little-Endian)
                    for (int j = 0; j < 3; j++) {
                        int16_t val = (int16_t)(mag_raw[j*2] | (mag_raw[j*2 + 1] << 8));

                        // Digital amplification to match the MPU9250 bounds in Open Ephys
                        int32_t amplified = (int32_t)val * 16;
                        if (amplified > 32767) amplified = 32767;
                        if (amplified < -32768) amplified = -32768;

                        last_mag[j] = (int16_t)amplified;
                    }
                }

                // 4. Output the Mag data (either the fresh data or the held staircase step)
                for (int j = 0; j < 3; j++) {
                    channel_out[j+6] = last_mag[j];
                }
            }
            else if (s->is_spi) {
                uint8_t raw[32];
                axi_spi_read(s->axi_map, s->data_reg_start, raw, s->num_channels * 2);
                for (int j = 0; j < s->num_channels; j++) {
                    int16_t val = (int16_t)((raw[j*2] << 8) | raw[j*2 + 1]);
                    channel_out[j] = val;
                }
            }
            else if (s->split_read) {
                uint8_t raw[12];
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, 0x3B, raw, 6);
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, 0x43, raw + 6, 6);
                for (int j = 0; j < s->num_channels; j++) {
                    int16_t val = (int16_t)((raw[j*2] << 8) | raw[j*2 + 1]);
                    channel_out[j] = val;
                }
            }
            else {
                uint8_t raw[32];
                axi_iic_read_n_bytes(s->axi_map, s->i2c_addr, s->data_reg_start, raw, s->num_channels * 2);
                for (int j = 0; j < s->num_channels; j++) {
                    int16_t val = (int16_t)((raw[j*2] << 8) | raw[j*2 + 1]);
                    channel_out[j] = val;
                }
            }
            current_byte_offset += (s->num_channels * 2);
        }


        // Calculate where in the giant buffer this specific packet goes
        uint8_t *current_packet_ptr = chunk_buffer + (buf_idx * bytes_per_packet);

        // Stamp the current sample number into our template header, then copy it to the chunk
        memcpy(header_template + 0, &sample_number, 4);
        memcpy(current_packet_ptr, header_template, HEADER_SIZE);

        // Copy the sensor payload right after the header
        memcpy(current_packet_ptr + HEADER_SIZE, frame_buffer, bytes_per_payload);

        buf_idx++;
        sample_number++; // Increment by 1 for EVERY sample to keep timestamps perfect

        // --- MASSIVE WRITE/SEND PHASE ---
        if (buf_idx >= CHUNK_SAMPLES) {

            // Write the full packets (Headers + Data) to the SD card.
            if (record && fp != NULL) {
                for (int k = 0; k < buf_idx; k++) {
                    fwrite(&timestamps[k].tv_sec,  sizeof(timestamps[k].tv_sec),  1, fp);
                    fwrite(&timestamps[k].tv_nsec, sizeof(timestamps[k].tv_nsec), 1, fp);
                    uint8_t *payload_ptr = chunk_buffer + (k * bytes_per_packet) + HEADER_SIZE;
                    fwrite(payload_ptr, 1, bytes_per_payload, fp);
                }
            }

            // Blast all 100 packets over the network at once
            //if (send(client_fd, chunk_buffer, chunk_size_bytes, 0) <= 0) break;
            sendto(udp_fd, chunk_buffer, chunk_size_bytes, 0,(struct sockaddr *)client_udp_addr, sizeof(*client_udp_addr));

            buf_idx = 0;
        }
    }

    free(chunk_buffer); free(frame_buffer); free(timestamps);
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

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("UDP socket failed"); return 1; }
    struct sockaddr_in udp_addr = {0};
    udp_addr.sin_family      = AF_INET;
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    udp_addr.sin_port        = htons(UDP_PORT);
    if (bind(udp_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("UDP bind failed"); return 1;
    }
    int udp_sndbuf = 1024 * 1024;
    setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &udp_sndbuf, sizeof(udp_sndbuf));
    printf("UDP data socket bound on port %d\n", UDP_PORT);

    listen(server_fd, 1);
    printf("Server is listening on Port %d. Waiting for client...\n", PORT);

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            perror("Accept failed");
            continue; // Go back to waiting if the connection drops
        }
        int flag = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) < 0) {
            perror("Could not set TCP_NODELAY");
        } else {
            printf("Client connected! TCP_NODELAY enabled. Stream unblocked.\n");
        }

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
                write(client_fd, "STARTED\n", 8);
                //if (run_stream(client_fd, &ctx, fp) < 0) { fclose(fp); break; }
                struct sockaddr_in client_udp_addr = {0};
                struct sockaddr_in tcp_peer = {0};
                socklen_t peer_len = sizeof(tcp_peer);
                getpeername(client_fd, (struct sockaddr *)&tcp_peer, &peer_len);
                client_udp_addr.sin_family = AF_INET;
                client_udp_addr.sin_addr   = tcp_peer.sin_addr;
                client_udp_addr.sin_port   = htons(UDP_PORT);

                if (run_stream(client_fd, udp_fd, &client_udp_addr, &ctx) < 0) {
                printf("Exited run stream \n");
                break;
                }
                write(client_fd, "STOPPED\n", 8);
            }

            else if (strstr(buffer, "FREQ:")) {
                int new_freq = 0;
                char *freq_ptr = strstr(buffer, "FREQ:");

                if (sscanf(freq_ptr, "FREQ:%d", &new_freq) == 1) {
                    if (new_freq < 100) {
                        new_freq = 100;
                    }
                    else if (new_freq > 2000) {
                        new_freq = 2000;
                    }
                    new_freq = ((new_freq + 5) / 10) * 10;

                    ctx.sample_frequency = new_freq;
                    printf("Sample rate securely set to %u Hz\n", ctx.sample_frequency);

                    write(client_fd, "OK\n", 3);
                }
            }
        }
        close(client_fd);
    }
    return 0;
}
