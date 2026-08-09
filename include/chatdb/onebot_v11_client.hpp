#pragma once
#include "chatdb/protocol.hpp"
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <ixwebsocket/IXWebSocket.h>

namespace chatdb {

class ChatDatabase;

// OneBot v11 WebSocket 客户端（连接 go-cqhttp / AstrBot / NoneBot）
class OneBotV11Client {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 3001;           // go-cqhttp / AstrBot 默认正向 WS 端口
        std::string path = "/";    // WS 路径
        std::string access_token;  // 鉴权 token（OneBot 的 access_token）
        int reconnect_interval_ms = 5000;
        int heartbeat_interval_ms = 30000;
        int max_reconnect_attempts = 0;  // 0 = 无限重连
    };

    OneBotV11Client(const Config& cfg, ChatDatabase* db);
    ~OneBotV11Client();

    OneBotV11Client(const OneBotV11Client&) = delete;
    OneBotV11Client& operator=(const OneBotV11Client&) = delete;

    bool connect();
    void disconnect();
    bool is_connected() const { return connected_.load(); }

    // OneBot API 调用
    bool send_group_msg(int64_t group_id, const std::string& message, bool auto_escape = false);
    bool send_private_msg(int64_t qq_id, const std::string& message, bool auto_escape = false);
    bool delete_msg(int64_t message_id);
    bool set_group_ban(int64_t group_id, int64_t qq_id, int duration);
    bool set_group_kick(int64_t group_id, int64_t qq_id, bool reject_add_request = false);

    // 获取信息
    std::string get_group_info(int64_t group_id, bool no_cache = false);
    std::string get_group_member_info(int64_t group_id, int64_t qq_id, bool no_cache = false);
    std::string get_stranger_info(int64_t qq_id, bool no_cache = false);

    // 事件回调
    void on_group_message(std::function<void(int64_t group_id, int64_t qq_id, 
                                              const std::string& nickname,
                                              const std::string& content,
                                              int64_t message_id)> cb);
    void on_private_message(std::function<void(int64_t qq_id, 
                                               const std::string& nickname,
                                               const std::string& content,
                                               int64_t message_id)> cb);
    void on_group_recall(std::function<void(int64_t group_id, int64_t msg_id,
                                            int64_t operator_id, int64_t user_id)> cb);
    void on_group_increase(std::function<void(int64_t group_id, int64_t qq_id,
                                              const std::string& sub_type)> cb);
    void on_group_decrease(std::function<void(int64_t group_id, int64_t qq_id,
                                              int64_t operator_id,
                                              const std::string& sub_type)> cb);
    void on_connect(std::function<void()> cb);
    void on_disconnect(std::function<void()> cb);

private:
    void handle_payload(const std::string& payload);
    void handle_post_message(const protocol::json& j);
    void handle_post_notice(const protocol::json& j);
    void handle_post_request(const protocol::json& j);
    void handle_post_meta(const protocol::json& j);

    protocol::json call_api(const std::string& action, const protocol::json& params);

    Config cfg_;
    ChatDatabase* db_;
    std::unique_ptr<ix::WebSocket> ws_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> should_stop_{false};

    std::function<void(int64_t, int64_t, const std::string&, const std::string&, int64_t)> group_msg_cb_;
    std::function<void(int64_t, const std::string&, const std::string&, int64_t)> private_msg_cb_;
    std::function<void(int64_t, int64_t, int64_t, int64_t)> recall_cb_;
    std::function<void(int64_t, int64_t, const std::string&)> group_increase_cb_;
    std::function<void(int64_t, int64_t, int64_t, const std::string&)> group_decrease_cb_;
    std::function<void()> connect_cb_;
    std::function<void()> disconnect_cb_;

    int64_t last_echo_ = 0;
};

} // namespace chatdb
