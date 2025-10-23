#include <shared/system.h>
#include "PTYSerial.h"
#include <shared/log.h>
#include <shared/strings.h>
#include <beeb/Trace.h>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <errno.h>
#include <cstring>
#include <sys/time.h>

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// Default timeout for select() in milliseconds
static constexpr int SELECT_TIMEOUT_MS = 10;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

PTYSerialDevice::PTYSerialDevice()
    : m_source(std::make_shared<PTYSerialDataSource>(this))
    , m_sink(std::make_shared<PTYSerialDataSink>(this)) {
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

PTYSerialDevice::~PTYSerialDevice() {
    Close();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool PTYSerialDevice::Open(const std::string &device_path, bool is_pty, bool debug) {
    TRACEF(m_trace, "PTY - Open: device=%s, is_pty=%d", device_path.c_str(), is_pty);

    if (m_is_open) {
        m_last_error = "Device already open";
        TRACEF(m_trace, "PTY - Open FAILED: already open");
        return false;
    }

    m_device_path = device_path;
    m_is_pty = is_pty;
    m_debug = debug;

    m_fd = open(device_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        m_last_error = strprintf("Failed to open %s: %s", device_path.c_str(), strerror(errno));
        TRACEF(m_trace, "PTY - Open FAILED: %s", m_last_error.c_str());
        if (m_debug) {
            LOGF(OUTPUT, "PTYSerial: %s\n", m_last_error.c_str());
        }
        return false;
    }

    TRACEF(m_trace, "PTY - Opened fd=%d successfully", m_fd);

    // Configure for raw mode
    struct termios tty;
    if (tcgetattr(m_fd, &tty) != 0) {
        m_last_error = strprintf("Failed to get termios: %s", strerror(errno));
        if (m_debug) {
            LOGF(OUTPUT, "PTYSerial: %s\n", m_last_error.c_str());
        }
        close(m_fd);
        m_fd = -1;
        return false;
    }

    // Set raw mode
    cfmakeraw(&tty);

    // Set baud rate (19200 for FujiNet protocol)
    cfsetispeed(&tty, B19200);
    cfsetospeed(&tty, B19200);

    // 8N1
    tty.c_cflag &= (tcflag_t)~PARENB;  // No parity
    tty.c_cflag &= (tcflag_t)~CSTOPB;  // 1 stop bit
    tty.c_cflag &= (tcflag_t)~CSIZE;
    tty.c_cflag |= CS8;                // 8 bits

    // Enable receiver, ignore modem control lines
    tty.c_cflag |= CREAD | CLOCAL;

    // Non-canonical mode, no echo
    tty.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHOE | ISIG);

    // No input processing
    tty.c_iflag &= (tcflag_t)~(IXON | IXOFF | IXANY);
    tty.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    // No output processing
    tty.c_oflag &= (tcflag_t)~OPOST;

    // Non-blocking reads
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
        m_last_error = strprintf("Failed to set termios: %s", strerror(errno));
        if (m_debug) {
            LOGF(OUTPUT, "PTYSerial: %s\n", m_last_error.c_str());
        }
        close(m_fd);
        m_fd = -1;
        return false;
    }

    // Clear buffers
    {
        std::lock_guard<std::mutex> rx_lock(m_rx_mutex);
        std::lock_guard<std::mutex> tx_lock(m_tx_mutex);
        while (!m_rx_buffer.empty()) m_rx_buffer.pop();
        while (!m_tx_buffer.empty()) m_tx_buffer.pop();
    }

    m_is_open = true;
    m_should_stop = false;

    m_read_thread = std::thread(&PTYSerialDevice::ReadThreadFn, this);

    if (m_debug) {
        LOGF(OUTPUT, "PTYSerial: Opened %s (%s mode)\n", 
             device_path.c_str(), is_pty ? "PTY" : "Hardware");
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void PTYSerialDevice::Close() {
    if (!m_is_open) {
        return;
    }

    m_is_open = false;
    m_should_stop = true;

    // Wait for read thread to finish
    if (m_read_thread.joinable()) {
        m_read_thread.join();
    }

    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }

    // Clear buffers
    {
        std::lock_guard<std::mutex> rx_lock(m_rx_mutex);
        std::lock_guard<std::mutex> tx_lock(m_tx_mutex);
        while (!m_rx_buffer.empty()) m_rx_buffer.pop();
        while (!m_tx_buffer.empty()) m_tx_buffer.pop();
    }

    if (m_debug) {
        LOGF(OUTPUT, "PTYSerial: Closed %s\n", m_device_path.c_str());
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool PTYSerialDevice::IsOpen() const {
    return m_is_open;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::shared_ptr<SerialDataSource> PTYSerialDevice::GetSource() {
    return m_source;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::shared_ptr<SerialDataSink> PTYSerialDevice::GetSink() {
    return m_sink;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::string PTYSerialDevice::GetLastError() const {
    return m_last_error;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_TRACE
void PTYSerialDevice::SetTrace(Trace *trace) {
    m_trace = trace;
    TRACEF(m_trace, "PTY - SetTrace called, trace=%p", (void*)trace);
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void PTYSerialDevice::ReadThreadFn() {
    uint8_t buffer[1024];

    while (!m_should_stop && m_is_open) {
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(m_fd, &read_fds);

        // Check if we have data to write
        bool has_tx_data = false;
        {
            std::lock_guard<std::mutex> lock(m_tx_mutex);
            has_tx_data = !m_tx_buffer.empty();
        }

        if (has_tx_data) {
            FD_SET(m_fd, &write_fds);
        }

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = SELECT_TIMEOUT_MS * 1000;

        int ret = select(m_fd + 1, &read_fds, &write_fds, nullptr, &timeout);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (m_debug) {
                LOGF(OUTPUT, "PTYSerial: select() error: %s\n", strerror(errno));
            }
            break;
        }

        if (ret == 0) {
            // Timeout - log if we have pending TX data
            if (has_tx_data) {
                TRACEF(m_trace, "PTY - select() timeout with %zu bytes pending in TX queue",
                       m_tx_buffer.size());
            }
            continue;
        }

        // DIAGNOSTIC: Log select() results when we have TX data
        // if (has_tx_data) {
        //     bool can_write = FD_ISSET(m_fd, &write_fds);
        //     bool can_read = FD_ISSET(m_fd, &read_fds);
        //     TRACEF(m_trace, "PTY - select() returned: can_read=%d, can_write=%d, tx_queue=%zu",
        //            can_read, can_write, m_tx_buffer.size());
        // }

        // Read available data
        if (FD_ISSET(m_fd, &read_fds)) {
            ssize_t n = read(m_fd, buffer, sizeof(buffer));
            if (n > 0) {
                std::lock_guard<std::mutex> lock(m_rx_mutex);
                for (ssize_t i = 0; i < n; ++i) {
                    m_rx_buffer.push(buffer[i]);
                }

                if (m_debug) {
                    LOGF(OUTPUT, "PTYSerial: RX %zd bytes\n", n);
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (m_debug) {
                    LOGF(OUTPUT, "PTYSerial: read() error: %s\n", strerror(errno));
                }
                break;
            }
        }

        // Write pending data - but only if debounce period has elapsed - TODO: IS THERE A BETTER WAY TO SYNC / BREAK THE RACE CONDITION?
        // This won't work if we are constantly writing new blocks of data more frequently than the debounce period.
        if (FD_ISSET(m_fd, &write_fds)) {
            bool should_write = false;
            {
                std::lock_guard<std::mutex> lock(m_tx_mutex);
                if (!m_tx_buffer.empty()) {
                    // Check if enough time has passed since last AddByte
                    struct timeval tv;
                    gettimeofday(&tv, nullptr);
                    uint64_t now_us = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
                    uint64_t last_add = m_last_addbyte_time.load(std::memory_order_acquire);

                    // Debounce: wait 22ms after last AddByte before writing
                    // This allows all bytes from a burst to accumulate
                    // 22ms comes from testing in loop, 20ms occasionally still caused the packets to be split.
                    if (last_add == 0 || (now_us - last_add) >= 22000) {
                        should_write = true;
                    }
                }
            }

            if (should_write) {
                ProcessWrite();
            }
        }
    }

    if (m_debug) {
        LOGF(OUTPUT, "PTYSerial: Read thread exiting\n");
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void PTYSerialDevice::ProcessWrite() {
    std::lock_guard<std::mutex> lock(m_tx_mutex);

    if (m_tx_buffer.empty()) {
        return;
    }

    // Gather all bytes from queue into temporary buffer
    // We need this because std::queue doesn't provide direct memory access
    uint8_t buffer[512];
    size_t count = 0;
    while (!m_tx_buffer.empty() && count < sizeof(buffer)) {
        buffer[count++] = m_tx_buffer.front();
        m_tx_buffer.pop();
    }

    // Log to trace with hex dump
    if (m_debug) {
        std::string hex_dump;
        for (size_t i = 0; i < count; i++) {
            hex_dump += strprintf("%02X ", buffer[i]);
        }
        TRACEF(m_trace, "PTY - ProcessWrite: %zu bytes [%s]", count, hex_dump.c_str());
    }

    // Helper to re-queue bytes from buffer starting at given offset
    auto requeue_bytes = [&](size_t start_offset) {
        std::queue<uint8_t> temp_queue;

        // Add unwritten bytes from buffer
        for (size_t i = start_offset; i < count; i++) {
            temp_queue.push(buffer[i]);
        }

        // Append any remaining bytes from original queue
        while (!m_tx_buffer.empty()) {
            temp_queue.push(m_tx_buffer.front());
            m_tx_buffer.pop();
        }

        m_tx_buffer = std::move(temp_queue);
    };

    // Write to file descriptor
    ssize_t n = write(m_fd, buffer, count);

    if (n > 0) {
        TRACEF(m_trace, "PTY - Wrote %zd of %zu bytes successfully", n, count);

        // Handle partial write: put unwritten bytes back at front of queue
        if ((size_t)n < count) {
            requeue_bytes((size_t)n);
        }
    } else if (n < 0) {
        // Write failed - put all bytes back
        TRACEF(m_trace, "PTY - Failed to write data: %s", strerror(errno));
        requeue_bytes(0);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
// PTYSerialDataSource implementation
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

PTYSerialDevice::PTYSerialDataSource::PTYSerialDataSource(PTYSerialDevice *device)
    : m_device(device) {
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool PTYSerialDevice::PTYSerialDataSource::HasData() {
    std::lock_guard<std::mutex> lock(m_device->m_rx_mutex);
    bool has_data = !m_device->m_rx_buffer.empty();

    return has_data;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint8_t PTYSerialDevice::PTYSerialDataSource::GetNextByte() {
    std::lock_guard<std::mutex> lock(m_device->m_rx_mutex);

    if (m_device->m_rx_buffer.empty()) {
        // Shouldn't be called if HasData() returns false, but handle gracefully
        return 0;
    }

    uint8_t byte = m_device->m_rx_buffer.front();
    m_device->m_rx_buffer.pop();

    TRACEF(m_device->m_trace, "PTY RX - GetNextByte: $%02X ('%c') [queue=%zu]", 
        byte, (byte >= 32 && byte < 127) ? byte : '.', m_device->m_rx_buffer.size());

    return byte;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
// PTYSerialDataSink implementation
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

PTYSerialDevice::PTYSerialDataSink::PTYSerialDataSink(PTYSerialDevice *device)
    : m_device(device) {
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void PTYSerialDevice::PTYSerialDataSink::AddByte(uint8_t value) {
    std::lock_guard<std::mutex> lock(m_device->m_tx_mutex);
    m_device->m_tx_buffer.push(value);

    // Update timestamp for debounce logic
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint64_t now_us = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
    m_device->m_last_addbyte_time.store(now_us, std::memory_order_release);

    TRACEF(m_device->m_trace, "PTY TX - AddByte: $%02X ('%c') [queue=%zu]",
           value, (value >= 32 && value < 127) ? value : '.', m_device->m_tx_buffer.size());
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

