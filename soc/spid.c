#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>
#include <syslog.h>

#define N_CHANNEL 3
#define BUFSIZE 255

#define debug(args...) printf(args)
#define info(args...)   syslog(LOG_INFO, args)
#define error(args...)  syslog(LOG_ERR, args)
#define fatal(args...) ({ \
    syslog (LOG_CRIT, args); \
    exit(1); \
})

typedef struct ChannelData {
    int in_length;
    char out_buf[BUFSIZE];
    int out_length;
    char in_buf[BUFSIZE];
} ChannelData;

ChannelData channels[N_CHANNEL];
uint8_t channel_status[N_CHANNEL];

/// Use sysfs to export the specified GPIO
void gpio_export(const char* gpio) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s", gpio);
    if (access(path, F_OK) == 0) {
        // Already exported;
        return;
    }

    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) {
      fatal("Error opening /sys/class/gpio/export: %s\n", strerror(errno));
      exit(1);
    }
    if (write(fd, gpio, strlen(gpio)) < 0) {
        fatal("GPIO export write: %s", strerror(errno));
    };
    close(fd);
}

/// Open a sysfs GPIO file
int gpio_open(const char* gpio, const char* file) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/%s", gpio, file);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
      fatal("Error opening %s: %s\n", path, strerror(errno));
    }
    return fd;
}

/// Set the direction of the specified GPIO pin
void gpio_direction(const char* gpio, const char* mode) {
    int fd = gpio_open(gpio, "direction");
    if (write(fd, mode, strlen(mode)) < 0) {
        fatal("GPIO direction write: %s", strerror(errno));
    }
    close(fd);
}

/// Set the edge trigger mode of the specified GPIO pin
void gpio_edge(const char* gpio, const char* mode) {
    int fd = gpio_open(gpio, "edge");
    if (write(fd, mode, strlen(mode)) < 0){
        fatal("GPIO edge write: %s", strerror(errno));
    }
    close(fd);
}

#define GPIO_POLL fds[0]
#define CONN_POLL(n) fds[1 + n]
#define SOCK_POLL(n) fds[1 + N_CHANNEL + n]
#define N_POLLFDS (N_CHANNEL * 2 + 1)
struct pollfd fds[N_POLLFDS];

void delay() {
    volatile int i = 1000;
    while(i--);
}

void close_connection(uint8_t channel) {

    info("Closing connection %d\n", channel);
    close(CONN_POLL(channel).fd);
    CONN_POLL(channel).fd = -1;

    channels[channel].out_length = 0;
    // Re-enable events on a new connection
    SOCK_POLL(channel).events = POLLIN;
}

int main(int argc, char** argv) {
    openlog("spid", LOG_PERROR | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
    info("Starting");

    if (argc != 5) {
      fatal("usage: spid /dev/spidev0.1 irq_gpio sync_gpio /var/run/tessel\n");
    }

    // Open SPI
    int spi_fd = open(argv[1], O_RDWR);
    if (spi_fd < 0) {
      fatal("Error opening SPI device %s: %s\n", argv[1], strerror(errno));
    }

    // set up IRQ pin
    gpio_export(argv[2]);
    gpio_direction(argv[2], "in");
    gpio_edge(argv[2], "rising");
    int irq_fd = gpio_open(argv[2], "value");

    // set up sync pin
    gpio_export(argv[3]);
    gpio_edge(argv[3], "none");
    gpio_direction(argv[3], "high");
    int sync_fd = gpio_open(argv[3], "value");

    memset(channels, 0, sizeof(channels));
    memset(channel_status, 0, sizeof(channel_status));
    memset(fds, 0, sizeof(fds));

    GPIO_POLL.fd = irq_fd;
    GPIO_POLL.events = POLLPRI;

    // Create the listening unix domain sockets
    for (int i = 0; i<N_CHANNEL; i++) {
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%d", argv[4], i);
        unlink(addr.sun_path);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            fatal("Error creating socket %s: %s\n", addr.sun_path, strerror(errno));
        }

        if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
            fatal("Error binding socket %s: %s\n", addr.sun_path, strerror(errno));
        }

        if (listen(fd, 1) == -1) {
            fatal("Error listening on socket %s: %s\n", addr.sun_path, strerror(errno));
        }

        SOCK_POLL(i).fd = fd;
        SOCK_POLL(i).events = POLLIN;
        CONN_POLL(i).fd = -1;
    }

    uint8_t writable = 0;
    uint8_t channels_open = 0;
    int retries = 0;

    while (1) {
        for (int i=0; i<N_POLLFDS; i++) {
            fds[i].revents = 0;
        }

        int nfds = poll(fds, N_POLLFDS, 5000);
        if (nfds < 0) {
            fatal("Error in poll: %s", strerror(errno));
        }

        debug("poll returned: %i\n", nfds);

        for (int i=0; i<N_POLLFDS; i++) {
            debug("%x ", fds[i].events);
        }
        debug("- %x %x %x \n", POLLIN, POLLOUT, POLLERR);

        for (int i=0; i<N_POLLFDS; i++) {
            debug("%x ", fds[i].revents);
        }
        debug("\n");

        // If it was a GPIO interrupt on the IRQ pin, acknowlege it
        if (GPIO_POLL.revents & POLLPRI) {
            char buf[2];
            lseek(irq_fd, SEEK_SET, 0);
            if (read(irq_fd, buf, 2) < 0) {
                fatal("GPIO read: %s", strerror(errno));
            }
            debug("GPIO interrupt %c\n", buf[0]);
        }

        // Sync pin low
        if (write(sync_fd, "0", 1) < 0) {
            fatal("GPIO write: %s", strerror(errno));
        }

        delay();

        // Check for new connections on unconnected sockets
        for (int i=0; i<N_CHANNEL; i++) {
            if (SOCK_POLL(i).revents & POLLIN) {
                int fd = accept(SOCK_POLL(i).fd, NULL, 0);
                if (fd == -1) {
                    fatal("Error in accept: %s", strerror(errno));
                }

                info("Accepted connection on %i\n", i);
                CONN_POLL(i).fd = fd;
                CONN_POLL(i).events = POLLIN | POLLOUT;

                // disable further events on listening socket
                SOCK_POLL(i).events = 0;
                channels_open |= (1<<i);
            }
        }

        // Check which connected sockets are readable / writable or closed
        for (int i=0; i<N_CHANNEL; i++) {
            bool to_close = false;
            if (CONN_POLL(i).revents & POLLIN) {
                int length = read(CONN_POLL(i).fd, channels[i].out_buf, BUFSIZE);
                CONN_POLL(i).events &= ~POLLIN;

                debug("%i: Read %u\n", i, length);

                if (length > 0) {
                    channels[i].out_length = length;
                } else {
                    if (length < 0) {
                        error("Error in read %i: %s\n", i, strerror(errno));
                    }
                    to_close = true;
                }
            }

            if (to_close || CONN_POLL(i).revents & POLLHUP
                         || CONN_POLL(i).revents & POLLERR
                         || CONN_POLL(i).revents & POLLRDHUP) {
                close_connection(i);
                writable &= ~(1 << i);
                channels_open &= ~(1<<i);

                continue;
            }

            if (CONN_POLL(i).revents & POLLOUT) {
                CONN_POLL(i).events &= ~POLLOUT;
                writable |= (1 << i);
                debug("%i: Writable\n", i);
            }
        }

        // Prepare the header transfer
        struct spi_ioc_transfer ctrl_transfer[2];
        memset(ctrl_transfer, 0, sizeof(ctrl_transfer));

        uint8_t tx_buf[2 + N_CHANNEL];
        uint8_t rx_buf[2 + N_CHANNEL];

        tx_buf[0] = 0x53;
        tx_buf[1] = writable | (channels_open << 4);

        for (int i=0; i<N_CHANNEL; i++) {
            tx_buf[2+i] = channels[i].out_length;
        }

        debug("tx: %2x %2x %2x %2x %2x\n", tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3], tx_buf[4]);

        ctrl_transfer[0].len = sizeof(tx_buf);
        ctrl_transfer[0].tx_buf = (unsigned long)tx_buf;
        ctrl_transfer[1].len = sizeof(rx_buf);
        ctrl_transfer[1].rx_buf = (unsigned long)rx_buf;
        int status = ioctl(spi_fd, SPI_IOC_MESSAGE(2), ctrl_transfer);

        if (status < 0) {
          fatal("SPI_IOC_MESSAGE: header: %s", strerror(errno));
        }

        debug("rx: %2x %2x %2x %2x %2x\n", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);
        if (write(sync_fd, "1", 1) < 0) {
            fatal("GPIO write: %s", strerror(errno));
        }

        if (rx_buf[0] != 0xCA) {
            error("Invalid command reply: %2x %2x %2x %2x %2x\n", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);
            retries++;

            if (retries > 15) {
                fatal("Too many retries, exiting");
            } else {
                continue;
            }
        }

        for (int i=0; i<N_CHANNEL; i++) {
            uint8_t new_status = (rx_buf[1] & (0x10 << i));
            uint8_t old_status = channel_status[i];
            // If the status hasn't changed
            if (new_status == old_status) {
                // Make no changes to the polling
                continue;
            }
            // If the new status has the channel enabled
            else if (new_status == 1) {
                // We should start listening for connect events
                SOCK_POLL(i).events = POLLIN;
            }
            // If the new status disables the channel
            else {
                close_connection(i);
                writable &= ~(1 << i);
                channels_open &= ~(1<<i);
                SOCK_POLL(i).events = 0;
            }

            channel_status[i] = new_status;
        }
        if ((rx_buf[1] & (0x10 << 0))) {
            debug("\n\nUSB Enabled\n\n");
        }
        else {
            debug("\n\nUSB Disabled\n\n");
        }

        retries = 0;

        delay();

        // Prepare the data transfer
        struct spi_ioc_transfer transfer[N_CHANNEL * 2];
        memset(transfer, 0, sizeof(transfer));
        int desc = 0;

        for (int chan=0; chan<N_CHANNEL; chan++) {
            int size = channels[chan].out_length;
            if (rx_buf[1] & (1<<chan) && size > 0) {
                CONN_POLL(chan).events |= POLLIN;
                transfer[desc].len = size;
                transfer[desc].tx_buf = (unsigned long) &channels[chan].out_buf[0];
                channels[chan].out_length = 0;
                desc++;
            }

            size = rx_buf[2+chan];
            if (writable & (1<<chan) && size > 0) {
                transfer[desc].len = size;
                transfer[desc].rx_buf = (unsigned long) &channels[chan].in_buf[0];
                desc++;
            }
        }

        if (desc != 0) {
            debug("Performing transfer on %i channels\n", desc);

            int status = ioctl(spi_fd, SPI_IOC_MESSAGE(desc), transfer);

            if (status < 0) {
              fatal("SPI_IOC_MESSAGE: data: %s", strerror(errno));
            }

            // Write received data to the appropriate socket
            for (int chan=0; chan<N_CHANNEL; chan++) {
                int size = rx_buf[2+chan];
                if (writable & (1<<chan) && size > 0) {
                    int r = write(CONN_POLL(chan).fd, &channels[chan].in_buf[0], size);
                    debug("%i: Write %u %i\n", chan, size, r);
                    if (r < 0) {
                        error("Error in write %i: %s\n", chan, strerror(errno));
                    }

                    CONN_POLL(chan).events |= POLLOUT;
                    writable &= ~(1<<chan);
                }
            }
        }
    }
}
