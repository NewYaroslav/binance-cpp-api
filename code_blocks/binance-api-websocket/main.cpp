#include <iostream>
#include "binance-cpp-api-websocket.hpp"

using namespace std;

int main() {
    std::cout << "start" << std::endl;
    binance_api::CandlestickStreams candlestick_streams;
    candlestick_streams.on_candle = [&](
            const std::string &symbol,
            const xquotes_common::Candle &candle,
            const uint32_t period,
            const bool close_candle) {
        std::cout
            << symbol
            //<< " o: " << candle.open
            << " c: " << candle.close
            << " h: " << candle.high
            << " l: " << candle.low
            << " v: " << candle.volume
            << " p: " << period
            << " t: " << xtime::get_str_time(candle.timestamp)
            << " cc: " << close_candle
            << std::endl;
    };

    candlestick_streams.add_symbol_stream("btcusdt", 1);
    candlestick_streams.add_symbol_stream("btcusdt", 5);
    candlestick_streams.add_symbol_stream("ethusdt", 1);

    candlestick_streams.start();
    candlestick_streams.wait();
    //candlestick_streams.add_symbol_stream("btcusdt", 1);
    //candlestick_streams.add_symbol_stream("ethusdt", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    candlestick_streams.add_symbol_stream("ethusdt", 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    std::cout << "UNSUBSCRIBE" << std::endl;
    candlestick_streams.del_symbol_stream("ethusdt", 5);
    std::system("pause");
    return EXIT_SUCCESS;
}
