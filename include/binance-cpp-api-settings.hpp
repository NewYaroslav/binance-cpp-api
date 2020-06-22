#ifndef BINANCE_CPP_API_SETTINGS_HPP_INCLUDED
#define BINANCE_CPP_API_SETTINGS_HPP_INCLUDED

#include <binance-cpp-api-common.hpp>
#include <nlohmann/json.hpp>

namespace binance_api {
    using json = nlohmann::json;
    using namespace common;

    /** \brief Класс настроек
     */
    class Settings {
    public:
        std::string json_settings_file;
        std::string path;
        std::string sert_file = "curl-ca-bundle.crt";       /**< Файл сертификата */
        std::string cookie_file = "binance_api_bot.cookie"; /**< Файл cookie */
        std::string named_pipe = "binance_api_bot";         /**< Имя именованного канала */
        std::vector<std::pair<std::string, uint32_t>> symbols;
        uint32_t candles = 1440;
        bool is_error = false;

        Settings() {};

        Settings(const int argc, char **argv) {
            /* обрабатываем аргументы командой строки */
            json j;
            bool is_default = false;
            if(!process_arguments(
                    argc,
                    argv,
                    [&](
                        const std::string &key,
                        const std::string &value) {
                /* аргумент json_file указываает на файл с настройками json */
                if(key == "json_settings_file" || key == "jsf" || key == "jf") {
                    json_settings_file = value;
                }
            })) {
                /* параметры не были указаны */
                if(!open_json_file("config.json", j)) {
                    is_error = true;
                    return;
                }
                is_default = true;
            }

            if(!is_default && !open_json_file(json_settings_file, j)) {
                is_error = true;
                return;
            }

            /* разбираем json сообщение */
            try {
                if(j["named_pipe"] != nullptr) named_pipe = j["named_pipe"];
                if(j["candles"] != nullptr) candles = j["candles"];
                if(j["path"] != nullptr) path = j["path"];
                if(j["symbols"] != nullptr && j["symbols"].is_array()) {
                    const size_t symbols_size = j["symbols"].size();
                    for(size_t i = 0; i < symbols_size; ++i) {
                        const std::string symbol = j["symbols"][i]["symbol"];
                        const uint32_t period = j["symbols"][i]["period"];
                        symbols.push_back(std::make_pair(symbol,period));
                    }
                }
            }
            catch(...) {
                is_error = true;
            }
            if(path.size() == 0 || symbols.size() == 0) is_error = true;
        }
    };
}

#endif // BINANCE_CPP_API_SETTINGS_HPP_INCLUDED
