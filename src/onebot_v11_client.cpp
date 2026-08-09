#include "chatdb/onebot_v11_client.hpp"
#include "chatdb/chat_database.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include "json.hpp"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

namespace chatdb {

using json = protocol::json;

OneBotV11Client::OneBotV11Client(const Config& cfg, ChatDatabase* db)
    : cfg_(cfg), db_(db) {}

OneBotV11Client::~OneBotV11Client() { disconnect(); }

bool OneBotV11Client::connect() {
    if (connected_.load()) return true;
    should_stop_ = false;

    ws_ = std::make_unique<ix::WebSocket>();
    std::string url = fmt::format("ws://{}:{}{}", cfg_.host, cfg_.port, cfg_.path);
    ws_->setUrl(url);

    ix::WebSocketHttpHeaders headers;
    if (!cfg_.access_token.empty()) {
        headers["Authorization"] = fmt::format("Bearer {}", cfg_.access_token);
    }
    ws_->setExtraHeaders(headers);

    // 启用自动重连

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Message:
                if (msg->binary) {
                    spdlog::warn("[OneBot] Received binary message, ignored");
                } else {
                    handle_payload(msg->str);
                }
                break;
            case ix::WebSocketMessageType::Open:
                connected_ = true;
                spdlog::info("[OneBot] Connected to {}", ws_->getUrl());
                if (connect_cb_) connect_cb_();
                break;
            case ix::WebSocketMessageType::Close:
                connected_ = false;
                spdlog::info("[OneBot] Connection closed: code={}, reason={}",
                             msg->closeInfo.code, msg->closeInfo.reason);
                if (disconnect_cb_) disconnect_cb_();
                break;
            case ix::WebSocketMessageType::Error:
                connected_ = false;
                spdlog::error("[OneBot] Connection error: {}", msg->errorInfo.reason);
                if (disconnect_cb_) disconnect_cb_();
                break;
            default:
                break;
        }
    });

    ws_->start();

    // 等待连接建立（最多 5 秒）
    for (int i = 0; i < 50; ++i) {
        if (connected_.load()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return connected_.load();
}

void OneBotV11Client::disconnect() {
    should_stop_ = true;
    connected_ = false;

    if (ws_) {
        ws_->stop();
        ws_->close();
        ws_.reset();
    }

    spdlog::info("[OneBot] Disconnected");
}

// ========== OneBot API 调用 ==========
json OneBotV11Client::call_api(const std::string& action, const json& params) {
    if (!ws_ || !connected_.load()) {
        return {{"status", "failed"}, {"retcode", -1}, {"msg", "not connected"}};
    }

    json req = {{"action", action}, {"params", params}, {"echo", ++last_echo_}};
    std::string payload = req.dump();

    ws_->send(payload);
    spdlog::debug("[OneBot] API call: {} echo={}", action, last_echo_);

    // 异步：ixwebsocket 的 send 是非阻塞的，响应通过 onMessage 回调处理
    // 这里返回 echo id，调用方可以通过回调或轮询获取结果
    return {{"status", "async"}, {"echo", last_echo_}};
}

bool OneBotV11Client::send_group_msg(int64_t group_id, const std::string& message, bool auto_escape) {
    auto result = call_api("send_group_msg", {
        {"group_id", group_id},
        {"message", message},
        {"auto_escape", auto_escape}
    });
    return result.value("status", "") == "async";
}

bool OneBotV11Client::send_private_msg(int64_t qq_id, const std::string& message, bool auto_escape) {
    auto result = call_api("send_private_msg", {
        {"user_id", qq_id},
        {"message", message},
        {"auto_escape", auto_escape}
    });
    return result.value("status", "") == "async";
}

bool OneBotV11Client::delete_msg(int64_t message_id) {
    auto result = call_api("delete_msg", {{"message_id", message_id}});
    return result.value("status", "") == "async";
}

bool OneBotV11Client::set_group_ban(int64_t group_id, int64_t qq_id, int duration) {
    auto result = call_api("set_group_ban", {
        {"group_id", group_id},
        {"user_id", qq_id},
        {"duration", duration}
    });
    return result.value("status", "") == "async";
}

bool OneBotV11Client::set_group_kick(int64_t group_id, int64_t qq_id, bool reject_add_request) {
    auto result = call_api("set_group_kick", {
        {"group_id", group_id},
        {"user_id", qq_id},
        {"reject_add_request", reject_add_request}
    });
    return result.value("status", "") == "async";
}

std::string OneBotV11Client::get_group_info(int64_t group_id, bool no_cache) {
    auto result = call_api("get_group_info", {{"group_id", group_id}, {"no_cache", no_cache}});
    return result.dump();
}

std::string OneBotV11Client::get_group_member_info(int64_t group_id, int64_t qq_id, bool no_cache) {
    auto result = call_api("get_group_member_info", {
        {"group_id", group_id},
        {"user_id", qq_id},
        {"no_cache", no_cache}
    });
    return result.dump();
}

std::string OneBotV11Client::get_stranger_info(int64_t qq_id, bool no_cache) {
    auto result = call_api("get_stranger_info", {{"user_id", qq_id}, {"no_cache", no_cache}});
    return result.dump();
}

// ========== 消息处理 ==========
void OneBotV11Client::handle_payload(const std::string& payload) {
    try {
        auto j = json::parse(payload, nullptr, false);
        if (j.is_discarded()) {
            spdlog::warn("[OneBot] Invalid JSON: {}", payload.substr(0, 200));
            return;
        }

        // 处理 API 响应（有 echo 字段）
        if (j.contains("echo")) {
            spdlog::debug("[OneBot] API response: {}", payload.substr(0, 200));
            return;
        }

        std::string post_type = j.value("post_type", "");

        if (post_type == "message") {
            handle_post_message(j);
        } else if (post_type == "notice") {
            handle_post_notice(j);
        } else if (post_type == "request") {
            handle_post_request(j);
        } else if (post_type == "meta_event") {
            handle_post_meta(j);
        }
    } catch (const std::exception& e) {
        spdlog::warn("[OneBot] Payload error: {} | {}", e.what(), payload.substr(0, 200));
    }
}

void OneBotV11Client::handle_post_message(const json& j) {
    std::string msg_type = j.value("message_type", "");
    auto& msg_data = j["message"];
    int64_t message_id = j.value("message_id", 0);

    // 解析消息内容（支持 CQ 码数组和纯字符串）
    std::string content;
    if (msg_data.is_array()) {
        for (auto& seg : msg_data) {
            std::string seg_type = seg.value("type", "");
            auto data = seg.value("data", json::object());

            if (seg_type == "text") {
                content += data.value("text", "");
            } else if (seg_type == "image") {
                content += "[图片]";
            } else if (seg_type == "face") {
                content += "[表情:" + data.value("id", "") + "]";
            } else if (seg_type == "at") {
                content += "[@" + data.value("qq", "") + "]";
            } else if (seg_type == "reply") {
                content += "[回复:" + data.value("id", "") + "]";
            } else if (seg_type == "forward") {
                content += "[合并转发]";
            } else if (seg_type == "file") {
                content += "[文件:" + data.value("name", "") + "]";
            } else {
                content += "[" + seg_type + "]";
            }
        }
    } else if (msg_data.is_string()) {
        content = msg_data.get<std::string>();
    }

    if (msg_type == "group") {
        int64_t group_id = j.value("group_id", 0);
        int64_t qq_id = j.value("user_id", 0);
        std::string nickname = j.value("sender", json::object()).value("nickname", "");
        std::string card = j.value("sender", json::object()).value("card", "");
        if (!card.empty()) nickname = card;  // 优先使用群名片

        if (group_msg_cb_) group_msg_cb_(group_id, qq_id, nickname, content, message_id);
        if (db_) db_->receive_message(group_id, qq_id, nickname, content, 1);

    } else if (msg_type == "private") {
        int64_t qq_id = j.value("user_id", 0);
        std::string nickname = j.value("sender", json::object()).value("nickname", "");

        if (private_msg_cb_) private_msg_cb_(qq_id, nickname, content, message_id);
        if (db_) db_->receive_message(0, qq_id, nickname, content, 1);  // group_id=0 表示私聊
    }
}

void OneBotV11Client::handle_post_notice(const json& j) {
    std::string notice_type = j.value("notice_type", "");

    if (notice_type == "group_recall") {
        int64_t group_id = j.value("group_id", 0);
        int64_t msg_id = j.value("message_id", 0);
        int64_t operator_id = j.value("operator_id", 0);
        int64_t user_id = j.value("user_id", 0);

        spdlog::info("[OneBot] Recall: group={} msg_id={} operator={} user={}",
                     group_id, msg_id, operator_id, user_id);

        bool is_important = (operator_id != user_id);  // 管理员撤回他人消息
        if (recall_cb_) recall_cb_(group_id, msg_id, operator_id, user_id);
        if (db_) db_->handle_recall(group_id, msg_id, "", is_important);

    } else if (notice_type == "group_increase") {
        int64_t group_id = j.value("group_id", 0);
        int64_t qq_id = j.value("user_id", 0);
        std::string sub_type = j.value("sub_type", "");  // approve/invite
        if (group_increase_cb_) group_increase_cb_(group_id, qq_id, sub_type);

    } else if (notice_type == "group_decrease") {
        int64_t group_id = j.value("group_id", 0);
        int64_t qq_id = j.value("user_id", 0);
        int64_t operator_id = j.value("operator_id", 0);
        std::string sub_type = j.value("sub_type", "");  // leave/kick/kick_me
        if (group_decrease_cb_) group_decrease_cb_(group_id, qq_id, operator_id, sub_type);
    }
}

void OneBotV11Client::handle_post_request(const json& j) {
    std::string request_type = j.value("request_type", "");
    spdlog::debug("[OneBot] Request: {}", request_type);
}

void OneBotV11Client::handle_post_meta(const json& j) {
    std::string meta_type = j.value("meta_event_type", "");
    if (meta_type == "heartbeat") {
        spdlog::debug("[OneBot] Heartbeat from server");
    } else if (meta_type == "lifecycle") {
        std::string sub_type = j.value("sub_type", "");
        spdlog::info("[OneBot] Lifecycle: {}", sub_type);
    }
}

// ========== 回调注册 ==========
void OneBotV11Client::on_group_message(
    std::function<void(int64_t, int64_t, const std::string&, const std::string&, int64_t)> cb) {
    group_msg_cb_ = std::move(cb);
}

void OneBotV11Client::on_private_message(
    std::function<void(int64_t, const std::string&, const std::string&, int64_t)> cb) {
    private_msg_cb_ = std::move(cb);
}

void OneBotV11Client::on_group_recall(
    std::function<void(int64_t, int64_t, int64_t, int64_t)> cb) {
    recall_cb_ = std::move(cb);
}

void OneBotV11Client::on_group_increase(
    std::function<void(int64_t, int64_t, const std::string&)> cb) {
    group_increase_cb_ = std::move(cb);
}

void OneBotV11Client::on_group_decrease(
    std::function<void(int64_t, int64_t, int64_t, const std::string&)> cb) {
    group_decrease_cb_ = std::move(cb);
}

void OneBotV11Client::on_connect(std::function<void()> cb) {
    connect_cb_ = std::move(cb);
}

void OneBotV11Client::on_disconnect(std::function<void()> cb) {
    disconnect_cb_ = std::move(cb);
}

} // namespace chatdb
