// fixlog: answers support questions from QuickFIX message logs.
//
//   fixlog summary LOG...          what's in the log
//   fixlog order CLORDID LOG...    every message for one order, cancels and replaces included
//   fixlog why CLORDID LOG...      what happened to the order and why, in plain English
//   fixlog rejects LOG...          every reject and its reason
//   fixlog gaps LOG...             sequence gaps, resend requests, resets
//
// LOG is a QuickFIX *.messages.current.log (or any file with one FIX message per line).

#include <iostream>
#include <string>
#include <vector>

#include "FixLog.h"

namespace {
int usage() {
    std::cerr << "usage: fixlog summary|rejects|gaps LOG...\n"
                 "       fixlog order|why CLORDID LOG...\n";
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    const bool needsId = cmd == "order" || cmd == "why";
    if (needsId && argc < 4) return usage();
    std::vector<std::string> files(argv + (needsId ? 3 : 2), argv + argc);

    try {
        auto msgs = fixlog::load(files);
        if (cmd == "summary") {
            std::cout << fixlog::summary(msgs);
        } else if (cmd == "order") {
            auto hist = fixlog::orderHistory(msgs, argv[2]);
            if (hist.empty()) std::cout << "no messages for " << argv[2] << "\n";
            for (auto* m : hist)
                std::cout << m->time << "  " << m->get(49) << " -> " << m->get(56) << "  seq "
                          << m->get(34) << "  " << fixlog::describe(*m) << "\n";
        } else if (cmd == "why") {
            std::cout << fixlog::explain(msgs, argv[2]);
        } else if (cmd == "rejects") {
            auto r = fixlog::rejects(msgs);
            if (r.empty()) std::cout << "no rejects\n";
            for (auto& line : r) std::cout << line << "\n";
        } else if (cmd == "gaps") {
            auto g = fixlog::checkGaps(msgs);
            for (auto& n : g.notes) std::cout << n << "\n";
            if (g.clean()) {
                std::cout << "OK: no unrecovered sequence gaps\n";
            } else {
                for (auto& f : g.findings) std::cout << "PROBLEM: " << f << "\n";
                return 1;
            }
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        std::cerr << "fixlog: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
