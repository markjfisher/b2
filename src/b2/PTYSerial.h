#ifndef HEADER_A1B2C3D4E5F6789012345678PTYSERIAL
#define HEADER_A1B2C3D4E5F6789012345678PTYSERIAL

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include <beeb/serproc.h>
#include <beeb/conf.h>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>

class Trace;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// PTY/Serial device handler for FujiNet integration
// Provides bidirectional serial communication with PTY or hardware serial devices

class PTYSerialDevice {
  public:
    PTYSerialDevice();
    ~PTYSerialDevice();

    PTYSerialDevice(const PTYSerialDevice &) = delete;
    PTYSerialDevice &operator=(const PTYSerialDevice &) = delete;

    // Open a PTY or serial device for communication
    bool Open(const std::string &device_path, bool is_pty, bool debug = false);

    // Close the device and clean up
    void Close();

    // Check if device is open and ready
    bool IsOpen() const;

    // Get source and sink for SERPROC connection
    std::shared_ptr<SerialDataSource> GetSource();
    std::shared_ptr<SerialDataSink> GetSink();

    // Get last error message
    std::string GetLastError() const;

#if BBCMICRO_TRACE
    // Set trace output (for TRACEF logging)
    void SetTrace(Trace *trace);
#endif

  private:
    class PTYSerialDataSource;
    class PTYSerialDataSink;

    std::shared_ptr<PTYSerialDataSource> m_source;
    std::shared_ptr<PTYSerialDataSink> m_sink;

    int m_fd = -1;
    std::string m_device_path;
    bool m_is_pty = false;
    bool m_debug = false;
    std::atomic<bool> m_is_open{false};
    std::string m_last_error;

    // Thread for reading from PTY/serial device
    std::thread m_read_thread;
    std::atomic<bool> m_should_stop{false};

    // Buffer for data read from device (device -> emulator)
    std::mutex m_rx_mutex;
    std::queue<uint8_t> m_rx_buffer;

    // Buffer for data to write to device (emulator -> device)
    std::mutex m_tx_mutex;
    std::queue<uint8_t> m_tx_buffer;
    bool m_tx_pending = false;
    std::atomic<uint64_t> m_last_addbyte_time{0};  // Timestamp of last AddByte call (microseconds)

#if BBCMICRO_TRACE
    Trace *m_trace = nullptr;
#endif

    void ReadThreadFn();
    void ProcessWrite();

    // Internal classes for SerialDataSource/Sink interface
    class PTYSerialDataSource : public SerialDataSource {
      public:
        explicit PTYSerialDataSource(PTYSerialDevice *device);
        bool HasData() override;
        uint8_t GetNextByte() override;

      private:
        PTYSerialDevice *m_device;
    };

    class PTYSerialDataSink : public SerialDataSink {
      public:
        explicit PTYSerialDataSink(PTYSerialDevice *device);
        void AddByte(uint8_t value) override;

      private:
        PTYSerialDevice *m_device;
    };

    friend class PTYSerialDataSource;
    friend class PTYSerialDataSink;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif

