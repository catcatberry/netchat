#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstring>  // 新增：for std::strlen

namespace net  = boost::asio;
namespace http = boost::beast::http;
namespace beast= boost::beast;
namespace ws   = boost::beast::websocket;
using tcp = net::ip::tcp;

static std::string g_static_dir = "web";
static std::string g_index_path = "web/index.html";

/* ----------- 简易工具 ----------- */
std::string load_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "";
    std::ostringstream oss; oss << ifs.rdbuf();
    return oss.str();
}

std::string url_decode(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i=0;i<s.size();++i) {
        if (s[i]=='%' && i+2<s.size()) {
            std::string hex = s.substr(i+1,2);
            char c = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            out.push_back(c);
            i += 2;
        } else if (s[i]=='+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_query(std::string target) {
    std::unordered_map<std::string, std::string> q;
    auto pos = target.find('?');
    if (pos==std::string::npos) return q;
    auto qs = target.substr(pos+1);
    std::vector<std::string> parts;
    boost::split(parts, qs, boost::is_any_of("&"));
    for (auto &kv: parts) {
        auto eq = kv.find('=');
        if (eq!=std::string::npos) {
            auto k = kv.substr(0,eq);
            auto v = kv.substr(eq+1);
            q[k]=url_decode(v);
        }
    }
    return q;
}

std::string guess_mime_type(std::string const& path) {
    auto ends_with = [](const std::string& s, const char* suf) {
        const size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };

    if (ends_with(path, ".htm")  || ends_with(path, ".html")) return "text/html";
    if (ends_with(path, ".css"))  return "text/css";
    if (ends_with(path, ".js"))   return "application/javascript";
    if (ends_with(path, ".png"))  return "image/png";
    if (ends_with(path, ".jpg")   || ends_with(path, ".jpeg")) return "image/jpeg";
    if (ends_with(path, ".gif"))  return "image/gif";
    if (ends_with(path, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

/* ----------- 房间与会话 ----------- */
struct ChatSession;
struct ChatState {
    std::mutex mu;
    // room -> sessions（用裸指针登记，实际用shared_ptr持有）
    std::unordered_map<std::string, std::unordered_set<ChatSession*>> rooms;

    void join(const std::string& room, ChatSession* s) {
        std::lock_guard<std::mutex> lk(mu);
        rooms[room].insert(s);
    }
    void leave(const std::string& room, ChatSession* s) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = rooms.find(room);
        if (it!=rooms.end()) {
            it->second.erase(s);
            if (it->second.empty()) rooms.erase(it);
        }
    }
    template<class Fn>
    void for_each_in(const std::string& room, Fn fn) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = rooms.find(room);
        if (it!=rooms.end()) {
            for (auto* s: it->second) fn(s);
        }
    }
};

struct ChatSession : public std::enable_shared_from_this<ChatSession> {
    ws::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    
    
    net::strand<net::io_context::executor_type> strand_;
    
    ChatState& state_;
    std::string room_;
    std::string name_;
    std::deque<std::string> send_q_;

    ChatSession(tcp::socket&& socket, ChatState& state, net::io_context& ioc)
    : ws_(std::move(socket))
    , strand_(net::make_strand(ioc))
    , state_(state) {}

    void run(http::request<http::string_body> req) {
        // 解析 room/name
        auto target = std::string(req.target());
        auto q = parse_query(target);
        room_ = q.count("room")? q["room"] : "lobby";
        name_ = q.count("name")? q["name"] : "Guest";

        ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(ws::stream_base::decorator([](ws::response_type& res){
            res.set(http::field::server, "netchat/1.0");
        }));

        // 升级
        ws_.async_accept(req,
            net::bind_executor(strand_,
                std::bind(&ChatSession::on_accept, shared_from_this(), std::placeholders::_1)));
    }

    void on_accept(beast::error_code ec) {
        if (ec) return fail(ec, "accept");
        state_.join(room_, this);
        // 进入房间广播加入
        broadcast_system(name_ + " 加入了房间");

        do_read();
    }

    void do_read() {
        ws_.async_read(buffer_,
            net::bind_executor(strand_,
                std::bind(&ChatSession::on_read, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
    }

    void on_read(beast::error_code ec, std::size_t) {
        if (ec == ws::error::closed) {
            on_close();
            return;
        }
        if (ec) {
            fail(ec, "read");
            on_close();
            return;
        }

        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        // 广播到房间
        broadcast(name_ + ": " + msg);

        do_read();
    }

    void send_text(std::string text) {
        net::post(strand_, [self=shared_from_this(), t=std::move(text)](){
            self->send_q_.push_back(t);
            if (self->send_q_.size()==1) self->do_write();
        });
    }

    void do_write() {
        ws_.text(true);
        ws_.async_write(net::buffer(send_q_.front()),
            net::bind_executor(strand_,
                std::bind(&ChatSession::on_write, shared_from_this(), std::placeholders::_1, std::placeholders::_2)));
    }

    void on_write(beast::error_code ec, std::size_t) {
        if (ec) {
            fail(ec, "write");
            on_close();
            return;
        }
        send_q_.pop_front();
        if (!send_q_.empty()) do_write();
    }

    void broadcast(const std::string& t) {
        state_.for_each_in(room_, [&](ChatSession* s){ s->send_text(t); });
    }
    void broadcast_system(const std::string& t) {
        state_.for_each_in(room_, [&](ChatSession* s){ s->send_text("[系统] " + t); });
    }

    void on_close() {
        state_.leave(room_, this);
        broadcast_system(name_ + " 离开了房间");
    }

    static void fail(beast::error_code ec, char const* what) {
        if (ec == net::error::operation_aborted) return;
        std::cerr << what << ": " << ec.message() << "\n";
    }
};

/* ----------- HTTP 会话（可升级为 WS） ----------- */
class HttpSession : public std::enable_shared_from_this<HttpSession> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    ChatState& state_;
    std::string doc_root_;
    http::request<http::string_body> req_;
    net::io_context& ioc_;

public:
    HttpSession(tcp::socket&& socket, ChatState& state, std::string doc_root, net::io_context& ioc)
    : stream_(std::move(socket)), state_(state), doc_root_(std::move(doc_root)), ioc_(ioc) {}

    void run() { do_read(); }

    void do_read() {
        req_ = {};
        http::async_read(stream_, buffer_, req_,
            std::bind(&HttpSession::on_read, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    void on_read(beast::error_code ec, std::size_t) {
        if (ec == http::error::end_of_stream) return do_close();
        if (ec) { std::cerr << "http read: " << ec.message() << "\n"; return; }

        if (ws::is_upgrade(req_)) {
            // 升级为 WebSocket
            auto s = std::make_shared<ChatSession>(stream_.release_socket(), state_, ioc_);
            s->run(std::move(req_));
            return;
        }

        handle_request();
    }

    void handle_request() {
        // 地址路由：/ 或 /room/<id> 都返回 index.html
        std::string target = std::string(req_.target());
        if (req_.method() != http::verb::get) {
            send(make_string_response(http::status::bad_request, "Method Not Allowed\n"));
            return;
        }

        if (target == "/" || target.rfind("/room/", 0)==0) {
            auto body = load_file(g_index_path);
            if (body.empty()) body = "<h1>index.html 未找到</h1>";
            send(make_string_response(http::status::ok, body, "text/html"));
            return;
        }

        // 简单静态文件
        std::string path = doc_root_ + target;
        auto body = load_file(path);
        if (body.empty()) {
            send(make_string_response(http::status::not_found, "404 Not Found\n"));
            return;
        }
        send(make_string_response(http::status::ok, body, guess_mime_type(path)));
    }

    http::response<http::string_body> make_string_response(http::status st, std::string body, std::string mime="text/plain") {
        http::response<http::string_body> res{st, req_.version()};
        res.set(http::field::server, "netchat/1.0");
        res.set(http::field::content_type, mime);
        res.keep_alive(req_.keep_alive());
        res.body() = std::move(body);
        res.prepare_payload();
        return res;
    }

    template <class Body>
    void send(http::response<Body>&& res) {
        auto sp = std::make_shared<http::response<Body>>(std::move(res));
        http::async_write(stream_, *sp,
            [self=shared_from_this(), sp](beast::error_code ec, std::size_t){
                if (ec) { std::cerr << "http write: " << ec.message() << "\n"; }
                if (!sp->keep_alive()) self->do_close();
                else self->do_read();
            });
    }

    void do_close() {
        beast::error_code ec;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
};

/* ----------- 监听器 ----------- */
class Listener : public std::enable_shared_from_this<Listener> {
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    ChatState& state_;
    std::string doc_root_;
public:
    Listener(net::io_context& ioc, tcp::endpoint ep, ChatState& state, std::string doc_root)
    : ioc_(ioc), acceptor_(net::make_strand(ioc)), state_(state), doc_root_(std::move(doc_root)) {
        beast::error_code ec;
        acceptor_.open(ep.protocol(), ec);
        if (ec) throw beast::system_error(ec);
        acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind(ep, ec);
        if (ec) throw beast::system_error(ec);
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) throw beast::system_error(ec);
    }
    void run() { do_accept(); }

    void do_accept() {
        acceptor_.async_accept(
            net::make_strand(ioc_),
            std::bind(&Listener::on_accept, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }
    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<HttpSession>(std::move(socket), state_, doc_root_, ioc_)->run();
        }
        do_accept();
    }
};

int main(int argc, char* argv[]) {
    // 简易参数：netchat [port] [static_dir]
    unsigned short port = 8080;
    if (argc >= 2) port = static_cast<unsigned short>(std::atoi(argv[1]));
    if (argc >= 3) { g_static_dir = argv[2]; g_index_path = g_static_dir + "/index.html"; }

    try {
        net::io_context ioc{std::max(1u, std::thread::hardware_concurrency())};
        ChatState state;
        auto addr = net::ip::make_address("0.0.0.0");
        auto ep = tcp::endpoint{addr, port};
        std::make_shared<Listener>(ioc, ep, state, g_static_dir)->run();
        std::cout << "netchat listening on http://0.0.0.0:" << port << "\n";
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
