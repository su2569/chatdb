#include "chatdb/chat_database.hpp"
#include "chatdb/query_engine.hpp"
#include "chatdb/config.hpp"
#include <spdlog/spdlog.h>
#include <iostream>
#include <iomanip>
#include <csignal>
#include <thread>

using namespace chatdb;

static ChatDatabase* g_db = nullptr;
static bool g_running = true;

void signal_handler(int sig) {
    spdlog::info("Received signal {}, shutting down...", sig);
    g_running = false;
    if (g_db) g_db->shutdown();
}

void print_usage(const char* prog) {
    std::cout << R"(ChatDB v2.0 - C++ Chat Message Database with AI Memory
Usage: )" << prog << R"( [options]

Options:
  -c, --config <file>      Load config file (default: chatdb.conf)
  -s, --stats              Print statistics and exit
  -t, --test               Test all connections and exit
  -d, --demo               Run demo mode with sample data
  -v, --vacuum             Run SQLite VACUUM and exit
  --cleanup <days>         Clean up messages older than N days
  --summarize <group> <lv> Trigger manual summary (level: 3h/12h/24h/month/year/3year)
  --switch-provider <name> Switch embedding provider (ollama/openai/aliyun)
  --backup                 Backup vector index
  --restore                Restore vector index
  -h, --help               Show this help

Interactive Commands (when running):
  stats                    Show statistics
  search <query>           Hybrid search
  semantic <query>         Semantic search
  recent <group_id>        Recent messages
  context <msg_id>         Message context
  providers                List embedding providers
  switch <name>            Switch provider
  memories <group_id>      List memories
  summarize <group> <lv>   Trigger summary
  active                   Trigger active chat test
  backup                   Backup index
  help                     Show commands
  quit                     Exit

Environment Variables:
  CHATDB_SQLITE_PATH, CHATDB_REDIS_HOST, CHATDB_REDIS_PORT
  CHATDB_OLLAMA_HOST, CHATDB_OLLAMA_PORT, CHATDB_OLLAMA_MODEL
  CHATDB_OPENAI_KEY, CHATDB_ALIYUN_KEY
  CHATDB_TCP_PORT, CHATDB_WS_HOST, CHATDB_WS_PORT
)";
}

int main(int argc, char* argv[]) {
    std::string config_file = "chatdb.conf";
    bool show_stats = false;
    bool test_only = false;
    bool demo_mode = false;
    bool do_vacuum = false;
    bool do_backup = false;
    bool do_restore = false;
    int cleanup_days = -1;
    std::string switch_provider_name;
    std::string summarize_group;
    std::string summarize_level;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) config_file = argv[++i];
        else if (arg == "-s" || arg == "--stats") show_stats = true;
        else if (arg == "-t" || arg == "--test") test_only = true;
        else if (arg == "-d" || arg == "--demo") demo_mode = true;
        else if (arg == "-v" || arg == "--vacuum") do_vacuum = true;
        else if (arg == "--cleanup" && i + 1 < argc) cleanup_days = std::stoi(argv[++i]);
        else if (arg == "--switch-provider" && i + 1 < argc) switch_provider_name = argv[++i];
        else if (arg == "--summarize" && i + 2 < argc) { summarize_group = argv[++i]; summarize_level = argv[++i]; }
        else if (arg == "--backup") do_backup = true;
        else if (arg == "--restore") do_restore = true;
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
    }

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] %v");

    auto& cfg_mgr = ConfigManager::instance();
    cfg_mgr.load_defaults();
    cfg_mgr.load_from_env();
    if (!config_file.empty()) {
        if (cfg_mgr.load_from_file(config_file)) spdlog::info("Loaded config: {}", config_file);
        else spdlog::info("Config not found ({}), using defaults", config_file);
    }
    auto& cfg = cfg_mgr.config();

    if (test_only) {
        std::cout << "\n========== ChatDB v2.0 Connection Test ==========\n";
        std::cout << "SQLite:    " << cfg.sqlite_path << "\n";
        std::cout << "Redis:     " << cfg.redis_host << ":" << cfg.redis_port << "\n";
        std::cout << "Ollama:    " << cfg.ollama_host << ":" << cfg.ollama_port 
                  << " (" << cfg.ollama_model << ", timeout=" << cfg.ollama_timeout_ms << "ms)\n";
        std::cout << "TCP:       port " << cfg.tcp_port << "\n";
        std::cout << "WS:        " << cfg.ws_host << ":" << cfg.ws_port << cfg.ws_path << "\n";
        std::cout << "Providers: ollama" 
                  << (cfg.openai_api_key.empty() ? "" : ", openai")
                  << (cfg.aliyun_api_key.empty() ? "" : ", aliyun") << "\n";

        ChatDatabase db;
        if (db.initialize(cfg)) {
            std::cout << "\n[OK] All systems operational\n";
            std::cout << db.get_stats() << "\n";
            db.shutdown();
        } else {
            std::cout << "\n[FAIL] Initialization error\n";
        }
        std::cout << "=================================================\n\n";
        return 0;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    ChatDatabase db;
    g_db = &db;

    if (!db.initialize(cfg)) {
        spdlog::error("Initialization failed");
        return 1;
    }

    if (show_stats) { std::cout << "\n" << db.get_stats() << "\n\n"; return 0; }
    if (do_vacuum) { db.vacuum(); std::cout << "VACUUM done.\n"; return 0; }
    if (cleanup_days >= 0) { int n = db.cleanup_old_data(cleanup_days); std::cout << "Cleaned " << n << " items.\n"; return 0; }
    if (!switch_provider_name.empty()) { db.switch_provider(switch_provider_name); std::cout << "Switched.\n"; return 0; }
    if (!summarize_group.empty()) { db.summarize_now(std::stoll(summarize_group), summarize_level); std::cout << "Summarized.\n"; return 0; }
    if (do_backup) { db.backup_index(); std::cout << "Backup done.\n"; return 0; }
    if (do_restore) { db.restore_index(); std::cout << "Restore done.\n"; return 0; }

    // 回调
    db.on_message_processed([](const ProcessedMessage& msg) {
        if (!msg.is_duplicate) spdlog::debug("Processed #{} group={}", msg.id, msg.group_id);
    });
    db.on_error([](const std::string& err) { spdlog::error("DB error: {}", err); });
    db.on_active_chat([](const ActiveChatRequest& req) {
        spdlog::info("Active chat triggered: group={} topic='{}' urgency={}", 
                     req.group_id, req.topic, req.urgency);
    });

    // Demo
    if (demo_mode) {
        spdlog::info("=== Demo Mode ===");
        std::vector<RawMessage> demo = {
            {10001, 20001, "Alice", "大家好，有人知道怎么装显卡吗？", 1, 0},
            {10001, 20002, "Bob", "显卡？你是什么主板？", 1, 0},
            {10001, 20001, "Alice", "B650M 迫击炮", 1, 0},
            {10001, 20003, "Carol", "这个板子支持 PCIe 4.0，随便装", 1, 0},
            {10001, 20002, "Bob", "记得先断电，防静电", 1, 0},
            {10001, 20001, "Alice", "好的谢谢，我试试", 1, 0},
            {10002, 30001, "Dave", "今天天气真不错，适合出门", 1, 0},
            {10002, 30002, "Eve", "是啊，要不要去公园？", 1, 0},
            {10002, 30001, "Dave", "走，带上相机", 1, 0},
            {10003, 40001, "Frank", "求推荐一款机械键盘", 1, 0},
            {10003, 40002, "Grace", "RK98 性价比很高", 1, 0},
            {10003, 40001, "Frank", "有线的还是三模的？", 1, 0},
        };
        db.receive_messages(demo);
        std::this_thread::sleep_for(std::chrono::seconds(3));

        std::cout << "\n========== Query Demo ==========\n";
        std::cout << "\n--- Fulltext: '显卡' ---\n";
        for (auto& r : db.search_fulltext("显卡", 10001, 10))
            std::cout << "[" << r.relevance_score << "] " << r.nickname << ": " << r.content << "\n";

        std::cout << "\n--- Semantic: '电脑硬件' ---\n";
        for (auto& r : db.search_semantic("电脑硬件", 10001, 5))
            std::cout << "[" << r.relevance_score << "] " << r.nickname << ": " << r.content << "\n";

        std::cout << "\n--- Hybrid: '键盘推荐' ---\n";
        for (auto& r : db.search_hybrid("键盘推荐", 10003, 5))
            std::cout << "[" << r.relevance_score << "] " << r.nickname << ": " << r.content << "\n";

        std::cout << "\n--- Stats ---\n" << db.get_stats() << "\n";
        std::cout << "================================\n\n";
    }

    spdlog::info("ChatDB v2.0 running. TCP port {}. Type 'help' for commands.", cfg.tcp_port);

    std::string line;
    while (g_running) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        auto parts = [](const std::string& s) {
            std::vector<std::string> r;
            std::stringstream ss(s);
            std::string t;
            while (ss >> t) r.push_back(t);
            return r;
        }(line);
        if (parts.empty()) continue;

        const std::string& cmd = parts[0];
        try {
            if (cmd == "quit" || cmd == "exit") break;
            else if (cmd == "stats") std::cout << db.get_stats() << "\n";
            else if (cmd == "search" && parts.size() > 1) {
                auto q = line.substr(7);
                for (auto& r : db.search_hybrid(q, 0, 10))
                    std::cout << "  [" << r.relevance_score << "] " << r.nickname 
                              << " (g" << r.group_id << "): " << r.content << "\n";
            }
            else if (cmd == "semantic" && parts.size() > 1) {
                auto q = line.substr(9);
                for (auto& r : db.search_semantic(q, 0, 10))
                    std::cout << "  [" << r.relevance_score << "] " << r.nickname 
                              << ": " << r.content << "\n";
            }
            else if (cmd == "recent" && parts.size() > 1) {
                for (auto& r : db.get_recent(std::stoll(parts[1]), 10))
                    std::cout << "  " << r.nickname << ": " << r.content << "\n";
            }
            else if (cmd == "context" && parts.size() > 1) {
                int64_t msg_id = std::stoll(parts[1]);
                int radius = (parts.size() > 2) ? std::stoi(parts[2]) : 5;
                auto results = db.query()->get_context(msg_id, radius);
                std::cout << "Context for msg " << msg_id << " (radius=" << radius << "):\n";
                for (const auto& r : results) {
                    std::time_t t = r.timestamp;
                    auto tm = *std::localtime(&t);
                    std::cout << std::put_time(&tm, "%H:%M:%S")
                              << " [" << r.nickname << "] " << r.content << "\n";
                }
            }
            else if (cmd == "providers") {
                for (auto& n : db.list_providers()) std::cout << "  " << n << "\n";
            }
            else if (cmd == "switch" && parts.size() > 1) {
                db.switch_provider(parts[1]);
                std::cout << "Switched to " << parts[1] << "\n";
            }
            else if (cmd == "memories" && parts.size() > 1) {
                for (auto& m : db.get_memories(std::stoll(parts[1]), "", 10))
                    std::cout << "  [" << m.level << "] " << m.summary << "\n";
            }
            else if (cmd == "summarize" && parts.size() > 2) {
                db.summarize_now(std::stoll(parts[1]), parts[2]);
                std::cout << "Summary queued.\n";
            }
            else if (cmd == "active") {
                std::cout << "Active chat test (see logs)\n";
            }
            else if (cmd == "backup") {
                db.backup_index();
                std::cout << "Backup done.\n";
            }
            else if (cmd == "help") {
                std::cout << "Commands: stats, search, semantic, recent, context, providers, switch,\n"
                          << "          memories, summarize, active, backup, help, quit\n";
            }
            else std::cout << "Unknown. Type 'help'.\n";
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    db.shutdown();
    spdlog::info("ChatDB exited.");
    return 0;
}
