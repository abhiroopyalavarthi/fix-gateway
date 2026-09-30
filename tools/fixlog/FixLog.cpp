#include "FixLog.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fixlog {

// ---------------------------------------------------------------------------
// parsing

std::string Msg::get(int tag) const {
    for (auto& [t, v] : fields)
        if (t == tag) return v;
    return {};
}

bool Msg::has(int tag) const {
    return std::any_of(fields.begin(), fields.end(), [&](auto& f) { return f.first == tag; });
}

int64_t Msg::seq() const {
    std::string s = get(34);
    return s.empty() ? -1 : std::stoll(s);
}

std::optional<Msg> parseLine(const std::string& raw) {
    // the message starts at "8=FIX"; anything before " : " is the timestamp
    size_t start = raw.find("8=FIX");
    if (start == std::string::npos) return std::nullopt;
    Msg m;
    size_t colon = raw.rfind(" : ", start);
    if (colon != std::string::npos) m.time = raw.substr(0, colon);
    char sep = '\x01';
    size_t soh = raw.find('\x01', start);
    if (soh == std::string::npos) sep = '|';
    size_t pos = start;
    while (pos < raw.size()) {
        size_t end = raw.find(sep, pos);
        if (end == std::string::npos) end = raw.size();
        std::string_view f(raw.data() + pos, end - pos);
        size_t eq = f.find('=');
        if (eq != std::string_view::npos && eq > 0) {
            int tag = 0;
            bool ok = true;
            for (char c : f.substr(0, eq)) {
                if (c < '0' || c > '9') { ok = false; break; }
                tag = tag * 10 + (c - '0');
            }
            if (ok) m.fields.emplace_back(tag, std::string(f.substr(eq + 1)));
        }
        pos = end + 1;
    }
    if (!m.has(35)) return std::nullopt;
    return m;
}

std::vector<Msg> loadText(const std::string& text) {
    std::vector<Msg> out;
    std::istringstream in(text);
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (auto m = parseLine(line)) {
            m->line = n;
            out.push_back(std::move(*m));
        }
    }
    return out;
}

std::vector<Msg> load(const std::vector<std::string>& paths) {
    std::vector<Msg> all;
    for (const auto& p : paths) {
        std::ifstream f(p);
        if (!f) throw std::runtime_error("can't open " + p);
        std::stringstream ss;
        ss << f.rdbuf();
        for (Msg& m : loadText(ss.str())) {
            m.file = p;
            all.push_back(std::move(m));
        }
    }
    // several files: merge by timestamp (QuickFIX timestamps sort as text)
    if (paths.size() > 1)
        std::stable_sort(all.begin(), all.end(), [](const Msg& a, const Msg& b) { return a.time < b.time; });
    return all;
}

// ---------------------------------------------------------------------------
// names

const char* typeName(const std::string& t) {
    static const std::map<std::string, const char*> names = {
        {"0", "Heartbeat"}, {"1", "TestRequest"}, {"2", "ResendRequest"}, {"3", "Reject"},
        {"4", "SequenceReset"}, {"5", "Logout"}, {"8", "ExecutionReport"}, {"9", "OrderCancelReject"},
        {"A", "Logon"}, {"D", "NewOrderSingle"}, {"F", "OrderCancelRequest"},
        {"G", "OrderCancelReplaceRequest"}, {"H", "OrderStatusRequest"}, {"j", "BusinessMessageReject"}};
    auto it = names.find(t);
    return it == names.end() ? "?" : it->second;
}

const char* execTypeName(const std::string& v) {
    static const std::map<std::string, const char*> names = {
        {"0", "New"}, {"4", "Canceled"}, {"5", "Replaced"}, {"8", "Rejected"}, {"F", "Trade"},
        {"C", "Expired"}, {"I", "OrderStatus"}, {"1", "PartialFill"}, {"2", "Fill"}};
    auto it = names.find(v);
    return it == names.end() ? "?" : it->second;
}

const char* ordStatusName(const std::string& v) {
    static const std::map<std::string, const char*> names = {
        {"0", "New"}, {"1", "PartiallyFilled"}, {"2", "Filled"}, {"4", "Canceled"},
        {"5", "Replaced"}, {"8", "Rejected"}, {"C", "Expired"}};
    auto it = names.find(v);
    return it == names.end() ? "?" : it->second;
}

namespace {

std::string sideName(const std::string& s) { return s == "1" ? "buy" : s == "2" ? "sell" : s; }

// the quantity fields are floats in QuickFIX ("100" or "100.0"); print them as integers
std::string qty(const std::string& s) {
    if (s.empty()) return s;
    size_t dot = s.find('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

}  // namespace

std::string describe(const Msg& m) {
    std::ostringstream o;
    const std::string t = m.type();
    o << typeName(t);
    if (t == "D" || t == "G") {
        o << ' ' << sideName(m.get(54)) << ' ' << qty(m.get(38)) << ' ' << m.get(55);
        o << (m.get(40) == "1" ? " market" : " @ " + m.get(44));
        if (m.has(41)) o << " (replaces " << m.get(41) << ')';
    } else if (t == "F") {
        o << " cancel " << m.get(41);
    } else if (t == "8") {
        o << ' ' << execTypeName(m.get(150)) << " status=" << ordStatusName(m.get(39));
        if (m.get(150) == "F") o << ' ' << qty(m.get(32)) << '@' << m.get(31);
        o << " cum=" << qty(m.get(14)) << " leaves=" << qty(m.get(151));
        if (m.get(14) != "0" && !m.get(14).empty()) o << " avg=" << m.get(6);
    } else if (t == "9") {
        o << " for " << m.get(41) << " (" << (m.get(434) == "2" ? "replace" : "cancel") << ")";
    } else if (t == "3") {
        o << " of seq " << m.get(45) << " tag " << m.get(371);
    } else if (t == "2") {
        o << ' ' << m.get(7) << ".." << (m.get(16) == "0" ? "latest" : m.get(16));
    } else if (t == "4") {
        o << (m.get(123) == "Y" ? " gap fill" : " reset") << " -> " << m.get(36);
    }
    if (m.has(58)) o << "  \"" << m.get(58) << '"';
    if (m.get(43) == "Y") o << "  [PossDup]";
    return o.str();
}

// ---------------------------------------------------------------------------
// order history

std::vector<const Msg*> orderHistory(const std::vector<Msg>& msgs, const std::string& clOrdId) {
    // grow the set of ids until it stops changing: ClOrdIDs linked through 41
    // (replaces and cancels) and the gateway's OrderID (37)
    std::set<std::string> ids{clOrdId}, orderIds;
    for (bool changed = true; changed;) {
        changed = false;
        for (const Msg& m : msgs) {
            std::string c = m.get(11), orig = m.get(41), oid = m.get(37);
            bool hit = ids.count(c) || (!orig.empty() && ids.count(orig)) ||
                       (!oid.empty() && oid != "NONE" && orderIds.count(oid));
            if (!hit) continue;
            if (!c.empty()) changed |= ids.insert(c).second;
            if (!orig.empty()) changed |= ids.insert(orig).second;
            if (!oid.empty() && oid != "NONE") changed |= orderIds.insert(oid).second;
        }
    }
    std::vector<const Msg*> out;
    for (const Msg& m : msgs) {
        std::string c = m.get(11), orig = m.get(41), oid = m.get(37);
        if (ids.count(c) || (!orig.empty() && ids.count(orig)) || (!oid.empty() && orderIds.count(oid)))
            out.push_back(&m);
    }
    return out;
}

std::string explain(const std::vector<Msg>& msgs, const std::string& clOrdId) {
    std::ostringstream o;
    auto hist = orderHistory(msgs, clOrdId);
    if (hist.empty())
        return "No message in this log mentions " + clOrdId +
               ". Check the id and that this is the right session's log; if the client says it sent "
               "it, look for a sequence gap (fixlog gaps) or check the client's own log.\n";

    const Msg* newOrder = nullptr;
    for (auto* m : hist)
        if (m->type() == "D") { newOrder = m; break; }
    if (newOrder) {
        o << clOrdId << ": " << describe(*newOrder) << ", received " << newOrder->time << " from "
          << newOrder->get(49) << ".\n";
        // a session-level reject points back at the message by sequence number
        for (const Msg& m : msgs)
            if (m.type() == "3" && m.get(45) == newOrder->get(34) && m.get(56) == newOrder->get(49))
                o << "Rejected at the session layer (never reached the order handler): \"" << m.get(58)
                  << "\" (tag " << m.get(371) << ").\n";
    } else {
        o << clOrdId << ": the NewOrderSingle isn't in this log (sent before the log starts?).\n";
    }

    std::vector<const Msg*> reports;
    for (auto* m : hist)
        if (m->type() == "8") reports.push_back(m);
    if (reports.empty()) {
        o << "The gateway sent no execution report for it. Check the event log and `fixlog gaps`.\n";
        return o.str();
    }

    int fills = 0;
    std::string firstFill, lastFill;
    for (auto* r : reports)
        if (r->get(150) == "F") {
            ++fills;
            if (firstFill.empty()) firstFill = r->time;
            lastFill = r->time;
        }
    for (auto* m : hist) {
        if (m->type() == "G") o << "Replace requested " << m->time << ": " << describe(*m) << ".\n";
        if (m->type() == "9")
            o << "A " << (m->get(434) == "2" ? "replace" : "cancel") << " was refused " << m->time
              << ": \"" << m->get(58) << "\".\n";
    }

    const Msg& last = *reports.back();
    const std::string st = last.get(39), cum = qty(last.get(14)), leaves = qty(last.get(151));
    if (st == "8") {
        o << "REJECTED " << last.time << ": \"" << last.get(58) << "\".\n";
        o << "It never reached the book.\n";
    } else if (st == "2") {
        o << "FILLED: " << cum << " shares at an average " << last.get(6) << " in " << fills << " fill"
          << (fills == 1 ? "" : "s") << " (" << firstFill << " to " << lastFill << ").\n";
    } else if (st == "4") {
        o << "CANCELED " << last.time << " with " << cum << " filled";
        // the cancel request that worked is the one the Canceled report answers
        const Msg* cxl = nullptr;
        for (auto* m : hist)
            if (m->type() == "F" && m->get(11) == last.get(11)) cxl = m;
        if (last.get(58) == "kill switch") o << ", by the kill switch.\n";
        else if (cxl) o << ", by the client's cancel request " << cxl->get(11) << " sent " << cxl->time << ".\n";
        else if (last.has(58)) o << ": \"" << last.get(58) << "\".\n";
        else o << ".\n";
    } else {
        // still working: say why it hasn't filled, using trades in the same symbol
        const std::string sym = last.get(55), side = newOrder ? newOrder->get(54) : "";
        std::string limit = last.get(44);
        o << "STILL WORKING (" << ordStatusName(st) << "): " << cum << " filled, " << leaves
          << " resting at " << (limit.empty() ? "market" : limit) << " as of " << last.time << ".\n";
        double lo = 1e18, hi = -1e18;
        int n = 0;
        for (const Msg& m : msgs)
            if (m.type() == "8" && m.get(150) == "F" && m.get(55) == sym && m.time >= last.time) {
                double px = std::stod(m.get(31));
                lo = std::min(lo, px);
                hi = std::max(hi, px);
                ++n;
            }
        if (!limit.empty()) {
            if (n == 0)
                o << "No " << sym << " trades show up in this log after that, so nothing traded at or "
                     "through its price.\n";
            else {
                double lim = std::stod(limit);
                bool buy = side == "1";
                o << sym << " traded " << n << " times after that, between " << lo << " and " << hi << ". ";
                if (buy && lo > lim) o << "Every trade was above the " << limit << " limit, so no seller came down to it.\n";
                else if (!buy && hi < lim) o << "Every trade was below the " << limit << " limit, so no buyer came up to it.\n";
                else o << "Some trades were at or through the limit, so it was behind other orders at its price (time priority).\n";
            }
        }
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// sequence gaps

GapReport checkGaps(const std::vector<Msg>& msgs) {
    GapReport rep;
    struct Dir {
        int64_t expected = -1;
        std::set<int64_t> missing;
        std::map<int64_t, std::string> missingSince;  // first missing seq of a gap -> time found
        size_t resent = 0;
    };
    std::map<std::string, Dir> dirs;
    std::vector<std::string> notes;

    for (const Msg& m : msgs) {
        const std::string key = m.get(49) + " -> " + m.get(56);
        Dir& d = dirs[key];
        const int64_t seq = m.seq();
        if (seq < 0) continue;
        const bool possDup = m.get(43) == "Y";
        const std::string t = m.type();

        if (t == "2")
            notes.push_back(m.time + "  " + m.get(49) + " asked " + m.get(56) + " to resend " + m.get(7) +
                            ".." + (m.get(16) == "0" ? "latest" : m.get(16)));

        if (t == "A" && m.get(141) == "Y") {  // logon with reset: numbering restarts
            d.expected = seq + 1;
            d.missing.clear();
            notes.push_back(m.time + "  " + key + " reset sequence numbers at logon");
            continue;
        }
        if (possDup) {
            ++d.resent;
            int64_t upto = seq;
            if (t == "4" && m.get(123) == "Y" && m.has(36)) upto = std::stoll(m.get(36)) - 1;  // gap fill
            for (int64_t s = seq; s <= upto; ++s) d.missing.erase(s);
            if (upto >= d.expected) d.expected = upto + 1;
            continue;
        }
        if (t == "4" && m.get(123) != "Y" && m.has(36)) {  // hard reset
            d.expected = std::stoll(m.get(36));
            notes.push_back(m.time + "  " + key + " SequenceReset to " + m.get(36));
            continue;
        }
        if (d.expected < 0) d.expected = seq;
        if (seq > d.expected) {
            for (int64_t s = d.expected; s < seq; ++s) d.missing.insert(s);
            d.missingSince[d.expected] = m.time;
            notes.push_back(m.time + "  " + key + " gap: expected " + std::to_string(d.expected) +
                            ", got " + std::to_string(seq) + " (" + std::to_string(seq - d.expected) +
                            " missing)");
        } else if (seq < d.expected) {
            rep.findings.push_back(m.time + "  " + key + " seq " + std::to_string(seq) +
                                   " is lower than expected " + std::to_string(d.expected) +
                                   " without PossDupFlag (duplicate or reset?)");
        }
        if (t == "4" && m.get(123) == "Y" && m.has(36)) d.expected = std::stoll(m.get(36));
        else d.expected = std::max(d.expected, seq + 1);
    }

    for (auto& [key, d] : dirs) {
        if (d.resent)
            notes.push_back(key + ": " + std::to_string(d.resent) + " messages resent with PossDupFlag=Y");
        if (d.missing.empty()) continue;
        // compress into ranges
        int64_t a = *d.missing.begin(), prev = a;
        auto flush = [&](int64_t x, int64_t y) {
            rep.findings.push_back(key + ": messages " + std::to_string(x) + ".." + std::to_string(y) +
                                   " never arrived and were never resent");
        };
        for (auto it = std::next(d.missing.begin()); it != d.missing.end(); ++it) {
            if (*it != prev + 1) {
                flush(a, prev);
                a = *it;
            }
            prev = *it;
        }
        flush(a, prev);
    }
    // notes go after the problems
    for (auto& n : notes) rep.notes.push_back(n);
    return rep;
}

// ---------------------------------------------------------------------------

std::vector<std::string> rejects(const std::vector<Msg>& msgs) {
    std::vector<std::string> out;
    for (const Msg& m : msgs) {
        const std::string t = m.type();
        std::string what;
        if (t == "8" && m.get(150) == "8") what = "order " + m.get(11) + " rejected";
        else if (t == "9") what = (m.get(434) == "2" ? "replace " : "cancel ") + m.get(11) + " of " + m.get(41) + " refused";
        else if (t == "3") what = "session reject of seq " + m.get(45) + " (tag " + m.get(371) + ")";
        else if (t == "j") what = "business reject of seq " + m.get(45);
        else continue;
        out.push_back(m.time + "  " + m.get(49) + " -> " + m.get(56) + "  " + what + ": \"" + m.get(58) + "\"");
    }
    return out;
}

std::string summary(const std::vector<Msg>& msgs) {
    std::ostringstream o;
    if (msgs.empty()) return "no FIX messages found\n";
    std::map<std::string, std::map<std::string, int>> counts;  // direction -> type -> n
    std::set<std::string> orders;
    int fills = 0, rej = 0;
    for (const Msg& m : msgs) {
        counts[m.get(49) + " -> " + m.get(56)][typeName(m.type())]++;
        if (m.type() == "D") orders.insert(m.get(49) + "/" + m.get(11));
        if (m.type() == "8" && m.get(150) == "F") ++fills;
        if (m.type() == "8" && m.get(150) == "8") ++rej;
    }
    o << msgs.size() << " messages, " << msgs.front().time << " to " << msgs.back().time << "\n";
    o << orders.size() << " orders, " << fills << " fill reports, " << rej << " rejected\n";
    for (auto& [dir, byType] : counts) {
        o << dir << ":";
        for (auto& [t, n] : byType) o << "  " << t << "=" << n;
        o << "\n";
    }
    return o.str();
}

}  // namespace fixlog
