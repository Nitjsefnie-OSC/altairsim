#pragma once
//
// The bus side the AMD Am9511A and Am9512 math chips share -- a CHIP INTERFACE, NOT A CARD.
//
// Both parts sit behind the same two ports (C/D = 0: push on write, pop on read; C/D = 1:
// command on write, status on read), keep their operands on a 16-byte LIFO ring, and drive
// the same END, SVREQ and PAUSE handshake (reference/AMD 9511A.md, reference/AMD 9512.md).
// What differs is the command set, the number formats and the error bits: each chip
// supplies execute(). The 9512 also has an ERR output; the 9511A has none.
//
// `now` is the count of the chip's CLK edges. A command is worked out when it is written,
// and is BUSY until its cycle count is done: the result goes onto the stack at once, and
// BUSY, END, SVREQ, ERR and the status bits follow `busyUntil_`. Holding the bus while
// BUSY (PAUSE) is the CARD's job: it reads busyUntil() and inserts the wait states.

#include <cstdint>
#include <string>

namespace altair {

class StateWriter;
class StateReader;

class Am95xx {
public:
    virtual ~Am95xx() = default;
    const std::string& name() const { return name_; }

    // ---- bus access. The CARD decodes C/D to these. ----
    void    writeData(uint8_t v, uint64_t now);     // C/D = 0, write: push a byte
    uint8_t readData(uint64_t now);                 // C/D = 0, read: pop a byte
    void    writeCommand(uint8_t v, uint64_t now);  // C/D = 1, write
    uint8_t readStatus(uint64_t now);               // C/D = 1, read (clears END and ERR)

    // ---- the handshake inputs and outputs ----
    void eack(uint64_t now);   // EACK low: clears END
    void svack();              // SVACK low: clears SVREQ
    bool endOut(uint64_t now) const;    // END asserted (pin LOW on a 9511A, HIGH on a 9512)
    bool svreqOut(uint64_t now) const;  // SVREQ asserted (the pin is HIGH)
    bool errOut(uint64_t now) const;    // ERR asserted (9512 only; a 9511A has no ERR pin)

    bool     busy(uint64_t now) const { return now < busyUntil_; }
    uint64_t busyUntil() const { return busyUntil_; }  // the CLK edge the command ends at

    // RESET: stops the command, status 0, stack pointer to its start, END, SVREQ and ERR
    // off. The stack bytes are kept (AMD: "may be affected"; Intel: "not affected").
    void reset();

    std::string describe(uint64_t now) const;  // "idle sp=4 status=00 TOS=03A00000"

    // The time left on a running command travels, not the CLK edge it ends at.
    void serialize(StateWriter& w, uint64_t now) const;
    void deserialize(StateReader& r, uint64_t now);

    // The status bits both chips share.
    static constexpr uint8_t kBusy = 0x80;
    static constexpr uint8_t kSign = 0x40;
    static constexpr uint8_t kZero = 0x20;

protected:
    // `errMask` is the status bits that drive ERR: 0 for a chip with no ERR pin.
    Am95xx(std::string name, uint8_t errMask) : name_(std::move(name)), errMask_(errMask) {}

    // Work out command `op` (the SR bit removed); set status_; return its cycle count.
    virtual unsigned execute(uint8_t op) = 0;

    // The stack is a 16-byte ring and a byte pointer (`sp_` = the next push slot). Push LS
    // byte first, pop MS byte first, so the MS byte of TOS sits just below the pointer.
    // Entry k counts down from the top in units of the width: get32(0) is TOS, get32(1) NOS.
    uint8_t& at(int i) { return stk_[(sp_ - 1 - i) & 15]; }
    uint8_t  at(int i) const { return stk_[(sp_ - 1 - i) & 15]; }
    uint16_t get16(int k) const;
    uint32_t get32(int k) const;
    uint64_t get64(int k) const;
    void     set16(int k, uint16_t v);
    void     set32(int k, uint32_t v);
    void     set64(int k, uint64_t v);
    void     lift(int n) { sp_ = (uint8_t)((sp_ + n) & 15); }  // make room for n bytes
    void     drop(int n) { sp_ = (uint8_t)((sp_ - n) & 15); }  // discard n bytes

    uint8_t status_ = 0;  // the status after the last command (BUSY is computed)

private:
    std::string name_;
    uint8_t  errMask_;
    uint8_t  stk_[16] = {};
    uint8_t  sp_      = 0;
    uint64_t busyUntil_ = 0;
    bool     endArmed_  = false;  // the END flip-flop: set at busyUntil_ unless cleared
    bool     errBefore_ = false;  // ERR while the command runs (an earlier error)...
    bool     errAfter_  = false;  // ...and this command's own error, from its end
    bool     svrqBefore_ = false; // SVREQ while the command runs...
    bool     svrqAfter_  = false; // ...and from its end (its SR bit)
};

}  // namespace altair
