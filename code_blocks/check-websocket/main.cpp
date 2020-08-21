#include <iostream>
#include "client_wss.hpp"
#include <openssl/ssl.h>
#include <wincrypt.h>
#include <xtime.hpp>
#include <nlohmann/json.hpp>

using namespace std;
using WssClient = SimpleWeb::SocketClient<SimpleWeb::WSS>;
using json = nlohmann::json;

void parse_pair(std::string value, std::string &one, std::string &two) {
    if(value.back() != '@') value += "@";
    std::size_t start_pos = 0;
    while(true) {
        std::size_t found_beg = value.find_first_of("@", start_pos);
        if(found_beg != std::string::npos) {
            std::size_t len = found_beg - start_pos;
            if(len > 0) {
                if(start_pos == 0) one = value.substr(start_pos, len);
                else two = value.substr(start_pos, len);
            }
            start_pos = found_beg + 1;
        } else break;
    }
}

int main() {
    std::string one, two;
    parse_pair("btcusdt@depth", one, two);
    std::cout << one << std::endl;
    std::cout << two << std::endl;
    return 0;
    //wss://
    //kline_
    //stream.binancefuture.com/stream?streams=btcusdt@depth@bookTicker@kline_1m
    //stream.binancefuture.com/stream?streams=btcusdt@bookTicker
    WssClient client("stream.binancefuture.com/stream", true, std::string(), std::string(), "curl-ca-bundle.crt");

    uint32_t volume = 0;
    client.on_message = [&](shared_ptr<WssClient::Connection> connection, std::shared_ptr<WssClient::InMessage> message) {
        std::string temp = message->string();
        std::cout << temp << std::endl;
    };

    client.on_open = [](shared_ptr<WssClient::Connection> connection) {
        std::cout << "Client: Opened connection" << std::endl;
        json j;
        j["method"] = "SUBSCRIBE";
        j["params"] = json::array();
        j["params"][0] = "btcusdt@kline_1m";
        j["params"][1] = "ethusdt@kline_1m";
        j["id"] = 1;
        string message = j.dump();
        cout << "Client: Sending message: \"" << message << "\"" << endl;
        connection->send(message);
    };

    client.on_close = [](shared_ptr<WssClient::Connection> /*connection*/, int status, const string & /*reason*/) {
        std::cout << "Client: Closed connection with status code " << status << endl;
    };

    // See http://www.boost.org/doc/libs/1_55_0/doc/html/boost_asio/reference.html, Error Codes for error code meanings
    client.on_error = [](shared_ptr<WssClient::Connection> /*connection*/, const SimpleWeb::error_code &ec) {
        cout << "Client: Error: " << ec << ", error message: " << ec.message() << endl;
    };

    client.start();

    return 0;
}
