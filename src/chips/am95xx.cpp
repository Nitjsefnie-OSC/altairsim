#include "chips/am95xx.h"

#include "core/statefile.h"

#include <cstdio>

namespace altair {

// ---------------------------------------------------------------------------
// THE STACK.
// ---------------------------------------------------------------------------
uint16_t Am95xx::get16(int k) const { return (uint16_t)(at(2 * k) << 8 | at(2 * k + 1)); }
uint32_t Am95xx::get32(int k) const {
    return (uint32_t)at(4 * k) << 24 | (uint32_t)at(4 * k + 1) << 16 |
           (uint32_t)at(4 * k + 2) << 8 | at(4 * k + 3);
}
uint64_t Am95xx::get64(int k) const {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | at(8 * k + i);
    return v;
}
void Am95xx::set16(int k, uint16_t v) {
    at(2 * k)     = (uint8_t)(v >> 8);
    at(2 * k + 1) = (uint8_t)v;
}
void Am95xx::set32(int k, uint32_t v) {
    at(4 * k)     = (uint8_t)(v >> 24);
    at(4 * k + 1) = (uint8_t)(v >> 16);
    at(4 * k + 2) = (uint8_t)(v >> 8);
    at(4 * k + 3) = (uint8_t)v;
}
void Am95xx::set64(int k, uint64_t v) {
    for (int i = 7; i >= 0; --i, v >>= 8) at(8 * k + i) = (uint8_t)v;
}

// ---------------------------------------------------------------------------
// BUS ACCESS. Any read or write clears the END flip-flop -- once it is set: an access while
// a command runs (only a status read can arrive then; PAUSE holds the rest) does not stop
// the flip-flop setting at the end (9511A p.7-67, 9512 p.7-92).
// ---------------------------------------------------------------------------
void Am95xx::writeData(uint8_t v, uint64_t now) {
    if (now >= busyUntil_) endArmed_ = false;
    stk_[sp_] = v;
    lift(1);
}

uint8_t Am95xx::readData(uint64_t now) {
    if (now >= busyUntil_) endArmed_ = false;
    drop(1);
    return stk_[sp_];  // reading does not alter the data; past empty it re-reads old bytes
}

// A status read also clears ERR (9512 p.7-92). While a command runs it clears only an
// earlier error's ERR: this command's error comes up at its end.
uint8_t Am95xx::readStatus(uint64_t now) {
    errBefore_ = false;
    if (now < busyUntil_) return kBusy;  // the other bits are not defined while BUSY
    endArmed_ = false;
    errAfter_ = false;
    return status_;
}

void Am95xx::writeCommand(uint8_t v, uint64_t now) {
    svrqBefore_ = svreqOut(now);
    svrqAfter_  = (v & 0x80) != 0;  // SR: set at the end; an SR = 0 command clears it
    errBefore_  = errOut(now);      // only a status read or RESET clears ERR
    endArmed_   = true;
    busyUntil_  = now + execute(v & 0x7F);
    errAfter_   = (status_ & errMask_) != 0;
}

void Am95xx::eack(uint64_t now) {
    if (now >= busyUntil_) endArmed_ = false;
}

void Am95xx::svack() { svrqBefore_ = svrqAfter_ = false; }

bool Am95xx::endOut(uint64_t now) const { return endArmed_ && now >= busyUntil_; }

bool Am95xx::svreqOut(uint64_t now) const {
    return now >= busyUntil_ ? svrqAfter_ : svrqBefore_;
}

bool Am95xx::errOut(uint64_t now) const {
    return errBefore_ || (errAfter_ && now >= busyUntil_);
}

void Am95xx::reset() {
    sp_ = 0;
    status_ = 0;
    busyUntil_ = 0;
    endArmed_ = errBefore_ = errAfter_ = false;
    svrqBefore_ = svrqAfter_ = false;
}

std::string Am95xx::describe(uint64_t now) const {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s sp=%u status=%02X TOS=%08X%s%s%s",
                  busy(now) ? "busy" : "idle", (unsigned)sp_,
                  (unsigned)(busy(now) ? kBusy : status_), (unsigned)get32(0),
                  endOut(now) ? " END" : "", svreqOut(now) ? " SVREQ" : "",
                  errOut(now) ? " ERR" : "");
    return buf;
}

void Am95xx::serialize(StateWriter& w, uint64_t now) const {
    w.raw(stk_, sizeof stk_);
    w.u8(sp_);
    w.u8(status_);
    w.u64(busyUntil_ > now ? busyUntil_ - now : 0);  // the time left, not the edge
    w.boolean(endArmed_);
    w.boolean(errBefore_);
    w.boolean(errAfter_);
    w.boolean(svrqBefore_);
    w.boolean(svrqAfter_);
}

void Am95xx::deserialize(StateReader& r, uint64_t now) {
    r.raw(stk_, sizeof stk_);
    sp_         = r.u8() & 15;
    status_     = r.u8();
    busyUntil_  = now + r.u64();
    endArmed_   = r.boolean();
    errBefore_  = r.boolean();
    errAfter_   = r.boolean();
    svrqBefore_ = r.boolean();
    svrqAfter_  = r.boolean();
}

}  // namespace altair
