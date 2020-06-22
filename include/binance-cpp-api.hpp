#ifndef BINANCE_CPP_API_HPP_INCLUDED
#define BINANCE_CPP_API_HPP_INCLUDED

#include "binance-cpp-api-http.hpp"
#include "binance-cpp-api-websocket.hpp"

namespace binance_api {
    using json = nlohmann::json;
    using namespace common;

    class BinanceApi {
    public:
        BinanceHttpApi binance_http_api;

        BinanceApi() {

        };

        void open_order(
                const std::string &symbol,
                const TypesPositionSide position_side,
                const double amount,
                const double take_profit,
                const double stop_loss,
                const uint64_t expiration) {
            /* В самом начале надо открыть три ордера.
             * Первый ордер открываем на short или long с рыночным исполнением
             * Еще два ордера октрываем по уровням take_profit и stop_loss с типом STOP MARKET
             * Также устанавливаем время жизни сделки
             */
            binance_http_api.new_order(symbol, )
        }
    };

}

#endif // BINANCE_CPP_API_HPP_INCLUDED
