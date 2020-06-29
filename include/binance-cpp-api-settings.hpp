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
        std::string api_key;                                /**< Ключ API */
        std::string secret_key;                             /**< Секретный ключ API */
        std::string path;
        std::string sert_file = "curl-ca-bundle.crt";       /**< Файл сертификата */
        std::string cookie_file = "binance-api.cookie";     /**< Файл cookie */
        std::string named_pipe = "binance_api_bot";         /**< Имя именованного канала */
        std::vector<std::pair<std::string, uint32_t>> symbols;
        std::vector<std::pair<std::string, uint32_t>> leverages;        /**< Крединое плечо для всех указанных символов */
        std::vector<std::pair<std::string, TypesMargin>> margin_types;  /**< Типы маржи для всех указанных символов */
        TypesPositionMode position_mode = TypesPositionMode::Hedge_Mode;    /**< Режим хеджирования */

        uint32_t candles = 1440;                            /**< Количество баров истории */
        uint32_t recv_window = 60000;                       /**< Время ответа от сервера для критически важных запросов */
        bool demo = true;                                   /**< Флаг демо аккаунта */

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
                if(j["api_key"] != nullptr) api_key = j["api_key"];
                if(j["secret_key"] != nullptr) secret_key = j["secret_key"];
                //if(j["demo"] != nullptr) named_pipe = j["demo"];
                if(j["named_pipe"] != nullptr) named_pipe = j["named_pipe"];
                if(j["candles"] != nullptr) candles = j["candles"];
                if(j["recv_window"] != nullptr) recv_window = j["recv_window"];
                if(j["path"] != nullptr) path = j["path"];
                if(j["symbols"] != nullptr && j["symbols"].is_array()) {
                    const size_t symbols_size = j["symbols"].size();
                    for(size_t i = 0; i < symbols_size; ++i) {
                        const std::string symbol = j["symbols"][i]["symbol"];
                        const uint32_t period = j["symbols"][i]["period"];
                        symbols.push_back(std::make_pair(symbol,period));
                    }
                }
                if(j["leverages"] != nullptr && j["leverages"].is_array()) {
                    const size_t leverages_size = j["leverages"].size();
                    for(size_t i = 0; i < leverages_size; ++i) {
                        const std::string symbol = j["leverages"][i]["symbol"];
                        const uint32_t leverage = j["leverages"][i]["leverage"];
                        leverages.push_back(std::make_pair(symbol, leverage));
                    }
                }
                if(j["margin_types"] != nullptr && j["margin_types"].is_array()) {
                    const size_t margin_types_size = j["margin_types"].size();
                    for(size_t i = 0; i < margin_types_size; ++i) {
                        const std::string symbol = j["margin_types"][i]["symbol"];
                        const std::string str_margin_type = j["margin_types"][i]["margin_type"];
                        TypesMargin margin_type = TypesMargin::CROSSED;
                        if(str_margin_type == "ISOLATED" || str_margin_type == "isolated") {
                            margin_type = TypesMargin::ISOLATED;
                        } else
                        if(str_margin_type == "CROSSED" || str_margin_type == "crossed") {
                            margin_type = TypesMargin::CROSSED;
                        }
                        margin_types.push_back(std::make_pair(symbol, margin_type));
                    }
                }
                if(j["position_mode"] != nullptr) {
                    if (j["position_mode"] == "Hedge_Mode" ||
                        j["position_mode"] == "HedgeMode" ||
                        j["position_mode"] == "Hedge-Mode" ||
                        j["position_mode"] == "hedge-mode" ||
                        j["position_mode"] == "hedge_mode") {
                        position_mode = TypesPositionMode::Hedge_Mode;
                    } else
                    if (j["position_mode"] == "One_way_Mode" ||
                        j["position_mode"] == "OneWayMode" ||
                        j["position_mode"] == "One-Way-Mode" ||
                        j["position_mode"] == "one-way-mode" ||
                        j["position_mode"] == "one_way_mode") {
                        position_mode = TypesPositionMode::One_way_Mode;
                    }
                }
            }
            catch(const json::parse_error& e) {
                std::cerr << "binance_api::Settings parser error (json::parse_error), what: " << std::string(e.what()) << std::endl;
                is_error = true;
            }
            catch(const json::out_of_range& e) {
                std::cerr << "binance_api::Settings parser error (json::out_of_range), what: " << std::string(e.what()) << std::endl;
                is_error = true;
            }
            catch(const json::type_error& e) {
                std::cerr << "binance_api::Settings parser error (json::type_error), what: " << std::string(e.what()) << std::endl;
                is_error = true;
            }
            catch(...) {
                std::cerr << "binance_api::Settings parser error" << std::endl;
                is_error = true;
            }
            if(path.size() == 0 || symbols.size() == 0) is_error = true;
        }
    };
}

#endif // BINANCE_CPP_API_SETTINGS_HPP_INCLUDED
