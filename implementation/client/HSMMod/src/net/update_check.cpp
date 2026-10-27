#include "net/update_check.h"
#include "net/http_client.h"
#include "gameplay/lobby.h"
#include "log.h"
#include "session/hsm_version.h"
#include <atomic>
#include <mutex>
#include <thread>

#ifndef HSM_GIT_TAG
#define HSM_GIT_TAG ""   // build.sh: the checkout's nearest v* tag as git describe prints it, if any
#endif

namespace hsm::update {
namespace {
constexpr const char kRepo[] = "Dudiebug/halfsword-multiplayer-mod";

std::atomic<bool> g_started{false};
std::mutex g_mu;
std::string g_notice;
bool g_chatShown = false;

// "check failed (...)" reason: what the player or a bug report can act on.
std::string Reason(const net::HttpResponse& r) {
    using Fail = net::HttpResponse::Fail;
    switch (r.fail) {
        case Fail::None: break;
        case Fail::Unavailable: return "WinHTTP is unavailable";
        case Fail::NoConnection: return "no connection";
        case Fail::Timeout: return "GitHub did not answer";
        case Fail::Certificate: return "GitHub's certificate is not valid";
        case Fail::Unreachable: return "cannot reach api.github.com";
        case Fail::Other: return "request failed (" + std::to_string(r.winError) + ")";
    }
    if (r.status == 403 || r.status == 429) return "GitHub is limiting requests (" + std::to_string(r.status) + ")";
    return "GitHub answered " + std::to_string(r.status);
}

void Check() {
    const std::string tag = HSM_GIT_TAG;
    const std::optional<Version> running = RunningVersion(game::kModVersion, tag);
    if (!running) { HSM_LOG("update: check failed (this build's version %s is not known)", game::kModVersion); return; }
    const std::string cur = FormatVersion(*running);

    net::HttpRequest req;
    req.host = L"api.github.com";
    req.path = L"/repos/" + net::Wide(kRepo) + L"/releases?per_page=10";
    req.headers = L"Accept: application/vnd.github+json";
    req.agent = L"HSMMod/" + net::Wide(cur);   // GitHub refuses a request without a User-Agent
    req.maxBody = kMaxReleasesJson;
    req.timeoutMs = 15000;
    const net::HttpResponse r = net::Http(req);
    if (r.fail != net::HttpResponse::Fail::None || r.status != 200) { HSM_LOG("update: check failed (%s)", Reason(r).c_str()); return; }
    const ReleaseList list = ParseReleases(r.body);
    if (list.releases.empty() && !list.complete) { HSM_LOG("update: check failed (the answer was not a release list)"); return; }

    const std::optional<UpdateOffer> offer = NewestOffer(list.releases, *running);
    if (!offer) { HSM_LOG("update: up to date (%s)", cur.c_str()); return; }
    const std::string latest = FormatVersion(offer->version);
    const std::string url = offer->url.empty() ? std::string("https://github.com/") + kRepo + "/releases" : offer->url;
    HSM_LOG("update: HSMMod %s is available (this is %s): %s", latest.c_str(), cur.c_str(), url.c_str());
    std::lock_guard<std::mutex> lock(g_mu);
    g_notice = "HSMMod " + latest + " is out (you have " + cur + "). Get it from GitHub: " + kRepo;
}
}  // namespace

void Start(bool enabled) {
    if (!enabled) { HSM_LOG("update: check off (update_check=0)"); return; }
    if (g_started.exchange(true)) return;
    std::thread(Check).detach();
}

std::string Notice() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_notice;
}

std::string TakeChatNotice() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_notice.empty() || g_chatShown) return {};
    g_chatShown = true;
    return g_notice;
}
}  // namespace hsm::update
