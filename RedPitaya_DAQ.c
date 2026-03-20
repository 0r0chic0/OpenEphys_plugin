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
#include<time.h>

#include "axi_header.h"

#define AXI_IIC_ADDRESS   0x41600000
#define AXI_GPIO_ADDRESS  0x41200000
#define RANGE             64000
#define IIC_RANGE         64000

#define mpu_addr          0x68
#define power_management  0x6B
#define mpu_ACC           0x3B
#define MPU_WHO_AM_I      0x75
#define USER_CTRL         0x6A

#define DESIRED_SAMPLE_RATE_HZ    100
#define CTR_CLK_RATE              125000000

#define PORT              5000

#define NUM_CHANNELS      6
#define NUM_SAMPLES       1
#define ELEMENT_SIZE      2
#define DATA_TYPE         3
#define BYTES_PER_BUFFER  (NUM_CHANNELS * NUM_SAMPLES * ELEMENT_SIZE)
#define HEADER_SIZE       22

typedef struct {
    void *axi_iic_map;
    void *axi_gpio_map;
    volatile uint32_t *gpio_counter;
    volatile uint32_t *gpio_reset;
} HardwareContext;

static int init_hardware(HardwareContext *ctx)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return -1;
    }

    ctx->axi_iic_map = mmap(NULL, IIC_RANGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AXI_IIC_ADDRESS);
    if (ctx->axi_iic_map == MAP_FAILED) {
        perror("mmap axi_iic");
        close(fd);
        return -1;
    }

    ctx->axi_gpio_map = mmap(NULL, RANGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AXI_GPIO_ADDRESS);
    if (ctx->axi_gpio_map == MAP_FAILED) {
        perror("mmap axi_gpio");
        munmap(ctx->axi_iic_map, IIC_RANGE);
        close(fd);
        return -1;
    }

    close(fd);

    ctx->gpio_counter = (volatile uint32_t *)(ctx->axi_gpio_map + 0x0);
    ctx->gpio_reset   = (volatile uint32_t *)(ctx->axi_gpio_map + 0x8);

    axi_iic_initialize(ctx->axi_iic_map);
    axi_iic_write2_bytes(ctx->axi_iic_map, mpu_addr, power_management, 0x01, 0x00);
    axi_iic_write_byte(ctx->axi_iic_map, mpu_addr, USER_CTRL, 0x00);

    uint8_t who_am_i = 0;
    axi_iic_read_n_bytes(ctx->axi_iic_map, mpu_addr, MPU_WHO_AM_I, &who_am_i, 1);
    printf("MPU WHO_AM_I = 0x%02X\n", who_am_i);

    return 0;
}

static void cleanup_hardware(HardwareContext *ctx)
{
    if (ctx->axi_iic_map && ctx->axi_iic_map != MAP_FAILED)
        munmap(ctx->axi_iic_map, IIC_RANGE);

    if (ctx->axi_gpio_map && ctx->axi_gpio_map != MAP_FAILED)
        munmap(ctx->axi_gpio_map, RANGE);

    rp_Release();
}

static int run_stream(int client_fd, HardwareContext *ctx, FILE *log_file)
{
    uint8_t MPU6050_data[14];
    uint8_t header[HEADER_SIZE];
    uint8_t packet[HEADER_SIZE + BYTES_PER_BUFFER];
    char cmd[32];
    
    bool record = false;

    int32_t offset = 0;
    int32_t bytesPerBuffer = BYTES_PER_BUFFER;
    int16_t dataType = DATA_TYPE;
    int32_t elementSize = ELEMENT_SIZE;
    int32_t numChannels = NUM_CHANNELS;
    int32_t numSamples = NUM_SAMPLES;

    memcpy(&header[0],  &offset,         4);
    memcpy(&header[4],  &bytesPerBuffer, 4);
    memcpy(&header[8],  &dataType,       2);
    memcpy(&header[10], &elementSize,    4);
    memcpy(&header[14], &numChannels,    4);
    memcpy(&header[18], &numSamples,     4);

    uint32_t ticks_per_sample = CTR_CLK_RATE / DESIRED_SAMPLE_RATE_HZ;

    *ctx->gpio_reset = 0x00000001;
    usleep(1);
    *ctx->gpio_reset = 0x00000000;

    uint32_t last_counter = *ctx->gpio_counter;

    printf("Streaming started\n");

    while (1) {
        uint32_t now = *ctx->gpio_counter;
        if ((now - last_counter) < ticks_per_sample) {
            int n = recv(client_fd, cmd, sizeof(cmd) - 1, MSG_DONTWAIT);
            if (n > 0) {
                cmd[n] = '\0';
                if (strcmp(cmd, "STOP\n") == 0 || strcmp(cmd, "STOP") == 0) return 0;
                if (strstr(cmd, "RECORD ON")) record = true;
                if (strstr(cmd, "RECORD OFF")) record = false;
            } else if (n == 0) return -1;

            usleep(500); // Increased slightly to save CPU
            continue;
        }

        last_counter += ticks_per_sample;

        axi_iic_read_n_bytes(ctx->axi_iic_map, mpu_addr, mpu_ACC, MPU6050_data, 14);

        int16_t ax_raw = (int16_t)((MPU6050_data[0]  << 8) | MPU6050_data[1]);
        int16_t ay_raw = (int16_t)((MPU6050_data[2]  << 8) | MPU6050_data[3]);
        int16_t az_raw = (int16_t)((MPU6050_data[4]  << 8) | MPU6050_data[5]);
        int16_t gx_raw = (int16_t)((MPU6050_data[8]  << 8) | MPU6050_data[9]);
        int16_t gy_raw = (int16_t)((MPU6050_data[10] << 8) | MPU6050_data[11]);
        int16_t gz_raw = (int16_t)((MPU6050_data[12] << 8) | MPU6050_data[13]);

        int16_t channels[NUM_CHANNELS];
        channels[0] = ax_raw;
        channels[1] = ay_raw;
        channels[2] = az_raw;
        channels[3] = gx_raw;
        channels[4] = gy_raw;
        channels[5] = gz_raw;

        if (record && log_file != NULL) {
        size_t written = fwrite(channels, sizeof(int16_t), NUM_CHANNELS, log_file);
    
            if (written < NUM_CHANNELS) {
                perror("Fwrite failed");
            }
    
            // Optional: Periodically flush to ensure data isn't lost if power is pulled
            // Only do this occasionally (e.g., every 100 samples) to maintain performance
            static int flush_ctr = 0;
            if (++flush_ctr >= 100) {
                fflush(log_file);
                flush_ctr = 0;
            }
        }

        memcpy(packet, header, HEADER_SIZE);
        memcpy(packet + HEADER_SIZE, channels, BYTES_PER_BUFFER);

        size_t to_send = HEADER_SIZE + BYTES_PER_BUFFER;
        size_t sent_total = 0;

        while (sent_total < to_send) {
            ssize_t s = send(client_fd, packet + sent_total, to_send - sent_total, 0);
            if (s <= 0) {
                perror("send");
                return -1;
            }
            sent_total += (size_t)s;
        }
    }
}
int main(void)
{
    HardwareContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (init_hardware(&ctx) < 0)
        return 1;

    int server_fd, client_fd;
    int opt = 1;
    struct sockaddr_in servaddr;
    char buffer[64];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        cleanup_hardware(&ctx);
        return 1;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(server_fd);
        cleanup_hardware(&ctx);
        return 1;
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        cleanup_hardware(&ctx);
        return 1;
    }

    while (1) {
        printf("Waiting for connection...\n");

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf("Client connected\n");

        while (1) {
            memset(buffer, 0, sizeof(buffer));
            int n = read(client_fd, buffer, sizeof(buffer) - 1);

            if (n <= 0) {
                printf("Client disconnected\n");
                break;
            }

            buffer[n] = '\0';
            printf("Received: [%s]\n", buffer);

            if (strcmp(buffer, "REDPITAYA\n") == 0 || strcmp(buffer, "REDPITAYA") == 0) {
                write(client_fd, "OK\n", 3);
                printf("Sent OK\n");
            }
            else if (strcmp(buffer, "START\n") == 0 || strcmp(buffer, "START") == 0) {
                system("rw"); 

                // 1. Get current system time
                time_t rawtime;
                struct tm *timeinfo;
                char time_str[20];
                char filename[128];

                time(&rawtime);
                timeinfo = localtime(&rawtime);

                // 2. Format time: YYYYMMDD_HHMMSS
                strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", timeinfo);
    
                // 3. Create full path
                sprintf(filename, "/root/Measurements/recording_%s.bin", time_str);

                // 4. Open the file
                FILE *fp = fopen(filename, "wb");
                if (fp == NULL) {
                    perror("Failed to open file on SD card");
                    write(client_fd, "ERROR_FILE\n", 11);
                    continue;
                }

                printf("Saving to: %s\n", filename);
                write(client_fd, "STARTED\n", 8);

                if (run_stream(client_fd, &ctx, fp) < 0) {
                    fclose(fp);
                    break;
                }

                fclose(fp);
                system("sync");
                write(client_fd, "STOPPED\n", 8);
            }
        }

        close(client_fd);
    }

    close(server_fd);
    cleanup_hardware(&ctx);
    return 0;
}