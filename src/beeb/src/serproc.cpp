#include <shared/system.h>
#include <beeb/serproc.h>
#include <6502/6502.h>
#include <beeb/MC6850.h>
#include <beeb/conf.h>
#include <beeb/Trace.h>

#include <shared/enum_def.h>
#include <beeb/serproc.inl>
#include <shared/enum_end.h>

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static_assert(CYCLES_PER_SECOND == 4000000, "BBCMicro::Update needs updating");

template <unsigned DIV>
static constexpr uint8_t GetSERPROCClockMask() {
    static_assert((DIV & (DIV - 1)) == 0);
    if constexpr (1.23e6 / DIV > CYCLES_PER_SECOND / 13) {
        // TODO: This case isn't emulated properly. But the data rate is too
        // high for the poor old 6502 anyway.
        return 0;
    } else {
        return DIV - 1;
    }
}

static constexpr uint8_t SERPROC_TX_CLOCK_MASKS[8] = {
    GetSERPROCClockMask<1>(),   // 19200: 1.23e6/64/1=19219 -> returns 0 (no extra division)
    GetSERPROCClockMask<2>(),   // 9600
    GetSERPROCClockMask<4>(),   // 4800
    GetSERPROCClockMask<8>(),   // 2400
    GetSERPROCClockMask<16>(),  // 1200
    GetSERPROCClockMask<64>(),  // 300
    GetSERPROCClockMask<128>(), // 150
    GetSERPROCClockMask<256>(), // 75
};

// Slightly slower RX timing so the MC6850/6502 don't overrun.
// For 19200 we *don't* use 0; we pick a small non-zero mask so we only
// push one bit into MC6850::UpdateReceive every N CPU cycles.
static constexpr uint8_t SERPROC_RX_CLOCK_MASKS[8] = {
    15,                         // 19200: 1 bit every 16 CPU cycles (fudged)
    GetSERPROCClockMask<2>(),   // 9600
    GetSERPROCClockMask<4>(),   // 4800
    GetSERPROCClockMask<8>(),   // 2400
    GetSERPROCClockMask<16>(),  // 1200
    GetSERPROCClockMask<64>(),  // 300
    GetSERPROCClockMask<128>(), // 150
    GetSERPROCClockMask<256>(), // 75
};


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const unsigned SERPROC_BAUD_RATES[8] = {
    19200, // 000
    1200,  // 001
    4800,  // 010
    150,   // 011
    9600,  // 100
    300,   // 101
    2400,  // 110
    75,    // 111
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

SerialDataSource::~SerialDataSource() {
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

SerialDataSink::~SerialDataSink() {
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void SERPROC::Write(void *serproc_, M6502Word addr, uint8_t value) {
    (void)addr;
    auto serproc = (SERPROC *)serproc_;

    serproc->m_control.value = value;

    // split TX/RX baud rate. Had issues with TX being "15" slowing the command getting to FN
    serproc->m_tx_clock_mask = SERPROC_TX_CLOCK_MASKS[serproc->m_control.bits.tx_baud];
    serproc->m_rx_clock_mask = SERPROC_RX_CLOCK_MASKS[serproc->m_control.bits.rx_baud];

    if (serproc->m_control.bits.rs423) {
        serproc->m_acia->SetNotDCD(false);
    } else {
        serproc->m_acia->SetNotDCD(true);
    }

    TRACEF(serproc->m_trace, "SERPROC - Write $%02x (%03u) (%%%s): Tx=%u; Rx=%u; RS423=%d; Motor=%d",
           value, value, BINARY_BYTE_STRINGS[value],
           SERPROC_BAUD_RATES[serproc->m_control.bits.tx_baud],
           SERPROC_BAUD_RATES[serproc->m_control.bits.rx_baud],
           serproc->m_control.bits.rs423,
           serproc->m_control.bits.motor);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void SERPROC::Update() {
    // TX (emulator -> device) – with fixed LSB-first code
    if ((m_clock & m_tx_clock_mask) == 0) {
        MC6850::TransmitResult result = m_acia->UpdateTransmit();

        switch (result.type) {
        default:
            break;

        case MC6850BitType_Start:
            m_tx_byte = 0;
            m_tx_bit_index = 0;
            break;

        case MC6850BitType_Data:
            // LSB-first
            m_tx_byte |= (result.bit << m_tx_bit_index);
            m_tx_bit_index++;
            break;

        case MC6850BitType_Stop:
            if (!!m_sink) {
                m_sink->AddByte(m_tx_byte);
            }
            break;
        }
    }

    // RX (device -> emulator) – drive ACIA once per bit time
    if ((m_clock & m_rx_clock_mask) == 0) {
        uint8_t bit = 1; // idle high by default

        if (!!m_source) {
            switch (m_rx_state) {
            case RXState_Idle:
                if (!m_rx_has_byte && m_source->HasData()) {
                    m_rx_byte = m_source->GetNextByte();
                    m_rx_has_byte = true;
                    m_rx_state = RXState_Start;
                    // line stays idle high *this* tick; start bit on next tick
                    bit = 1;
                } else {
                    bit = 1; // idle
                }
                break;

            case RXState_Start:
                bit = 0;                  // start bit
                m_rx_bit_index = 0;
                m_rx_state = RXState_Data;
                break;

            case RXState_Data:
                bit = (m_rx_byte >> m_rx_bit_index) & 1;
                m_rx_bit_index++;
                if (m_rx_bit_index >= 8) {
                    m_rx_state = RXState_Stop;
                }
                break;

            case RXState_Stop:
                bit = 1;                  // stop bit
                m_rx_state = RXState_Idle;
                m_rx_has_byte = false;
                // Optional for an inter-byte gap:
                // m_rx_idle_count = 0;
                break;
            }
        }

        m_acia->UpdateReceive(bit);
    }

    ++m_clock;
}


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_TRACE
void SERPROC::SetTrace(Trace *t) {
    m_trace = t;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool SERPROC::HasSource() const {
    return !!m_source;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool SERPROC::HasSink() const {
    return !!m_sink;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool SERPROC::IsMotorOn() const {
    return !!m_control.bits.motor;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void SERPROC::Link(MC6850 *acia) {
    m_acia = acia;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void SERPROC::SetSource(std::shared_ptr<SerialDataSource> source) {
    m_source = std::move(source);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void SERPROC::SetSink(std::shared_ptr<SerialDataSink> sink) {
    m_sink = std::move(sink);
}
