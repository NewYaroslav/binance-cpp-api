#include <iostream>
#include <fstream>

#include "binance-cpp-api.hpp"
#include "binance-cpp-api-http.hpp"
#include "binance-cpp-api-websocket.hpp"

int main(int argc, char **argv) {
    std::cout << "start binance cpp api general step 1 test!" << std::endl;
    binance_api::BinanceApi binanceapi;
    binance_api::Settings settings(argc, argv);
    if(settings.is_error) {
        std::cout << "Settings error!" << std::endl;
        return EXIT_FAILURE;
    }
    settings.demo = true;
    /* инициализируем основные настройки */
    binanceapi.init_main(settings);

    /* отменяем уже открытые ордера */
    int err = binanceapi.close_order("BTCUSDT");
    std::cout << "close_order code: " << err << std::endl;

    /* открываем ордер */

    std::system("pause");
    return 0;
}
