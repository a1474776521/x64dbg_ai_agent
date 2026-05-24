// locator/locator_engine.cpp
#include "locator/locator_engine.h"

#include "ai/embedding_client.h"
#include "locator/api_scanner.h"
#include "locator/pattern_scanner.h"
#include "locator/string_scanner.h"
#include "storage/project_context.h"
#include "storage/session_store.h"
#include "util/logging.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace x64ai {

namespace {

std::string buildChunkText(const HeuristicHit& h)
{
    char head[256];
    std::snprintf(head, sizeof(head),
                  "[locator/%s] kind=%s va=0x%llX score=%d category=%s\n",
                  hitKindName(h.kind), hitKindName(h.kind),
                  static_cast<unsigned long long>(h.va),
                  h.score, h.category.c_str());
    std::string s = head;
    s.append("label: ");
    s.append(h.label);
    s.push_back('\n');
    if (!h.evidence.empty()) {
        s.append("evidence: ");
        s.append(h.evidence);
        s.push_back('\n');
    }
    if (h.refVa) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "ref: 0x%llX\n",
                      static_cast<unsigned long long>(h.refVa));
        s.append(buf);
    }
    return s;
}

}  // namespace

std::vector<HeuristicHit> LocatorEngine::runAll(const Options& opt,
                                                const ProgressFn& progress)
{
    std::vector<HeuristicHit> all;
    auto report = [&](const char* phase, int cur, int tot, bool done) {
        if (progress) progress(phase, cur, tot, done);
    };

    int phaseTotal = (int)opt.runApi + (int)opt.runString + (int)opt.runPattern;
    int phaseIdx = 0;

    // 关键字归一化：API/String 用小写；Pattern 保留原始（可能含 hex / 大小写敏感）
    std::vector<std::string> kwLower;
    kwLower.reserve(opt.userKeywords.size());
    for (const auto& k : opt.userKeywords) {
        if (k.empty()) continue;
        std::string s = k;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        kwLower.push_back(std::move(s));
    }

    if (opt.runApi) {
        report("API", ++phaseIdx, phaseTotal, false);
        auto v = ApiScanner::scan(/*maxXrefsPerApi=*/6, kwLower);
        all.insert(all.end(), v.begin(), v.end());
        report("API", phaseIdx, phaseTotal, true);
    }
    if (opt.runString) {
        report("String", ++phaseIdx, phaseTotal, false);
        StringScanner::Options so;
        so.maxXrefsPerStr = 2;
        so.userKeywords = kwLower;
        auto v = StringScanner::scan(so);
        all.insert(all.end(), v.begin(), v.end());
        report("String", phaseIdx, phaseTotal, true);
    }
    if (opt.runPattern) {
        report("Pattern", ++phaseIdx, phaseTotal, false);
        auto v = PatternScanner::scan(opt.userKeywords);
        all.insert(all.end(), v.begin(), v.end());
        report("Pattern", phaseIdx, phaseTotal, true);
    }

    XAI_LOG_INFO("LocatorEngine: total hits={}", static_cast<int>(all.size()));

    // 写 RAG
    if (opt.ragMinScore > 0) {
        auto store = ProjectContext::instance().store();
        if (store) {
            int written = 0;
            int candidates = 0;
            for (const auto& h : all) {
                if (h.score < opt.ragMinScore) continue;
                ++candidates;
            }
            int idx = 0;
            for (const auto& h : all) {
                if (h.score < opt.ragMinScore) continue;
                ++idx;
                report("RAG", idx, candidates, false);
                std::string text = buildChunkText(h);
                auto emb = EmbeddingClient::instance().embed(text);
                std::string kind = std::string("hit-") + hitKindName(h.kind);
                if (store->addChunk(kind, h.va, text, emb) > 0) ++written;
            }
            XAI_LOG_INFO("LocatorEngine: wrote {} chunks to RAG (candidates={})",
                         written, candidates);
            report("RAG", candidates, candidates, true);
        }
    }

    return all;
}

}  // namespace x64ai
