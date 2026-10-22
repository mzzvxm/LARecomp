//
// larecomp_identity_tool: the player side of the nick protocol from the command
// line, one step per call (for poking a master by hand).
//
//   keygen                              -> seed=<hex> pubkey=<hex> xuid=<hex>
//   request <seed> <op> <nick> [code]   -> the POST body for the master
//   proof <seed> <nonce> <relay> <ticket>
//                                       -> sig=<hex> over ProofMessage
//   check <master_pub> <ticket>         -> ok / bad (and the nick)
//

#include <cstdio>
#include <cstring>
#include <string>

#include "../protocol/identity_protocol.h"

namespace id = larecomp::identity;

namespace {

bool Seed(const char* hex, id::KeyPair& kp) {
    uint8_t seed[32];
    if (!id::FromHex(hex, seed, 32)) return false;
    kp = id::KeyPairFromSeed(seed);
    return true;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: larecomp_identity_tool keygen | request <seed> <op> <nick> [code] |\n"
                 "       proof <seed> <nonce> <relay> <ticket> | check <master_pub> <ticket>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::string cmd = argv[1];
    if (cmd == "keygen") {
        uint8_t seed[32];
        id::RandomBytes(seed, 32);
        const id::KeyPair kp = id::KeyPairFromSeed(seed);
        std::printf("seed=%s\npubkey=%s\nxuid=%016llX\n", id::ToHex(seed, 32).c_str(),
                    id::ToHex(kp.pub, 32).c_str(),
                    static_cast<unsigned long long>(id::XuidFromPublicKey(kp.pub)));
        return 0;
    }
    if (cmd == "request" && (argc == 5 || argc == 6)) {
        id::KeyPair kp;
        if (!Seed(argv[2], kp)) return Usage();
        const std::string op = argv[3], nick = argv[4];
        const std::string code = argc == 6 ? argv[5] : "";
        const std::string pub = id::ToHex(kp.pub, 32);
        const int64_t now = id::NowUnix();
        const std::string salt = id::NewSalt();
        uint8_t sig[64];
        id::Sign(sig, kp,
                 id::RequestMessage(op, nick, pub, now, salt, op == "recover" ? id::CanonicalCode(code) : ""));
        std::printf("op=%s\nnick=%s\npubkey=%s\ntime=%lld\nsalt=%s\nsig=%s\n", op.c_str(), nick.c_str(),
                    pub.c_str(), static_cast<long long>(now), salt.c_str(), id::ToHex(sig, 64).c_str());
        if (!code.empty()) std::printf("code=%s\n", code.c_str());
        return 0;
    }
    if (cmd == "proof" && argc == 6) {
        id::KeyPair kp;
        uint8_t nonce[16];
        id::Ticket t;
        if (!Seed(argv[2], kp) || !id::FromHex(argv[3], nonce, 16) ||
            !id::FromHex(argv[5], reinterpret_cast<uint8_t*>(&t), sizeof(t)))
            return Usage();
        char relay[64] = {};
        std::snprintf(relay, sizeof(relay), "%s", argv[4]);
        uint8_t sig[64];
        id::Sign(sig, kp, id::ProofMessage(nonce, relay, t));
        std::printf("sig=%s\n", id::ToHex(sig, 64).c_str());
        return 0;
    }
    if (cmd == "check" && argc == 4) {
        uint8_t pub[32];
        id::Ticket t;
        if (!id::FromHex(argv[2], pub, 32) || !id::FromHex(argv[3], reinterpret_cast<uint8_t*>(&t), sizeof(t)))
            return Usage();
        const bool ok = id::CheckTicket(pub, t, id::NowUnix());
        std::printf("%s nick=%s\n", ok ? "ok" : "bad", id::TicketNick(t).c_str());
        return ok ? 0 : 1;
    }
    return Usage();
}
