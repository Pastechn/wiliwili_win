//
// Created by maye174 on 2023/4/6.
//

#include "live/danmaku_live.hpp"
#include "bilibili/util/http.hpp"
#include "live/ws_utils.hpp"
#include "utils/config_helper.hpp"

#include <cstddef>
#include <ctime>
#include <iostream>
#include <queue>
#include <condition_variable>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#endif

#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::string url = "ws://broadcastlv.chat.bilibili.com:2244/sub";
static std::string key = "";

static void get_live_s(int room_id) {
    auto res = bilibili::HTTP::get(
        "https://api.live.bilibili.com/xlive/web-room/v1/index/"
        "getDanmuInfo?type=0&id=" +
        std::to_string(room_id));

    if (res.status_code != 200) {
        brls::Logger::error("getDanmuInfo error:{}", res.status_code);
    } else {
        json _json;
        try {
            _json = json::parse(res.text);
        } catch (const std::exception &e) {
            std::cout << "getDanmuInfo json parse error" << std::endl;
        }
        if (_json["code"].get<int>() == 0) {
            // url = "ws://" +
            //       _json["data"]["host_list"][0]["host"]
            //           .get_ref<const std::string &>() +
            //       ":" +
            //       std::to_string(
            //           _json["data"]["host_list"][0]["ws_port"].get<int>()) +
            //       "/sub";
            key = _json["data"]["token"].get_ref<const std::string &>();
        }
    }
}

static void mongoose_event_handler(struct mg_connection *nc, int ev,
                                   void *ev_data, void *user_data);

static void heartbeat_timer(void *param) {
    auto liveDanmaku = static_cast<LiveDanmaku *>(param);
    if (liveDanmaku->is_connected() and liveDanmaku->is_evOK()) {
        liveDanmaku->send_heartbeat();
    }
}

typedef struct task {
    // 函数
    const std::function<void(std::string &&)> onMessage;
    // 参数
    std::string arg;
    // 优先级，暂时都设置0
    int priority;
} task;

static std::queue<task> task_q;
static std::condition_variable cv;
static std::mutex task_mutex;

static void add_task(const std::function<void(std::string &&)> &func,
                     std::string &&a) {
    std::lock_guard<std::mutex> lock(task_mutex);
    task_q.emplace(task{func, a, 0});
    cv.notify_one();
}

LiveDanmaku::LiveDanmaku() {
#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        printf("WSAStartup failed with error: %d\n", result);
    }
#endif
}

LiveDanmaku::~LiveDanmaku() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

void LiveDanmaku::connect(int room_id, int64_t uid) {
    if (connected.load(std::memory_order_acquire)) {
        return;
    }
    connected.store(true, std::memory_order_release);

    // Create and configure Mongoose connection
    this->mgr = new mg_mgr;
    if (this->mgr == nullptr) {
        disconnect();
        return;
    }

    get_live_s(room_id);

    mg_log_set(MG_LL_NONE);
    mg_mgr_init(this->mgr);

    this->nc = mg_ws_connect(this->mgr, url.c_str(), mongoose_event_handler,
                             this, nullptr);

    if (this->nc == nullptr) {
        std::cout << "nc is null" << std::endl;
        disconnect();
        mg_mgr_free(this->mgr);
        delete this->mgr;
        return;
    }

    this->room_id = room_id;
    this->uid     = uid;
    //mg_mgr_poll(this->mgr, 10);

    // Start Mongoose event loop and heartbeat thread
    mongoose_thread = std::thread([this]() {
        while (this->is_connected()) {
            this->mongoose_mutex.lock();
            if (this->nc == nullptr) {
                break;
            }
            this->mongoose_mutex.unlock();
            mg_mgr_poll(this->mgr, this->wait_time);
        }
        mg_mgr_free(this->mgr);
        delete this->mgr;
    });

    task_thread = std::thread([this]() {
        while (true) {
            std::unique_lock<std::mutex> lock(task_mutex);
            cv.wait(lock, [this] {
                return !task_q.empty() or !this->is_connected();
            });
            if (!this->is_connected()) break;
            auto task = task_q.front();
            task_q.pop();
            lock.unlock();

            task.onMessage(std::move(task.arg));
        }
    });

    brls::Logger::info("(LiveDanmaku) connect step finish");
}

void LiveDanmaku::disconnect() {
    if (!connected.load(std::memory_order_acquire)) {
        return;
    }

    // Stop Mongoose event loop thread
    connected.store(false, std::memory_order_release);

    // Stop Mongoose event loop thread
    if (mongoose_thread.joinable()) {
        mongoose_thread.join();
    }

    cv.notify_one();

    if (task_thread.joinable()) {
        task_thread.join();
    }
    brls::Logger::info("(LiveDanmaku) close step finish");
}

void LiveDanmaku::set_wait_time(size_t time) { wait_time = time; }

bool LiveDanmaku::is_connected() {
    return connected.load(std::memory_order_acquire);
}

bool LiveDanmaku::is_evOK() { return ms_ev_ok.load(std::memory_order_acquire); }

void LiveDanmaku::send_join_request(const int room_id, const int64_t uid) {
    json join_request = {
        {"uid", uid},        {"roomid", room_id},
        {"protover", 2},     {"buvid", ProgramConfig::instance().getBuvid3()},
        {"platform", "web"}, {"type", 2},
        {"key", key}};
    std::string join_request_str = join_request.dump();
    brls::Logger::info("(LiveDanmaku) join_request:{}", join_request_str);
    std::vector<uint8_t> packet = encode_packet(0, 7, join_request_str);
    std::string packet_str(packet.begin(), packet.end());
    mongoose_mutex.lock();
    if (this->nc == nullptr) return;
    mg_ws_send(this->nc, packet_str.data(), packet_str.size(),
               WEBSOCKET_OP_BINARY);
    mongoose_mutex.unlock();
}

void LiveDanmaku::send_heartbeat() {
    brls::Logger::debug("(LiveDanmaku) send_heartbeat");
    std::vector<uint8_t> packet = encode_packet(0, 2, "");
    std::string packet_str(packet.begin(), packet.end());
    mongoose_mutex.lock();
    if (this->nc == nullptr) return;
    mg_ws_send(this->nc, packet_str.data(), packet_str.size(),
               WEBSOCKET_OP_BINARY);
    mongoose_mutex.unlock();
}

void LiveDanmaku::send_text_message(const std::string &message) {
    //暂时不用
}

static void mongoose_event_handler(struct mg_connection *nc, int ev,
                                   void *ev_data, void *user_data) {
    LiveDanmaku *liveDanmaku = static_cast<LiveDanmaku *>(user_data);
    liveDanmaku->ms_ev_ok.store(true, std::memory_order_release);
    if (ev == MG_EV_OPEN) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
#ifdef MONGOOSE_HEX_DUMPS
        nc->is_hexdumping = 1;
#else
        nc->is_hexdumping = 0;
#endif
    } else if (ev == MG_EV_ERROR) {
        MG_ERROR(("%p %s", nc->fd, (char *)ev_data));
        liveDanmaku->ms_ev_ok.store(false, std::memory_order_release);
    } else if (ev == MG_EV_WS_OPEN) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
        liveDanmaku->send_join_request(liveDanmaku->room_id, liveDanmaku->uid);
        mg_timer_add(liveDanmaku->mgr, 30000, MG_TIMER_REPEAT, heartbeat_timer,
                     user_data);
    } else if (ev == MG_EV_WS_MSG) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        add_task(liveDanmaku->onMessage,
                 std::string(wm->data.ptr, wm->data.len));
    } else if (ev == MG_EV_CLOSE) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
        liveDanmaku->ms_ev_ok.store(false, std::memory_order_release);
    }
}

void LiveDanmaku::setonMessage(std::function<void(std::string &&)> func) {
    onMessage = func;
}