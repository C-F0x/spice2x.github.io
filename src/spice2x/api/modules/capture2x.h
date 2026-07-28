#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdint>

#include "api/module.h"

namespace api {
    struct ClientState;
}

namespace api::modules {

    class Capture2x : public Module {
    public:
        Capture2x();
        ~Capture2x() override;

    private:

        struct ClientCtx {
            api::ClientState *state = nullptr;
            int screen = 0;
            std::atomic<int> divide{1};
            std::atomic<int> fps{60};
            std::atomic<int> keyframe_count{10};
            std::atomic<bool> running{false};
            int frame_counter = 0;
            std::thread worker;
            std::vector<uint8_t> prev_frame;
            int prev_width = 0;
            int prev_height = 0;
        };

        std::mutex ctx_m;
        std::unordered_map<api::ClientState*, std::unique_ptr<ClientCtx>> contexts;

        void handle_subscribe(Request &req, Response &res);
        void handle_set_params(Request &req, Response &res);
        void handle_unsubscribe(Request &req, Response &res);

        static void capture_worker(ClientCtx *ctx);
        // simple nearest-neighbor downscale; adequate for game UI.
        // a bilinear/bicubic filter would improve quality at non-integer divide ratios.
        static void apply_downscale(
                const std::vector<uint8_t> &src,
                int src_w, int src_h,
                int divide,
                std::vector<uint8_t> &dst,
                int &out_w, int &out_h);
    };

    // global switch & overrides
    extern bool CAPTURE2X_ENABLED;
    extern std::optional<uint32_t> CAPTURE2X_DIVIDE_OVERRIDE;
    extern std::optional<uint32_t> CAPTURE2X_FPS_OVERRIDE;
}
