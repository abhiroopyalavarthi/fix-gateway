#include "FixClient.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "quickfix/Session.h"
#include "quickfix/fix44/ExecutionReport.h"
#include "quickfix/fix44/NewOrderSingle.h"
#include "quickfix/fix44/OrderCancelReject.h"
#include "quickfix/fix44/OrderCancelReplaceRequest.h"
#include "quickfix/fix44/OrderCancelRequest.h"

namespace {
std::string field(const FIX::FieldMap& m, int tag) {
    return m.isSetField(tag) ? m.getField(tag) : std::string();
}
}  // namespace

FixClient::FixClient(std::string journalPath) : journalPath_(std::move(journalPath)) {
    if (journalPath_.empty()) return;
    // replay what this client already knew before it was restarted
    std::ifstream in(journalPath_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "SENT") {
            std::string id;
            ss >> id;
            sent_.push_back(id);
        } else if (kind == "ER") {
            std::string execId, clOrdId, orig;
            ClientOrder r;
            ss >> execId >> clOrdId >> orig >> r.orderId >> r.status >> r.lastExecType >> r.cum >>
                r.leaves >> r.avgPx;
            if (orig == "-") orig.clear();
            seenExec_.insert(execId);
            if (!orig.empty() && orig != clOrdId) orders_.erase(orig);
            orders_[clOrdId] = r;
        }
    }
    journalOut_.open(journalPath_, std::ios::app);
}

void FixClient::journal(const std::string& line) {
    if (!journalOut_.is_open()) return;
    // written from the main thread (SENT) and QuickFIX's thread (ER); ofstream isn't thread-safe
    std::lock_guard<std::mutex> lk(journalMu_);
    journalOut_ << line << '\n';
    journalOut_.flush();  // survives kill -9 (it's in the OS page cache)
}

FIX::SessionSettings FixClient::makeSettings(const std::string& cfgPath, const std::string& sender,
                                             const std::string& storeRoot, bool resetOnLogon) {
    FIX::SessionSettings file(cfgPath);
    FIX::Dictionary defaults = file.get();
    FIX::SessionSettings out;
    out.set(defaults);
    FIX::Dictionary d;
    d.setString("SenderCompID", sender);
    if (resetOnLogon) d.setString("ResetOnLogon", "Y");
    if (!storeRoot.empty()) {
        d.setString("FileStorePath", storeRoot + "/store");
        d.setString("FileLogPath", storeRoot + "/log");
    }
    FIX::SessionID sid(defaults.getString("BeginString"), sender, defaults.getString("TargetCompID"));
    out.set(sid, d);
    return out;
}

void FixClient::onLogon(const FIX::SessionID&) {
    loggedOn_ = true;
    cv_.notify_all();
}

void FixClient::onLogout(const FIX::SessionID&) {
    loggedOn_ = false;
    cv_.notify_all();
}

bool FixClient::waitLogon(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, timeout, [&] { return loggedOn_.load(); });
}

bool FixClient::waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, timeout, [&] {
        lk.unlock();
        bool ok = pred();
        lk.lock();
        return ok;
    });
}

// ---------------------------------------------------------------------------

void FixClient::sendNew(const std::string& clOrdId, const std::string& symbol, char side,
                        int64_t qty, const std::string& price, char tif, bool possResend) {
    FIX::TransactTime now;
    FIX44::NewOrderSingle m(FIX::ClOrdID(clOrdId), FIX::Side(side), now,
                            FIX::OrdType(price.empty() ? FIX::OrdType_MARKET : FIX::OrdType_LIMIT));
    m.set(FIX::Symbol(symbol));
    m.set(FIX::OrderQty(static_cast<double>(qty)));
    if (!price.empty()) m.setField(FIX::FIELD::Price, price);  // exact text
    m.set(FIX::TimeInForce(tif));
    if (possResend) m.getHeader().setField(FIX::PossResend(true));  // 97 lives in the header
    if (!possResend) {
        std::lock_guard<std::mutex> lk(mu_);
        sent_.push_back(clOrdId);
    }
    journal("SENT " + clOrdId);
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_[clOrdId] = Clock::now();
    }
    FIX::Session::sendToTarget(m, session_);
}

void FixClient::sendCancel(const std::string& clOrdId, const std::string& origClOrdId,
                           const std::string& symbol, char side) {
    FIX::TransactTime now;
    FIX44::OrderCancelRequest m(FIX::OrigClOrdID(origClOrdId), FIX::ClOrdID(clOrdId), FIX::Side(side), now);
    m.set(FIX::Symbol(symbol));
    FIX::Session::sendToTarget(m, session_);
}

void FixClient::sendReplace(const std::string& clOrdId, const std::string& origClOrdId,
                            const std::string& symbol, char side, int64_t qty,
                            const std::string& price) {
    FIX::TransactTime now;
    FIX44::OrderCancelReplaceRequest m(FIX::OrigClOrdID(origClOrdId), FIX::ClOrdID(clOrdId),
                                       FIX::Side(side), now, FIX::OrdType(FIX::OrdType_LIMIT));
    m.set(FIX::Symbol(symbol));
    m.set(FIX::OrderQty(static_cast<double>(qty)));
    m.setField(FIX::FIELD::Price, price);
    FIX::Session::sendToTarget(m, session_);
}

// ---------------------------------------------------------------------------

void FixClient::fromAdmin(const FIX::Message& m, const FIX::SessionID&) {
    if (field(m.getHeader(), FIX::FIELD::MsgType) != "3") return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sessionRejects_.push_back(field(m, FIX::FIELD::Text));
    }
    cv_.notify_all();
}

std::vector<std::string> FixClient::sessionRejects() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessionRejects_;
}

void FixClient::fromApp(const FIX::Message& m, const FIX::SessionID& s) {
    currentIsResend_ = field(m.getHeader(), FIX::FIELD::PossDupFlag) == "Y";
    crack(m, s);
}

void FixClient::applyReport(const std::string& execId, const std::string& clOrdId,
                            const std::string& origClOrdId, const ClientOrder& r) {
    char avg[32];
    std::snprintf(avg, sizeof avg, "%.6f", r.avgPx);
    journal("ER " + execId + " " + clOrdId + " " + (origClOrdId.empty() ? "-" : origClOrdId) + " " +
            r.orderId + " " + r.status + " " + r.lastExecType + " " + std::to_string(r.cum) + " " +
            std::to_string(r.leaves) + " " + avg);
    if (!origClOrdId.empty() && origClOrdId != clOrdId) orders_.erase(origClOrdId);
    orders_[clOrdId] = r;
}

void FixClient::onMessage(const FIX44::ExecutionReport& m, const FIX::SessionID&) {
    auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++reports_;
        if (currentIsResend_) ++resent_;
        std::string execId = field(m, FIX::FIELD::ExecID);
        if (!seenExec_.insert(execId).second) {
            ++dupes_;  // already applied this one before a reconnect
        } else {
            std::string clOrdId = field(m, FIX::FIELD::ClOrdID);
            if (auto it = pending_.find(clOrdId); it != pending_.end()) {
                latencies_.push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - it->second).count());
                pending_.erase(it);
            }
            ClientOrder r;
            r.orderId = field(m, FIX::FIELD::OrderID);
            r.status = field(m, FIX::FIELD::OrdStatus)[0];
            r.lastExecType = field(m, FIX::FIELD::ExecType)[0];
            r.cum = static_cast<int64_t>(std::stod(field(m, FIX::FIELD::CumQty)));
            r.leaves = static_cast<int64_t>(std::stod(field(m, FIX::FIELD::LeavesQty)));
            r.avgPx = std::stod(field(m, FIX::FIELD::AvgPx));
            r.text = field(m, FIX::FIELD::Text);
            applyReport(execId, clOrdId, field(m, FIX::FIELD::OrigClOrdID), r);
        }
    }
    cv_.notify_all();
}

void FixClient::onMessage(const FIX44::OrderCancelReject& m, const FIX::SessionID&) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        CancelRejectSeen c;
        c.clOrdId = field(m, FIX::FIELD::ClOrdID);
        c.origClOrdId = field(m, FIX::FIELD::OrigClOrdID);
        c.text = field(m, FIX::FIELD::Text);
        std::string reason = field(m, FIX::FIELD::CxlRejReason);
        c.reason = reason.empty() ? 0 : std::stoi(reason);
        cxlRejects_.push_back(c);
        ++reports_;
    }
    cv_.notify_all();
}

// ---------------------------------------------------------------------------

std::optional<ClientOrder> FixClient::order(const std::string& clOrdId) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = orders_.find(clOrdId);
    if (it == orders_.end()) return std::nullopt;
    return it->second;
}

std::map<std::string, ClientOrder> FixClient::orders() const {
    std::lock_guard<std::mutex> lk(mu_);
    return orders_;
}

std::vector<CancelRejectSeen> FixClient::cancelRejects() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cxlRejects_;
}

size_t FixClient::reportCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return reports_;
}

size_t FixClient::duplicateReports() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dupes_;
}

size_t FixClient::resentReports() const {
    std::lock_guard<std::mutex> lk(mu_);
    return resent_;
}

std::vector<int64_t> FixClient::ackLatencies() const {
    std::lock_guard<std::mutex> lk(mu_);
    return latencies_;
}

size_t FixClient::ackCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return latencies_.size();
}

void FixClient::clearLatencies() {
    std::lock_guard<std::mutex> lk(mu_);
    latencies_.clear();
}

std::string FixClient::view() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, std::string>> rows;  // sort by numeric order id
    char avg[32];
    for (auto& [id, o] : orders_) {
        std::snprintf(avg, sizeof avg, "%.6f", o.avgPx);
        rows.push_back({o.orderId, id + " " + o.orderId + " " + o.status + " " + std::to_string(o.cum) +
                                       " " + std::to_string(o.leaves) + " " + avg});
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) {
        auto num = [](const std::string& s) {  // "O123" -> 123, "NONE" -> 0
            unsigned long long n = 0;
            for (char c : s)
                if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
            return n;
        };
        return num(a.first) < num(b.first);
    });
    std::string out;
    for (auto& r : rows) out += r.second + '\n';
    return out;
}
