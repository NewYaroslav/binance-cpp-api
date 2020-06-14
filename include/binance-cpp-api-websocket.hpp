/*
* intrade-bar-api-cpp - C ++ API client for intrade.bar
*
* Copyright (c) 2019 Elektro Yar. Email: git.electroyar@gmail.com
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#ifndef BINANCE_CPP_API_WEBSOCKET_HPP_INCLUDED
#define BINANCE_CPP_API_WEBSOCKET_HPP_INCLUDED

#include <binance-cpp-api-common.hpp>
#include "client_wss.hpp"
#include <openssl/ssl.h>
#include <wincrypt.h>
#include <xtime.hpp>
#include <nlohmann/json.hpp>
#include <xquotes_common.hpp>
#include <mutex>
#include <atomic>
#include <future>
//#include "utf8.h" // http://utfcpp.sourceforge.net/

/*
 * клиент иметь несколько конечных точек
 * https://gitlab.com/eidheim/Simple-WebSocket-Server/-/issues/136
 */

namespace binance_api {
    using namespace common;

    /** \brief Класс потока котировок
     */
    class CandlestickStreams {
    private:
        using WssClient = SimpleWeb::SocketClient<SimpleWeb::WSS>;
        using json = nlohmann::json;
        std::string point = "stream.binancefuture.com/stream";
        std::string sert_file = "curl-ca-bundle.crt";

        std::shared_ptr<WssClient::Connection> save_connection;
        std::shared_ptr<WssClient> client;      /**< Webclosket Клиент */
        std::future<void> client_future;		/**< Поток соединения */
        std::mutex save_connection_mutex;

        const std::map<std::string, uint32_t> str_interval_to_index = {
            {"1m",1},{"3m",3},{"5m",5},{"15m",15},{"30m",30},
            {"1h",60},{"2h",120},{"4h",240},{"6h",360},{"8h",480},{"12h",720},
            {"1d",1440},{"3d",4320},{"1w",10080},{"1M",43200}
        }; /**< Преобразование строкового периода в число */

        const std::map<uint32_t, std::string> index_interval_to_str = {
            {1,"1m"},{3,"3m"},{5,"5m"},{15,"15m"},{30,"30m"},
            {60,"1h"},{120,"2h"},{240,"4h"},{360,"6h"},{480,"8h"},{720,"12h"},
            {1440,"1d"},{4320,"3d"},{10080,"1w"},{43200,"1M"}
        }; /**< Преобразование индекса периода в строку */

        std::map<std::string, std::map<uint32_t, bool>> list_subscriptions;
        std::mutex list_subscriptions_mutex;

        using candle_data = std::map<xtime::timestamp_t, xquotes_common::Candle>;
        using period_data = std::map<uint32_t, candle_data>;
        std::map<std::string, period_data> candles;
        std::recursive_mutex candles_mutex;

        std::atomic<bool> is_websocket_init;    /**< Состояние соединения */
        std::atomic<bool> is_error;             /**< Ошибка соединения */
        std::atomic<bool> is_close_connection;  /**< Флаг для закрытия соединения */
        std::atomic<bool> is_open;

        std::string error_message;
        std::recursive_mutex error_message_mutex;
        std::recursive_mutex array_offset_timestamp_mutex;

        const uint32_t array_offset_timestamp_size = 256;
        std::array<xtime::ftimestamp_t, 256> array_offset_timestamp;    /**< Массив смещения метки времени */
        uint8_t index_array_offset_timestamp = 0;                       /**< Индекс элемента массива смещения метки времени */
        uint32_t index_array_offset_timestamp_count = 0;
        xtime::ftimestamp_t last_offset_timestamp_sum = 0;
        std::atomic<double> offset_timestamp;                           /**< Смещение метки времени */
        std::atomic<bool> is_autoupdate_logger_offset_timestamp;

        std::atomic<double> last_server_timestamp;

        /** \brief Обновить смещение метки времени
         *
         * Данный метод использует оптимизированное скользящее среднее
         * для выборки из 256 элеметов для нахождения смещения метки времени сервера
         * относительно времени компьютера
         * \param offset смещение метки времени
         */
        inline void update_offset_timestamp(const xtime::ftimestamp_t offset) {
            std::lock_guard<std::recursive_mutex> lock(array_offset_timestamp_mutex);

            if(index_array_offset_timestamp_count != array_offset_timestamp_size) {
                array_offset_timestamp[index_array_offset_timestamp] = offset;
                index_array_offset_timestamp_count = (uint32_t)index_array_offset_timestamp + 1;
                last_offset_timestamp_sum += offset;
                offset_timestamp = last_offset_timestamp_sum / (xtime::ftimestamp_t)index_array_offset_timestamp_count;
                ++index_array_offset_timestamp;
                return;
            }
            /* находим скользящее среднее смещения метки времени сервера относительно компьютера */
            last_offset_timestamp_sum = last_offset_timestamp_sum +
                (offset - array_offset_timestamp[index_array_offset_timestamp]);
            array_offset_timestamp[index_array_offset_timestamp++] = offset;
            offset_timestamp = last_offset_timestamp_sum/
                (xtime::ftimestamp_t)array_offset_timestamp_size;
        }

        /** \brief Парсер строки, состоящей из пары параметров
         *
         * Разделитель - символ @
         * \param value Строка
         * \param one Первое значение
         * \param two Второе значение
         */
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

        /** \brief Парсер сообщения от вебсокета
         * \param response Ответ от сервера
         */
        void parser(const std::string &response) {
            /* Пример сообщения с котировками
                {
                    "stream":"btcusdt@kline_1m",
                    "data":{
                        "e":"kline",
                        "E":1591780986589,
                        "s":"BTCUSDT",
                        "k":{
                            "t":1591780980000,
                            "T":1591781039999,
                            "s":"BTCUSDT",
                            "i":"1m",
                            "f":338196545,
                            "L":338196585,
                            "o":"9720.65000000",
                            "c":"9720.51000000",
                            "h":"9721.44000000",
                            "l":"9720.00000000",
                            "v":"3.26326300",
                            "n":41,
                            "x":false,
                            "q":"31719.81462426",
                            "V":"1.13514200",
                            "Q":"11033.93899455",
                            "B":"0"
                        }
                    }
                }
             */
            try {
                json j = json::parse(response);

                /* парсим параметры потока */
                const std::string stream = j["stream"];
                std::string symbol, param;
                parse_pair(stream, symbol, param);
                std::string s = to_upper_case(symbol);

                /* проверяем, является ли наш поток потоком баров */
                if(param.find("kline") != std::string::npos) {
                    auto j_data = j.find("data");
                    auto j_kline = j_data->find("k");

                    const std::string timeframe = (*j_kline)["i"];
                    const bool close_candle = (*j_kline)["x"];
                    const double open = std::atof(std::string((*j_kline)["o"]).c_str());
                    const double close = std::atof(std::string((*j_kline)["c"]).c_str());
                    const double high = std::atof(std::string((*j_kline)["h"]).c_str());
                    const double low = std::atof(std::string((*j_kline)["l"]).c_str());
                    const double volume = std::atof(std::string((*j_kline)["v"]).c_str());

                    const xtime::timestamp_t open_timestamp = ((xtime::timestamp_t)(*j_kline)["t"]) / 1000;
                    const xtime::ftimestamp_t timestamp = ((xtime::ftimestamp_t)(*j_data)["E"]) / 1000.0d;

                    /* проверяем, не поменялась ли метка времени */
                    static xtime::ftimestamp_t last_timestamp = 0;
                    if(last_timestamp < timestamp) {

                        /* если метка времени поменялась, найдем время сервера */
                        xtime::ftimestamp_t pc_timestamp = xtime::get_ftimestamp();
                        xtime::ftimestamp_t offset_timestamp = timestamp - pc_timestamp;
                        update_offset_timestamp(offset_timestamp);
                        last_timestamp = timestamp;

                        /* запоминаем последнюю метку времени сервера */
                        last_server_timestamp = timestamp;
                    }

                    auto it = str_interval_to_index.find(timeframe);
                    if(it == str_interval_to_index.end()) return;

                    xquotes_common::Candle candle(open,high,low,close,volume,open_timestamp);
                    {
                        std::lock_guard<std::recursive_mutex> lock(candles_mutex);
                        candles[s][it->second][open_timestamp] = candle;
                    }
                    if(on_candle != nullptr) on_candle(s, candle, it->second, close_candle);
                    is_websocket_init = true;
                }
            }
            catch(const json::parse_error& e) {
            }
            catch(json::out_of_range& e) {
            }
            catch(json::type_error& e) {
            }
            catch(...) {
            }
        }

        void send(const std::string &message) {
            std::lock_guard<std::mutex> lock(save_connection_mutex);
            if(!save_connection) return;
            save_connection->send(message);
        }

        std::string to_upper_case(const std::string &s){
            std::string temp = s;
            std::transform(temp.begin(), temp.end(), temp.begin(), [](char ch) {
                return std::use_facet<std::ctype<char>>(std::locale()).toupper(ch);
            });
            return temp;
        }

    public:
        std::function<void(
            const std::string &symbol,
            const xquotes_common::Candle &candle,
            const uint32_t period,
            const bool close_candle)> on_candle = nullptr;

        /** \brief Конструктор класс для получения потока котировок
         * \param user_point Конечная точка подключения
         * \param user_sert_file Файл-сертификат. По умолчанию используется от curl: curl-ca-bundle.crt
         */
        CandlestickStreams(
                const std::string user_point = "stream.binancefuture.com/stream",
                const std::string user_sert_file = "curl-ca-bundle.crt") {
            /* инициализируем переменные */
            point = user_point;
            sert_file = user_sert_file;
            offset_timestamp = 0;
            is_websocket_init = false;
            is_close_connection = false;
            is_error = false;
            is_open = false;
        }

        ~CandlestickStreams() {
            is_close_connection = true;
            std::shared_ptr<WssClient> client_ptr = std::atomic_load(&client);
            if(client_ptr) {
                client_ptr->stop();
            }

            if(client_future.valid()) {
                try {
                    client_future.wait();
                    client_future.get();
                }
                catch(const std::exception &e) {
                    std::cerr << "binance_api::~CandlestickStreams() error, what: " << e.what() << std::endl;
                }
                catch(...) {
                    std::cerr << "binance_api::~CandlestickStreams() error" << std::endl;
                }
            }
        };

        /** \brief Состояние соединения
         * \return вернет true, если соединение есть
         */
        inline bool connected() {
            return is_websocket_init;
        }

        /** \brief Подождать соединение
         *
         * Данный метод ждет, пока не установится соединение
         * \return вернет true, если соединение есть, иначе произошла ошибка
         */
        inline bool wait() {
            uint32_t tick = 0;
            while(!is_error && !is_open && !is_close_connection) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ++tick;
                const uint32_t MAX_TICK = 10*100*5;
                if(tick > MAX_TICK) {
                    is_error = true;
                    return is_open;
                }
            }
            return is_open;
        }

        /** \brief Получить метку времени сервера
         *
         * Данный метод возвращает метку времени сервера. Часовая зона: UTC/GMT
         * \return Метка времени сервера
         */
        inline xtime::ftimestamp_t get_server_timestamp() {
            return xtime::get_ftimestamp() + offset_timestamp;
        }

        /** \brief Получить последнюю метку времени сервера
         *
         * Данный метод возвращает последнюю полученную метку времени сервера. Часовая зона: UTC/GMT
         * \return Метка времени сервера
         */
        inline xtime::ftimestamp_t get_last_server_timestamp() {
            return last_server_timestamp;
        }

        /** \brief Получить смещение метки времени ПК
         * \return Смещение метки времени ПК
         */
        inline xtime::ftimestamp_t get_offset_timestamp() {
            return offset_timestamp;
        }

        /** \brief Получить цену тика символа
         *
         * \param symbol Имя символа
         * \param period Период
         * \return Последняя цена bid
         */
        inline double get_price(const std::string &symbol, const uint32_t period) {
            if(!is_websocket_init) return 0.0;
            std::string s = to_upper_case(symbol);
            std::lock_guard<std::recursive_mutex> lock(candles_mutex);
            auto it_symbol = candles.find(s);
            if(it_symbol == candles.end()) return 0.0;
            auto it_period = it_symbol->second.find(period);
            if(it_period == it_symbol->second.end()) return 0.0;
            const size_t size_candle = it_period->second.size();
            if(size_candle == 0) return 0.0;
            auto it_begin_candle = it_period->second.begin();
            std::advance(it_begin_candle, (size_candle - 1));
            return it_begin_candle->second.close;
        }

        /** \brief Получить бар
         *
         * \param symbol Имя символа
         * \param period Период
         * \param offset Смещение
         * \return Бар
         */
        inline xquotes_common::Candle get_candle(
                const std::string &symbol,
                const uint32_t period,
                const size_t offset = 0) {
            if(!is_websocket_init) return xquotes_common::Candle();
            std::string s = to_upper_case(symbol);
            std::lock_guard<std::recursive_mutex> lock(candles_mutex);
            auto it_symbol = candles.find(s);
            if(it_symbol == candles.end()) return xquotes_common::Candle();
            auto it_period = it_symbol->second.find(period);
            if(it_period == it_symbol->second.end()) return xquotes_common::Candle();
            const size_t size_candle = it_period->second.size();
            if(size_candle == 0) return xquotes_common::Candle();
            if(offset >= size_candle) return xquotes_common::Candle();
            auto it_begin_candle = it_period->second.begin();
            std::advance(it_begin_candle, (size_candle - 1 - offset));
            return it_begin_candle->second;
        }

        /** \brief Получить количество баров
         * \param symbol Имя символа
         * \param period Период
         * \return Количество баров
         */
        inline uint32_t get_num_candles(
                const std::string &symbol,
                const uint32_t period) {
            if(!is_websocket_init) return 0;
            std::string s = to_upper_case(symbol);
            std::lock_guard<std::recursive_mutex> lock(candles_mutex);
            auto it_symbol = candles.find(s);
            if(it_symbol == candles.end()) return 0;
            auto it_period = it_symbol->second.find(period);
            if(it_period == it_symbol->second.end()) return 0;
            return it_period->second.size();
        }

        /** \brief Получить бар по метке времени
         * \param symbol Имя символа
         * \param period Период
         * \param timestamp Метка времени
         * \return Бар
         */
        inline xquotes_common::Candle get_timestamp_candle(
                const std::string &symbol,
                const uint32_t period,
                const xtime::timestamp_t timestamp) {
            if(!is_websocket_init) return xquotes_common::Candle();
            std::string s = to_upper_case(symbol);
            std::lock_guard<std::recursive_mutex> lock(candles_mutex);
            auto it_symbol = candles.find(s);
            if(it_symbol == candles.end()) return xquotes_common::Candle();
            auto it_period = it_symbol->second.find(period);
            if(it_period == it_symbol->second.end()) return xquotes_common::Candle();
            if(it_period->second.size() == 0) return xquotes_common::Candle();
            auto it_candle = it_period->second.find(timestamp);
            if(it_candle == it_period->second.end()) return xquotes_common::Candle();
            return it_candle->second;
        }

        /** \brief Инициализировать массив японских свечей
         * \param symbol Имя символа
         * \param period Период
         * \param new_candles Массив баров
         * \return Код ошибки, вернет 0 если все в порядке
         */
        template<class T>
        int init_array_candles(
                const std::string &symbol,
                const uint32_t period,
                const T &new_candles) {
            std::string s = to_upper_case(symbol);
            std::lock_guard<std::recursive_mutex> lock(candles_mutex);
            auto it_symbol = candles.find(s);
            if(it_symbol == candles.end()) {
                candles.insert(
                    std::pair<std::string, period_data>(s, period_data()));
                it_symbol = candles.find(s);
            }
            auto it_period = it_symbol->second.find(period);
            if(it_period == it_symbol->second.end()) {
                it_symbol->second.insert(
                    std::pair<uint32_t, candle_data>(period, candle_data()));
                it_period = it_symbol->second.find(period);
            }

            for(auto &candle : new_candles) {
                it_period->second.insert(
                    std::pair<xtime::timestamp_t, xquotes_common::Candle>(candle.timestamp, candle));
            }
            return OK;
        }

        /** \brief Ждать закрытие бара (минутного)
         * \param f Лямбда-функция, которую можно использовать как callbacks
         */
        inline void wait_candle_close(std::function<void(
                const xtime::ftimestamp_t timestamp,
                const xtime::ftimestamp_t timestamp_stop)> f = nullptr) {
            const xtime::ftimestamp_t timestamp_stop =
                xtime::get_first_timestamp_minute(get_server_timestamp()) +
                xtime::SECONDS_IN_MINUTE;
            while(!is_close_connection) {
                const xtime::ftimestamp_t t = get_server_timestamp();
                if(t >= timestamp_stop) break;
                if(f != nullptr) f(t, timestamp_stop);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        /** \brief Проверить наличие ошибки
         * \return вернет true, если была ошибка
         */
        inline bool check_error() {
            return is_error;
        }

        /** \brief Очистить состояние ошибки
         */
        inline void clear_error() {
            is_error = false;
            std::lock_guard<std::recursive_mutex> lock(error_message_mutex);
            error_message.clear();
        }

        /** \brief Получить текст сообщения об ошибке
         * \return сообщения об ошибке, если есть
         */
        std::string get_error_message() {
            std::lock_guard<std::recursive_mutex> lock(error_message_mutex);
            if(is_error) return error_message;
            return std::string();
        }

        /** \brief Добавить поток символа с заданным периодом
         * \param symbol Имя символа
         * \param period Период
         */
        void add_symbol_stream(
                const std::string &symbol,
                const uint32_t period) {
            auto it = index_interval_to_str.find(period);
            if(it == index_interval_to_str.end()) return;
            json j;
            j["method"] = "SUBSCRIBE";
            j["params"] = json::array();
            std::string param = symbol + "@kline_" + it->second;
            j["params"][0] = param;
            j["id"] = 1;
            {
                std::lock_guard<std::mutex> lock(list_subscriptions_mutex);
                /* имя символа ОБЯЗАТЕЛЬНО В НИЖНЕМ РЕГИСТРЕ! */
                list_subscriptions[symbol][period] = true;
            }
            if(!is_open) return;
            send(j.dump());
        }

        /** \brief Убрать поток символа с заданным периодом
         * \param symbol Имя символа
         * \param period Период
         */
        void del_symbol_stream(
                const std::string &symbol,
                const uint32_t period) {
            auto it = index_interval_to_str.find(period);
            if(it == index_interval_to_str.end()) return;
            json j;
            j["method"] = "UNSUBSCRIBE";
            j["params"] = json::array();
            std::string param = symbol + "@kline_" + it->second;
            j["params"][0] = param;
            j["id"] = 1;
            {
                std::lock_guard<std::mutex> lock(list_subscriptions_mutex);
                /* имя символа ОБЯЗАТЕЛЬНО В НИЖНЕМ РЕГИСТРЕ! */
                auto it_symbol = list_subscriptions.find(symbol);
                if(it_symbol != list_subscriptions.end()) {
                    auto it_period = it_symbol->second.find(period);
                    if(it_period != it_symbol->second.end()) {
                        it_symbol->second.erase(period);
                    }
                }
            }
            if(!is_open) return;
            send(j.dump());
        }

        void start() {
            if(client_future.valid()) return;
            /* запустим соединение в отдельном потоке */
            client_future = std::async(std::launch::async,[&]() {
                while(!is_close_connection) {
                    try {
                        /* создадим соединение */;
                        client = std::make_shared<WssClient>(
                                point,
                                true,
                                std::string(),
                                std::string(),
                                std::string(sert_file));

                        /* читаем собщения, которые пришли */
                        client->on_message =
                                [&](std::shared_ptr<WssClient::Connection> connection,
                                std::shared_ptr<WssClient::InMessage> message) {
                            parser(message->string());
                            //std::cout << "on_message " << message->string() << std::endl;
                        };

                        client->on_open =
                            [&](std::shared_ptr<WssClient::Connection> connection) {
                            {
                                std::lock_guard<std::mutex> lock(save_connection_mutex);
                                save_connection = connection;
                            }
                            if(list_subscriptions.size() != 0) {
                                json j;
                                j["method"] = "SUBSCRIBE";
                                j["params"] = json::array();
                                j["id"] = 1;
                                {
                                    std::lock_guard<std::mutex> lock(list_subscriptions_mutex);
                                    uint32_t index = 0;
                                    for(auto &item_symbol : list_subscriptions) {
                                        /* имя символа ОБЯЗАТЕЛЬНО В НИЖНЕМ РЕГИСТРЕ! */
                                        const std::string symbol = item_symbol.first;
                                        for(auto &subscrip : item_symbol.second) {
                                            const uint32_t period = subscrip.first;
                                            auto it = index_interval_to_str.find(period);
                                            if(it == index_interval_to_str.end()) return;
                                            std::string param = symbol + "@kline_" + it->second;
                                            j["params"][index++] = param;
                                        }
                                    }
                                }
                                //std::cout << "j.dump()" << j.dump() << std::endl;
                                connection->send(j.dump());
                            }
                            is_open = true;
                            //std::cout << "on_open" << std::endl;
                        };

                        client->on_close =
                                [&](std::shared_ptr<WssClient::Connection> /*connection*/,
                                int status, const std::string & /*reason*/) {
                            is_websocket_init = false;
                            is_open = false;
                            is_error = true;
                            {
                                std::lock_guard<std::mutex> lock(save_connection_mutex);
                                if(save_connection) save_connection.reset();
                            }
                            std::cerr
                                << point
                                << " closed connection with status code " << status
                                << std::endl;
                        };

                        // See http://www.boost.org/doc/libs/1_55_0/doc/html/boost_asio/reference.html, Error Codes for error code meanings
                        client->on_error =
                                [&](std::shared_ptr<WssClient::Connection> /*connection*/,
                                const SimpleWeb::error_code &ec) {
                            is_websocket_init = false;
                            is_open = false;
                            is_error = true;

                            {
                                std::lock_guard<std::mutex> lock(save_connection_mutex);
                                if(save_connection) save_connection.reset();
                            }

                            std::cerr
                                << point
                                << " wss error: " << ec
                                << std::endl;
                        };
                        client->start();
                        client.reset();
                    } catch (std::exception& e) {
                        is_websocket_init = false;
                        is_error = true;
                    }
                    catch (...) {
                        is_websocket_init = false;
                        is_error = true;
                    }
                    if(is_close_connection) break;
					const uint64_t RECONNECT_DELAY = 1000;
					std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY));
                } // while
            });
        }
    };
}

#endif // BINANCE_CPP_API_WEBSOCKET_HPP_INCLUDED
