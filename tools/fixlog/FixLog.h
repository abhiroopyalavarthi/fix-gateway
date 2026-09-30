#pragma once

// Parsing and analysis for QuickFIX message logs, for the fixlog CLI.
// Plain C++, no QuickFIX, so it works on any FIX log someone sends you.
//
// A QuickFIX FileLog line looks like
//   20260930-02:41:07.123456789 : 8=FIX.4.4<SOH>9=...<SOH>35=D<SOH>...
// Both directions of a session are in the same file; the direction comes from
// SenderCompID (49). '|' is accepted as a separator too, for hand-written logs.

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fixlog {

struct Msg {
    std::string time;  // as written in the log
    std::vector<std::pair<int, std::string>> fields;
    size_t line = 0;   // line number in its file
    std::string file;

    std::string get(int tag) const;
    bool has(int tag) const;
    std::string type() const { return get(35); }
    int64_t seq() const;
};

std::optional<Msg> parseLine(const std::string& line);
std::vector<Msg> load(const std::vector<std::string>& paths);
std::vector<Msg> loadText(const std::string& text);  // for tests

const char* typeName(const std::string& msgType);
const char* execTypeName(const std::string& v);
const char* ordStatusName(const std::string& v);

// One-line human description of a message: "ExecutionReport Trade 100@255.00 cum=100 leaves=0 ..."
std::string describe(const Msg& m);

// Every message that belongs to this order: follows ClOrdID -> OrigClOrdID
// chains through cancels and replaces, and matches OrderID.
std::vector<const Msg*> orderHistory(const std::vector<Msg>& msgs, const std::string& clOrdId);

// Plain-English answer to "what happened to this order and why".
std::string explain(const std::vector<Msg>& msgs, const std::string& clOrdId);

struct GapReport {
    std::vector<std::string> findings;  // problems: gaps never filled, sequence going backwards
    std::vector<std::string> notes;     // what happened: gaps detected, resend requests, resets
    bool clean() const { return findings.empty(); }
};
// Checks MsgSeqNum continuity per direction and lists resend requests,
// sequence resets and resent (PossDup) messages.
GapReport checkGaps(const std::vector<Msg>& msgs);

// Rejects of every kind: execution reports with ExecType=8, cancel rejects,
// session-level rejects and business message rejects.
std::vector<std::string> rejects(const std::vector<Msg>& msgs);

std::string summary(const std::vector<Msg>& msgs);

}  // namespace fixlog
