#undef CINTERFACE

#define QOI_IMPLEMENTATION
#include "external/qoi.h"

#include "capture2x.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "api/controller.h"
#include "hooks/graphics/graphics.h"
#include "util/logging.h"

using namespace std::placeholders;

#define CAPTURE2X_FRAME_TYPE_KEYFRAME  ((uint8_t)0x00)
#define CAPTURE2X_FRAME_TYPE_DIFF      ((uint8_t)0x01)
#define CAPTURE2X_HEADER_SIZE          ((size_t)20)

namespace api::modules {

    bool CAPTURE2X_ENABLED = false;
    std::optional<uint32_t> CAPTURE2X_DIVIDE_OVERRIDE;
    std::optional<uint32_t> CAPTURE2X_FPS_OVERRIDE;

    Capture2x::Capture2x() : Module("capture2x") {
        functions["subscribe"] = std::bind(&Capture2x::handle_subscribe, this, _1, _2);
        functions["set_params"] = std::bind(&Capture2x::handle_set_params, this, _1, _2);
        functions["unsubscribe"] = std::bind(&Capture2x::handle_unsubscribe, this, _1, _2);
    }

    Capture2x::~Capture2x() {

        // stop all workers
        std::lock_guard<std::mutex> lock(ctx_m);
        for (auto &pair : contexts) {
            pair.second->running = false;
        }
        for (auto &pair : contexts) {
            if (pair.second->worker.joinable()) {
                pair.second->worker.join();
            }
        }
        contexts.clear();
    }

    /**
     * subscribe([screen=0, divide=1, fps=60])
     */
    void Capture2x::handle_subscribe(Request &req, Response &res) {

        if (!CAPTURE2X_ENABLED) {
            error(res, "capture2x module is not enabled");
            return;
        }

        auto state = req.client_state;
        if (!state) {
            error(res, "client state unavailable for capture2x");
            return;
        }

        // only WebSocket clients can subscribe
        if (!state->capture2x_send) {
            error(res, "capture2x requires a WebSocket connection");
            return;
        }

        // check if already subscribed (hold lock across check-and-create)
        {
            std::lock_guard<std::mutex> lock(ctx_m);
            if (contexts.count(state)) {
                error(res, "already subscribed");
                return;
            }
        }

        // parse optional parameters
        auto ctx = std::make_unique<ClientCtx>();
        ctx->state = state;
        ctx->screen = 0;

        if (req.params.Size() > 0 && req.params[0].IsInt()) {
            int val = req.params[0].GetInt();
            if (val >= 0) {
                ctx->screen = val;
            }
        }
        if (req.params.Size() > 1 && req.params[1].IsInt()) {
            int val = req.params[1].GetInt();
            if (val >= 1 && val <= 8) {
                ctx->divide = val;
            }
        }
        if (req.params.Size() > 2 && req.params[2].IsInt()) {
            int val = req.params[2].GetInt();
            if (val >= 1 && val <= 60) {
                ctx->fps = val;
            }
        }

        // apply server-side overrides
        if (CAPTURE2X_DIVIDE_OVERRIDE.has_value()) {
            ctx->divide = (int)CAPTURE2X_DIVIDE_OVERRIDE.value();
        }
        if (CAPTURE2X_FPS_OVERRIDE.has_value()) {
            ctx->fps = (int)CAPTURE2X_FPS_OVERRIDE.value();
        }

        // start worker
        ctx->running = true;
        ctx->worker = std::thread(capture_worker, ctx.get());

        // capture log values before moving ownership
        int log_screen = ctx->screen;
        int log_divide = ctx->divide.load();
        int log_fps = ctx->fps.load();

        // store context
        {
            std::lock_guard<std::mutex> lock(ctx_m);
            contexts[state] = std::move(ctx);
        }

        log_info("api::capture2x", "client subscribed (screen={}, divide={}, fps={})",
                log_screen, log_divide, log_fps);

        auto ok = true;
        res.add_data(ok);
    }

    /**
     * set_params([divide=-1, fps=-1])
     * Passing -1 for any parameter leaves it unchanged.
     */
    void Capture2x::handle_set_params(Request &req, Response &res) {

        auto state = req.client_state;
        if (!state) {
            error(res, "client state unavailable");
            return;
        }

        std::lock_guard<std::mutex> lock(ctx_m);
        auto it = contexts.find(state);
        if (it == contexts.end()) {
            error(res, "not subscribed");
            return;
        }

        auto &ctx = it->second;

        // divide (override takes priority)
        if (req.params.Size() > 0 && req.params[0].IsInt()) {
            int val = req.params[0].GetInt();
            if (val >= 1 && val <= 8) {
                ctx->divide = val;
            }
        }
        if (CAPTURE2X_DIVIDE_OVERRIDE.has_value()) {
            ctx->divide = (int)CAPTURE2X_DIVIDE_OVERRIDE.value();
        }

        // fps (override takes priority)
        if (req.params.Size() > 1 && req.params[1].IsInt()) {
            int val = req.params[1].GetInt();
            if (val >= 1 && val <= 60) {
                ctx->fps = val;
            }
        }
        if (CAPTURE2X_FPS_OVERRIDE.has_value()) {
            ctx->fps = (int)CAPTURE2X_FPS_OVERRIDE.value();
        }



        log_info("api::capture2x", "params updated (divide={}, fps={})",
                ctx->divide.load(), ctx->fps.load());

        auto ok = true;
        res.add_data(ok);
    }

    /**
     * unsubscribe()
     */
    void Capture2x::handle_unsubscribe(Request &req, Response &res) {

        auto state = req.client_state;
        if (!state) {
            error(res, "client state unavailable");
            return;
        }

        std::unique_ptr<ClientCtx> ctx_ptr;
        {
            std::lock_guard<std::mutex> lock(ctx_m);
            auto it = contexts.find(state);
            if (it == contexts.end()) {
                error(res, "not subscribed");
                return;
            }
            ctx_ptr = std::move(it->second);
            contexts.erase(it);
        }

        // stop worker and join
        ctx_ptr->running = false;
        if (ctx_ptr->worker.joinable()) {
            ctx_ptr->worker.join();
        }

        log_info("api::capture2x", "client unsubscribed");
        auto ok = true;
        res.add_data(ok);
    }

    static void build_frame_header(
            uint8_t frame_type,
            int divide, int fps, int compression,
            uint64_t timestamp,
            uint16_t width, uint16_t height,
            uint32_t data_size,
            uint8_t *header_out) {

        header_out[0] = frame_type;
        header_out[1] = (uint8_t)divide;
        header_out[2] = (uint8_t)fps;
        header_out[3] = (uint8_t)compression;
        memcpy(&header_out[4],  &timestamp,   8);
        memcpy(&header_out[12], &width,       2);
        memcpy(&header_out[14], &height,      2);
        memcpy(&header_out[16], &data_size,   4);
    }

    void Capture2x::capture_worker(ClientCtx *ctx) {

        ClientState *client = ctx->state;
        const int screen = ctx->screen;

        log_info("api::capture2x", "worker started (screen={}, divide={}, fps={})",
                screen, ctx->divide.load(), ctx->fps.load());

        while (ctx->running) {

            auto frame_start = std::chrono::steady_clock::now();
            int divide = ctx->divide.load();
            int fps = ctx->fps.load();

            // trigger capture and wait for raw data
            graphics_capture_trigger(screen);

            std::vector<uint8_t> raw_data;
            int raw_width = 0, raw_height = 0;
            uint64_t timestamp = 0;

            if (!graphics_capture_receive_raw(screen, raw_data,
                    &timestamp, &raw_width, &raw_height)) {
                // capture timed out or failed, retry
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (raw_data.empty() || raw_width <= 0 || raw_height <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // apply downscaling
            std::vector<uint8_t> scaled_data;
            int scaled_width = raw_width;
            int scaled_height = raw_height;

            if (divide > 1) {
                apply_downscale(raw_data, raw_width, raw_height,
                        divide, scaled_data, scaled_width, scaled_height);
            } else {
                scaled_data = std::move(raw_data);
            }

            // --- encode: QOI every frame ---
            qoi_desc desc = {};
            desc.width = (unsigned int)scaled_width;
            desc.height = (unsigned int)scaled_height;
            desc.channels = 3;
            desc.colorspace = QOI_SRGB;

            int qoi_len = 0;
            void *qoi_data = qoi_encode(scaled_data.data(), &desc, &qoi_len);

            if (!qoi_data || qoi_len <= 0) {
                // encoding failed, retry with next capture
                continue;
            }

            std::vector<uint8_t> encoded;
            encoded.assign((uint8_t *)qoi_data, (uint8_t *)qoi_data + qoi_len);
            QOI_FREE(qoi_data);

            // check callback still valid
            if (!client->capture2x_send) {
                break;
            }

            // build binary frame
            size_t total_size = CAPTURE2X_HEADER_SIZE + encoded.size();
            std::vector<uint8_t> frame(total_size);
            build_frame_header(
                    CAPTURE2X_FRAME_TYPE_KEYFRAME, divide, fps, 0,
                    timestamp,
                    (uint16_t)scaled_width, (uint16_t)scaled_height,
                    (uint32_t)encoded.size(),
                    frame.data());
            memcpy(frame.data() + CAPTURE2X_HEADER_SIZE,
                    encoded.data(), encoded.size());

            // send
            client->capture2x_send(frame.data(), frame.size());

            // frame rate limiting: sleep only for remaining budget
            auto elapsed = std::chrono::steady_clock::now() - frame_start;
            auto target_interval = std::chrono::milliseconds(1000 / (fps > 0 ? fps : 60));
            if (elapsed < target_interval) {
                std::this_thread::sleep_for(target_interval - elapsed);
            }
        }
        }

    void Capture2x::apply_downscale(
            const std::vector<uint8_t> &src,
            int src_w, int src_h,
            int divide,
            std::vector<uint8_t> &dst,
            int &out_w, int &out_h) {

        out_w = (src_w + divide - 1) / divide;
        out_h = (src_h + divide - 1) / divide;
        dst.resize((size_t)out_w * out_h * 3);

        for (int y = 0; y < out_h; y++) {
            int src_y = y * divide;
            int src_row_offset = src_y * src_w;
            int dst_row_offset = y * out_w;

            for (int x = 0; x < out_w; x++) {
                int src_x = x * divide;
                size_t src_idx = (size_t)(src_row_offset + src_x) * 3;
                size_t dst_idx = (size_t)(dst_row_offset + x) * 3;

                dst[dst_idx + 0] = src[src_idx + 0];
                dst[dst_idx + 1] = src[src_idx + 1];
                dst[dst_idx + 2] = src[src_idx + 2];
            }
        }
    }
}
