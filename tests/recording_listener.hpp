#pragma once
// Records every event as a compact string so two books can be compared
// event for event, and scenario tests can assert on the exact sequence.
#include <string>
#include <vector>

#include "me/types.hpp"

namespace me::test {

inline const char* name(Side s) { return s == Side::Buy ? "B" : "S"; }

inline const char* name(OrderType t) {
    switch (t) {
        case OrderType::Limit: return "LMT";
        case OrderType::Market: return "MKT";
        case OrderType::IOC: return "IOC";
        case OrderType::FOK: return "FOK";
        case OrderType::PostOnly: return "PO";
    }
    return "?";
}

inline const char* name(RejectReason r) {
    switch (r) {
        case RejectReason::InvalidQuantity: return "bad_qty";
        case RejectReason::InvalidPrice: return "bad_px";
        case RejectReason::DuplicateOrderId: return "dup_id";
        case RejectReason::UnknownOrderId: return "unknown_id";
        case RejectReason::PostOnlyWouldTake: return "po_would_take";
    }
    return "?";
}

inline const char* name(CancelReason r) {
    switch (r) {
        case CancelReason::UserRequested: return "user";
        case CancelReason::UnfilledRemainder: return "remainder";
        case CancelReason::FillOrKillFailed: return "fok_failed";
    }
    return "?";
}

struct RecordingListener {
    std::vector<std::string>* log;

    void push(std::string s) { log->push_back(std::move(s)); }
    static std::string n(long long v) { return std::to_string(v); }

    void on_accepted(OrderId id, Side s, OrderType t, Price p, Qty q) {
        push("accept " + n(id) + " " + name(s) + " " + name(t) + " " + n(p) + "x" + n(q));
    }
    void on_trade(const Trade& t) {
        push("trade " + n(t.taker) + "<-" + n(t.maker) + " " + name(t.taker_side) + " " + n(t.price) + "x" + n(t.qty));
    }
    void on_rested(OrderId id, Side s, Price p, Qty q) { push("rest " + n(id) + " " + name(s) + " " + n(p) + "x" + n(q)); }
    void on_cancelled(OrderId id, Qty q, CancelReason r) { push("cancel " + n(id) + " " + n(q) + " " + name(r)); }
    void on_modified(OrderId id, Price p, Qty q) { push("modify " + n(id) + " " + n(p) + "x" + n(q)); }
    void on_rejected(OrderId id, RejectReason r) { push("reject " + n(id) + " " + name(r)); }
};

}  // namespace me::test
