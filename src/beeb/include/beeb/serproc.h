#ifndef HEADER_E81A9A399B9F4CEC9C472D77C905F64C // -*- mode:c++ -*-
#define HEADER_E81A9A399B9F4CEC9C472D77C905F64C

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include "conf.h"
#include <memory>

#include <shared/enum_decl.h>
#include "serproc.inl"
#include <shared/enum_end.h>

union M6502Word;
class MC6850;
#if BBCMICRO_TRACE
class Trace;
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// Transmit = ACIA->SERPROC->Device (Sink)

// Receive = Device (Source)->SERPROC->ACIA

// TODO: maybe the base classes are overkill, and could be replaced with
// std::function...?

// Serial data sources and sinks will may to bear in mind thread safety.

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

class SerialDataSource : public std::enable_shared_from_this<SerialDataSource> {
  public:
    SerialDataSource() = default;
    virtual ~SerialDataSource();

    SerialDataSource(const SerialDataSource &) = delete;
    SerialDataSource &operator=(const SerialDataSource &) = delete;
    SerialDataSource(SerialDataSource &&) = delete;
    SerialDataSource &operator=(SerialDataSource &&) = delete;

    // Check if data is available
    virtual bool HasData() = 0;

    // Get next byte (only call if HasData() returns true)
    virtual uint8_t GetNextByte() = 0;

  protected:
  private:
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

class SerialDataSink : public std::enable_shared_from_this<SerialDataSink> {
  public:
    SerialDataSink() = default;
    virtual ~SerialDataSink();

    SerialDataSink(const SerialDataSink &) = delete;
    SerialDataSink &operator=(const SerialDataSink &) = delete;
    SerialDataSink(SerialDataSink &&) = delete;
    SerialDataSink &operator=(SerialDataSink &&) = delete;

    virtual void AddByte(uint8_t value) = 0;

  protected:
  private:
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

class SERPROC {
  public:
    struct ControlRegisterBits {
        SERPROCBaudRate tx_baud : 3;
        SERPROCBaudRate rx_baud : 3;
        uint8_t rs423 : 1;
        uint8_t motor : 1;
    };

    union ControlRegister {
        uint8_t value;
        ControlRegisterBits bits;
    };
    CHECK_SIZEOF(ControlRegister, 1);

    static void Write(void *serproc, M6502Word addr, uint8_t value);

    void Update();

#if BBCMICRO_TRACE
    void SetTrace(Trace *t);
#endif

    bool HasSource() const;
    bool HasSink() const;
    bool IsMotorOn() const;

    void Link(MC6850 *acia);

    void SetSource(std::shared_ptr<SerialDataSource> source);
    void SetSink(std::shared_ptr<SerialDataSink> sink);

  protected:
  private:
    ControlRegister m_control = {};
    uint8_t m_clock = 0;
    uint8_t m_tx_clock_mask = 0;
    uint8_t m_rx_clock_mask = 0;

    uint8_t m_tx_byte = 0;
    uint8_t m_tx_bit_index = 0;  // Track which bit we're receiving (0-7)

    // RX state for receiving from source
    enum RXState {
        RXState_Idle,
        RXState_Start,
        RXState_Data,
        RXState_Stop
    };
    RXState m_rx_state = RXState_Idle;
    uint8_t m_rx_byte = 0;
    uint8_t m_rx_bit_index = 0;
    bool m_rx_has_byte = false;
    uint8_t m_rx_idle_count = 0;  // Counter for inter-byte gap

    std::shared_ptr<SerialDataSource> m_source;
    std::shared_ptr<SerialDataSink> m_sink;

    MC6850 *m_acia = nullptr;

#if BBCMICRO_TRACE
    Trace *m_trace = nullptr;
#endif

    friend class SerialDebugWindow;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// Indexed by SERPROCBaudRate.
extern const unsigned SERPROC_BAUD_RATES[8];

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif
