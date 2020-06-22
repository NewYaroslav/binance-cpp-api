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
#ifndef BINANCE_CPP_API_HTTP_HPP_INCLUDED
#define BINANCE_CPP_API_HTTP_HPP_INCLUDED

#include <binance-cpp-api-common.hpp>
#include <xquotes_common.hpp>
#include <curl/curl.h>
#include <gzip/decompress.hpp>
#include <nlohmann/json.hpp>
#include "hmac.hpp"
#include "xtime.hpp"
#include <thread>
#include <future>
#include <mutex>
#include <atomic>
#include <array>
#include <map>
//#include "utf8.h" // http://utfcpp.sourceforge.net/

namespace binance_api {
    using json = nlohmann::json;
    using namespace common;

    /** \brief Класс API брокера Binance
     */
    class BinanceHttpApi {
    private:
        //std::string point = "https://api.binance.com";
        std::string point = "https://testnet.binancefuture.com";
        std::string api_key;
        std::string secret_key;
        std::string sert_file = "curl-ca-bundle.crt";   /**< Файл сертификата */
        std::string cookie_file = "binance-api.cookie"; /**< Файл cookie */

        std::atomic<bool> is_demo = ATOMIC_VAR_INIT(true);  /**< Флаг демо-аккаунта */
        char error_buffer[CURL_ERROR_SIZE];
        static const int POST_TIME_OUT = 10;    /**< Время ожидания ответа сервера для разных запросов */

        /** \brief Класс для хранения Http заголовков
         */
        class HttpHeaders {
        private:
            struct curl_slist *http_headers = nullptr;
        public:

            HttpHeaders() {};

            HttpHeaders(std::vector<std::string> headers) {
                for(size_t i = 0; i < headers.size(); ++i) {
                    add_header(headers[i]);
                }
            };

            void add_header(const std::string &header) {
                http_headers = curl_slist_append(http_headers, header.c_str());
            }

            void add_header(const std::string &key, const std::string &val) {
                std::string header(key + ": " + val);
                http_headers = curl_slist_append(http_headers, header.c_str());
            }

            ~HttpHeaders() {
                if(http_headers != nullptr) {
                    curl_slist_free_all(http_headers);
                    http_headers = nullptr;
                }
            };

            inline struct curl_slist *get() {
                return http_headers;
            }
        };

        /* ограничение количества запросов в минуту */
        std::atomic<uint32_t> request_counter = ATOMIC_VAR_INIT(0);
        std::atomic<uint32_t> request_limit = ATOMIC_VAR_INIT(6000);
        std::atomic<xtime::timestamp_t> request_timestamp = ATOMIC_VAR_INIT(0);

        void check_request_limit(const uint32_t weight = 1) {
            request_counter += weight;
            if(request_timestamp == 0) {
                request_timestamp = xtime::get_first_timestamp_minute();
                return;
            } else
            if(request_timestamp == xtime::get_first_timestamp_minute()) {
                if(request_counter >= request_limit) {
                    while(request_timestamp == xtime::get_first_timestamp_minute()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    request_counter = 0;
                    request_timestamp = xtime::get_first_timestamp_minute();
                    return;
                }
            } else {
                request_timestamp = xtime::get_first_timestamp_minute();
            }
        }


        std::mutex symbols_spec_mutex;
        std::map<std::string, SymbolSpec> symbols_spec; /**< Массив параметров символов */

        WalletSpec wallet;
        TypesPositionMode position_mode;                /**< Режим хеджирования позиции */
        std::vector<PositionSpec> positions;

        /** \brief Callback-функция для обработки ответа
         * Данная функция нужна для внутреннего использования
         */
        static int binance_writer(char *data, size_t size, size_t nmemb, void *userdata) {
            int result = 0;
            std::string *buffer = (std::string*)userdata;
            if(buffer != NULL) {
                buffer->append(data, size * nmemb);
                result = size * nmemb;
            }
            return result;
        }

        /** \brief Парсер строки, состоящей из пары параметров
         *
         * Разделитель - символ @
         * \param value Строка
         * \param one Первое значение
         * \param two Второе значение
         */
        static void parse_pair(std::string value, std::string &one, std::string &two) {
            if(value.back() != ' ' || value.back() != '\n' || value.back() != ' ') value += " ";
            std::size_t start_pos = 0;
            while(true) {
                std::size_t found_beg = value.find_first_of(" \t\n", start_pos);
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

        /** \brief Callback-функция для обработки HTTP Header ответа
         * Данный метод нужен, чтобы определить, какой тип сжатия данных используется (или сжатие не используется)
         * Данный метод нужен для внутреннего использования
         */
        static int binance_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
            size_t buffer_size = nitems * size;
            std::map<std::string,std::string> *headers = (std::map<std::string,std::string>*)userdata;
            std::string str_buffer(buffer, buffer_size);
            std::string key, val;
            parse_pair(str_buffer, key, val);
            headers->insert({key, val});
            return buffer_size;
        }

        /** \brief Часть парсинга HTML
         * Данный метод нужен для внутреннего использования
         */
        std::size_t get_string_fragment(
                const std::string &str,
                const std::string &div_beg,
                const std::string &div_end,
                std::string &out,
                std::size_t start_pos = 0) {
            std::size_t beg_pos = str.find(div_beg, start_pos);
            if(beg_pos != std::string::npos) {
                std::size_t end_pos = str.find(div_end, beg_pos + div_beg.size());
                if(end_pos != std::string::npos) {
                    out = str.substr(beg_pos + div_beg.size(), end_pos - beg_pos - div_beg.size());
                    return end_pos;
                } else return std::string::npos;
            } else return std::string::npos;
        }

        /** \brief Часть парсинга HTML
         * Данная метод нужен для внутреннего использования
         */
        std::size_t get_string_fragment(
                const std::string &str,
                const std::string &div_beg,
                std::string &out) {
            std::size_t beg_pos = str.find(div_beg, 0);
            if(beg_pos != std::string::npos) {
                out = str.substr(beg_pos + div_beg.size());
                return beg_pos;
            } else return std::string::npos;
        }

        enum class TypesRequest {
            REQ_GET = 0,
            REQ_POST = 1,
            REQ_PUT = 2,
            REQ_DELETE = 3
        };

        /** \brief Инициализация CURL
         *
         * Данная метод является общей инициализацией для разного рода запросов
         * Данный метод нужен для внутреннего использования
         * \param url URL запроса
         * \param body Тело запроса
         * \param response Ответ сервера
         * \param http_headers Заголовки HTTP
         * \param timeout Таймаут
         * \param writer_callback Callback-функция для записи данных от сервера
         * \param header_callback Callback-функция для обработки заголовков ответа
         * \param is_use_cookie Использовать cookie файлы
         * \param is_clear_cookie Очистить cookie файлы
         * \param type_req Использовать POST, GET и прочие запросы
         * \return вернет указатель на CURL или NULL, если инициализация не удалась
         */
        CURL *init_curl(
                const std::string &url,
                const std::string &body,
                std::string &response,
                struct curl_slist *http_headers,
                const int timeout,
                int (*writer_callback)(char*, size_t, size_t, void*),
                int (*header_callback)(char*, size_t, size_t, void*),
                void *userdata,
                const bool is_use_cookie = true,
                const bool is_clear_cookie = false,
                const TypesRequest type_req = TypesRequest::REQ_POST) {
            CURL *curl = curl_easy_init();
            if(!curl) return NULL;
            curl_easy_setopt(curl, CURLOPT_CAINFO, sert_file.c_str());
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            //curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            if(type_req == TypesRequest::REQ_POST) curl_easy_setopt(curl, CURLOPT_POST, 1L);
            else if(type_req == TypesRequest::REQ_GET) curl_easy_setopt(curl, CURLOPT_POST, 0);
            else if(type_req == TypesRequest::REQ_PUT) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            else if(type_req == TypesRequest::REQ_DELETE) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writer_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout); // выход через N сек
            if(is_use_cookie) {
                if(is_clear_cookie) curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
                else curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_file.c_str()); // запускаем cookie engine
                curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookie_file.c_str()); // запишем cookie после вызова curl_easy_cleanup
            }
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, userdata);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http_headers);
            if(type_req == TypesRequest::REQ_POST || type_req == TypesRequest::REQ_PUT || type_req == TypesRequest::REQ_DELETE) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            //curl_easy_setopt(curl, CURLOPT_VERBOSE, true);
            return curl;
        }

        /** \brief Обработать ответ сервера
         * \param curl Указатель на структуру CURL
         * \param headers Заголовки, которые были приняты
         * \param buffer Буфер с ответом сервера
         * \param response Итоговый ответ, который будет возвращен
         * \return Код ошибки
         */
        int process_server_response(CURL *curl, std::map<std::string,std::string> &headers, std::string &buffer, std::string &response) {
            CURLcode result = curl_easy_perform(curl);
            long response_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            //curl_easy_cleanup(curl);
            switch(response_code) {
            case 403: // Код возврата используется при нарушении лимита WAF (брандмауэр веб-приложений).
                return WAF_LIMIT;
            case 503: // Код возврата используется, когда API успешно отправил сообщение, но не получил ответ в течение периода ожидания.
                return NO_RESPONSE_WAITING_PERIOD;
            case 429: // Код возврата используется при нарушении ограничения скорости запроса.
                return LIMITING_NUMBER_REQUESTS;
            case 418: // Код возврата используется при блокировки после нарушении ограничения скорости запроса.
                return IP_BLOCKED;
            }

            if(result == CURLE_OK) {
                if(headers.find("Content-Encoding:") != headers.end()) {
                    std::string content_encoding = headers["Content-Encoding:"];
                    if(content_encoding.find("gzip") != std::string::npos) {
                        if(buffer.size() == 0) return NO_ANSWER;
                        const char *compressed_pointer = buffer.data();
                        response = gzip::decompress(compressed_pointer, buffer.size());
                    } else
                    if(content_encoding.find("identity") != std::string::npos) {
                        response = buffer;
                    } else {
                        if(response_code != 200) return CURL_REQUEST_FAILED;
                        return CONTENT_ENCODING_NOT_SUPPORT;
                    }
                } else
                if(headers.find("content-encoding:") != headers.end()) {
                    std::string content_encoding = headers["content-encoding:"];
                    if(content_encoding.find("gzip") != std::string::npos) {
                        if(buffer.size() == 0) return NO_ANSWER;
                        const char *compressed_pointer = buffer.data();
                        response = gzip::decompress(compressed_pointer, buffer.size());
                    } else
                    if(content_encoding.find("identity") != std::string::npos) {
                        response = buffer;
                    } else {
                        if(response_code != 200) return CURL_REQUEST_FAILED;
                        return CONTENT_ENCODING_NOT_SUPPORT;
                    }
                } else {
                    response = buffer;
                    if(response_code != 200) return CURL_REQUEST_FAILED;
                }
                if(response_code != 200) return CURL_REQUEST_FAILED;
            }
            return result;
        }

        /** \brief POST запрос
         *
         * Данный метод нужен для внутреннего использования
         * \param url URL сообщения
         * \param body Тело сообщения
         * \param http_headers Заголовки
         * \param response Ответ
         * \param is_use_cookie Использовать cookie файлы
         * \param is_clear_cookie Очистить cookie
         * \param timeout Время ожидания ответа
         * \return код ошибки
         */
        int post_request(
                const std::string &url,
                const std::string &body,
                struct curl_slist *http_headers,
                std::string &response,
                const bool is_use_cookie = true,
                const bool is_clear_cookie = false,
                const int timeout = POST_TIME_OUT) {
            std::map<std::string,std::string> headers;
            std::string buffer;
            CURL *curl = init_curl(
                url,
                body,
                buffer,
                http_headers,
                timeout,
                binance_writer,
                binance_header_callback,
                &headers,
                is_use_cookie,
                is_clear_cookie,
                TypesRequest::REQ_POST);

            if(curl == NULL) return CURL_CANNOT_BE_INIT;
            int err = process_server_response(curl, headers, buffer, response);
            curl_easy_cleanup(curl);
            return err;
        }

        /** \brief GET запрос
         *
         * Данный метод нужен для внутреннего использования
         * \param url URL сообщения
         * \param body Тело сообщения
         * \param http_headers Заголовки
         * \param response Ответ
         * \param is_clear_cookie Очистить cookie
         * \param timeout Время ожидания ответа
         * \return код ошибки
         */
        int get_request(
                const std::string &url,
                const std::string &body,
                struct curl_slist *http_headers,
                std::string &response,
                const bool is_use_cookie = true,
                const bool is_clear_cookie = false,
                const int timeout = POST_TIME_OUT) {
            //int content_encoding = 0;   // Тип кодирования сообщения
            std::map<std::string,std::string> headers;
            std::string buffer;
            CURL *curl = init_curl(
                url,
                body,
                buffer,
                http_headers,
                timeout,
                binance_writer,
                binance_header_callback,
                &headers,
                is_use_cookie,
                is_clear_cookie,
                TypesRequest::REQ_GET);

            if(curl == NULL) return CURL_CANNOT_BE_INIT;
            int err = process_server_response(curl, headers, buffer, response);
            curl_easy_cleanup(curl);
            return err;
        }

        std::string to_lower_case(const std::string &s){
            std::string temp = s;
            std::transform(temp.begin(), temp.end(), temp.begin(), [](char ch) {
                return std::use_facet<std::ctype<char>>(std::locale()).tolower(ch);
            });
            return temp;
        }

        const std::map<uint32_t, std::string> index_interval_to_str = {
            {1,"1m"},{3,"3m"},{5,"5m"},{15,"15m"},{30,"30m"},
            {60,"1h"},{120,"2h"},{240,"4h"},{360,"6h"},{480,"8h"},{720,"12h"},
            {1440,"1d"},{4320,"3d"},{10080,"1w"},{43200,"1M"}
        }; /**< Преобразование индекса периода в строку */

        void parse_history(
                std::vector<xquotes_common::Candle> &candles,
                std::string &response) {
            try {
                json j = json::parse(response);
                const size_t size_data = j.size();
                for(size_t i = 0; i < size_data; ++i) {
                    json j_canlde = j[i];
                    const xtime::timestamp_t open_timestamp = ((xtime::timestamp_t)j_canlde[0]) / 1000;
                    const double open = std::atof(std::string(j_canlde[1]).c_str());
                    const double high = std::atof(std::string(j_canlde[2]).c_str());
                    const double low = std::atof(std::string(j_canlde[3]).c_str());
                    const double close = std::atof(std::string(j_canlde[4]).c_str());
                    const double volume = std::atof(std::string(j_canlde[5]).c_str());
                    candles.push_back(xquotes_common::Candle(open,high,low,close,volume,open_timestamp));
                }
            } catch(...) {}
        }

        void parse_history(
                std::map<xtime::timestamp_t, xquotes_common::Candle> &candles,
                std::string &response) {
            try {
                json j = json::parse(response);
                const size_t size_data = j.size();
                for(size_t i = 0; i < size_data; ++i) {
                    json j_canlde = j[i];
                    const xtime::timestamp_t open_timestamp = ((xtime::timestamp_t)j_canlde[0]) / 1000;
                    const double open = std::atof(std::string(j_canlde[1]).c_str());
                    const double high = std::atof(std::string(j_canlde[2]).c_str());
                    const double low = std::atof(std::string(j_canlde[3]).c_str());
                    const double close = std::atof(std::string(j_canlde[4]).c_str());
                    const double volume = std::atof(std::string(j_canlde[5]).c_str());
                    candles[open_timestamp] = xquotes_common::Candle(open,high,low,close,volume,open_timestamp);
                }
            } catch(...) {}
        }

        void parse_exchange_info(std::string &response) {
            try {
                json j = json::parse(response);
                json j_rate_limits = j["rateLimits"];
                /* парсим ограничения скорости */
                for(size_t i =0; i < j_rate_limits.size(); ++i) {
                    if(j_rate_limits[i]["rateLimitType"] == "REQUEST_WEIGHT") {
                        request_limit = j_rate_limits[i]["limit"];
                    } else
                    if(j_rate_limits[i]["rateLimitType"] == "ORDERS") {

                    }
                }
                /* парсим параметры символов */
                json j_symbols = j["symbols"];
                for(size_t i =0; i < j_symbols.size(); ++i) {
                    const std::string symbol = j_symbols[i]["symbol"];
                    const uint32_t precision = j_symbols[i]["pricePrecision"];
                    bool is_active = false;
                    if(j_symbols[i]["status"] == "TRADING") is_active = true;
                    std::lock_guard<std::mutex> lock(symbols_spec_mutex);
                    symbols_spec[symbol].is_active = is_active;
                    symbols_spec[symbol].precision = precision;
                }
            } catch(...) {
                std::cout << "exchange info parser error" << std::endl;
            }
        }


        int get_request_none_security(std::string &response, const std::string &url, const uint64_t weight = 1) {
            int err = 0;
            const std::string body;
            check_request_limit(weight);
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json"});
            return get_request(url, body, http_headers.get(), response, false, false);
        }

        int post_request

    public:

        /** \brief Установить демо счет
         *
         * Данный метод влияет на выбор конечной точки подключения, а также
         * влияет на то, какое API будет использовано - для теста или для релаьной торговли
         * \param demo Флаг демо счета. По умолчанию true
         */
        void set_demo(const bool demo = true) {
            if(demo) point = "https://testnet.binancefuture.com";
            else point = "https://api.binance.com";
        }

        /** \brief Проверить соединение с сервером
         * \return Вернет true, если связь с сервером есть
         */
        bool ping() {
            std::string url(point);
            if(is_demo) url += "/fapi/v1/ping";
            else url += "/api/v3/ping";
            std::string response;
            int err = get_request_none_security(response, url);
            if(err != OK) return false;
            if(response == "{}") return true;
            return false;
        }


        /** \brief Получить текущие правила биржевой торговли и символьной информация
         *
         * Данные правил торговли и символьной информации сохраняются внутри класса,
         * поэтому данный метод не возвращает никаких значений, кроме кода ошибки
         * \return Код ошибки
         */
        int get_exchange_info() {
            std::string url(point);
            if(is_demo) url += "/fapi/v1/exchangeInfo";
            else url += "/api/v3/exchangeInfo";
            std::string response;
            int err = get_request_none_security(response, url);
            if(err != OK) return err;
            parse_exchange_info(response);
            return OK;
        }

        /** \brief Получить исторические данные
         *
         * \param candles Массив баров
         * \param symbol Имя символа
         * \param period Период
         * \param limit Ограничение количества баров
         * \return Код ошибки
         */
        int get_historical_data(
                std::vector<xquotes_common::Candle> &candles,
                const std::string &symbol,
                const uint32_t period,
                const uint32_t limit) {
            auto it = index_interval_to_str.find(period);
            if(it == index_interval_to_str.end()) return DATA_NOT_AVAILABLE;
            std::string url_klines(point);
            if(is_demo) url_klines += "/fapi/v1/klines?";
            else url_klines += "/api/v3/klines?";
            url_klines += "symbol=";
            url_klines += to_lower_case(symbol);
            url_klines += "&interval=";
            url_klines += it->second;
            url_klines += "&limit=";
            url_klines += std::to_string(limit);
            std::string response;
            int err = get_request_none_security(response, url_klines);
            if(err != OK) return err;
            parse_history(candles, response);
            return OK;
        }

        /** \brief Получить исторические данные
         *
         * \param candles Массив баров
         * \param symbol Имя символа
         * \param period Период
         * \param start_date Дата начала загрузки
         * \param stop_date Дата окончания загрузки
         * \param limit Ограничение количества баров
         * \return Код ошибки
         */
        int get_historical_data(
                std::vector<xquotes_common::Candle> &candles,
                const std::string &symbol,
                const uint32_t period,
                const xtime::timestamp_t start_date,
                const xtime::timestamp_t stop_date,
                const uint32_t limit = 1500) {
            auto it = index_interval_to_str.find(period);
            if(it == index_interval_to_str.end()) return DATA_NOT_AVAILABLE;
            std::string url_klines(point);
            if(is_demo) url_klines += "/fapi/v1/klines?";
            else url_klines += "/api/v3/klines?";
            url_klines += "symbol=";
            url_klines += to_lower_case(symbol);
            url_klines += "&interval=";
            url_klines += it->second;
            url_klines += "&startTime=";
            url_klines += std::to_string(xtime::get_first_timestamp_minute(start_date)*1000);
            url_klines += "&endTime=";
            url_klines += std::to_string(xtime::get_first_timestamp_minute(stop_date)*1000);
            url_klines += "&limit=";
            url_klines += std::to_string(limit);
            std::string response;
            int err = get_request_none_security(response, url_klines);
            if(err != OK) return err;
            parse_history(candles, response);
            return OK;
        }

        /** \brief Установить тип хеджирования
         * \param type Тип хеджирования (TypesPositionMode::Hedge_Mode или TypesPositionMode::One_way_Mode)
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int change_position_mode(const TypesPositionMode type) {
            int err = 0;
            if((err = get_position_mode(position_mode)) == OK) {
                if(position_mode == type) return OK;
            }
            std::string query_string;
            query_string += "dualSidePosition=";
            if(type == TypesPositionMode::Hedge_Mode) query_string += "true";
            else if(type == TypesPositionMode::One_way_Mode) query_string += "false";
            query_string += "&recvWindow=60000"; // 5000
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));

            std::string url(point);
            url += "/fapi/v1/positionSide/dual?";
            url += query_string;
            url += "&signature=";
            url += signature;

            /* создаем заголовки */
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});

            const std::string body;
            std::string response;
            check_request_limit();
            if((err = post_request(url, body, http_headers.get(), response, false, false)) != OK) {
                return err;
            }
            try {
                json j = json::parse(response);
                if(j["code"] != 200) {
                    return INVALID_PARAMETER;
                } else
                if(j["msg"] != "success") {
                    return INVALID_PARAMETER;
                }
                position_mode = type;
            } catch(...) {}

            //std::cout << "response: " << response << std::endl;
            return OK;
        }

        /** \brief Получить тип хеджирования
         * \param type Тип хеджирования (в случае успеза будет равен TypesPositionMode::Hedge_Mode или TypesPositionMode::One_way_Mode)
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int get_position_mode(TypesPositionMode &type) {
            std::string query_string;
            query_string += "recvWindow=5000"; // 5000
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));

            std::string url(point);
            url += "/fapi/v1/positionSide/dual?";
            url += query_string;
            url += "&signature=";
            url += signature;

            /* создаем заголовки */
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});

            const std::string body;
            std::string response;
            int err = 0;
            check_request_limit(30);
            if((err = get_request(url, body, http_headers.get(), response, false, false)) != OK) {
                //deinit_http_headers(http_headers);
                position_mode = TypesPositionMode::None;
                return err;
            }

            try {
                json j = json::parse(response);
                if(j.find("dualSidePosition") == j.end()) {
                    position_mode = TypesPositionMode::None;
                    return DATA_NOT_AVAILABLE;
                }
                if(j["dualSidePosition"]) type = TypesPositionMode::Hedge_Mode;
                else type = TypesPositionMode::One_way_Mode;
                position_mode = type;
            } catch(...) {
                position_mode = TypesPositionMode::None;
                return DATA_NOT_AVAILABLE;
            }
            return OK;
        }

        inline TypesPositionMode get_position_mode() {
            return position_mode;
        }

        int start_user_data_stream(std::string &listen_key) {
            std::string query_string;
            query_string += "recvWindow=5000"; // 5000
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));

            std::string url(point);
            url += "/fapi/v1/listenKey?";
            url += query_string;
            url += "&signature=";
            url += signature;

            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});

            const std::string body;
            std::string response;
            int err = 0;
            check_request_limit();
            if((err = post_request(url, body, http_headers.get(), response, false, false)) != OK) {
                return err;
            }

            try {
                json j = json::parse(response);
                if(j.find("listenKey") == j.end()) {
                    return DATA_NOT_AVAILABLE;
                }
                listen_key = j["listenKey"];
            } catch(...) {
                return DATA_NOT_AVAILABLE;
            }
            return OK;
        }

        int get_account_information() {
            std::string query_string;
            query_string += "recvWindow=5000";
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));

            std::string url(point);
            url += "/fapi/v1/account?";
            url += query_string;
            url += "&signature=";
            url += signature;

            /* создаем заголовки */
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});

            const std::string body;
            std::string response;
            int err = 0;
            check_request_limit();
            if((err = get_request(url, body, http_headers.get(), response, false, false)) != OK) {
                return err;
            }

            try {
                json j = json::parse(response);
                std::cout << "j: " << j.dump(4) << std::endl;
            } catch(...) {
                return DATA_NOT_AVAILABLE;
            }
            return OK;
        }

        /** \brief Автоматическая отмена всех открытых ордеров
         * \param symbol Торговый символ
         * \param countdown_time
         * \return Код ошибки
         */
        int auto_cancel_all_open_orders(
                const std::string &symbol,
                const uint64_t countdown_time) {
            std::string query_string;
            query_string += "symbol=";
            query_string += symbol;
            query_string += "&countdownTime=";
            query_string += std::to_string(countdown_time);
            query_string += "&recvWindow=5000";
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));
            std::string url(point);
            url += "/fapi/v1/countdownCancelAll?";
            url += query_string;
            url += "&signature=";
            url += signature;

            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});

            const std::string body;
            std::string response;
            int err = 0;

            check_request_limit(10);
            if((err = post_request(url, body, http_headers.get(), response, false, false)) != OK) {
                return err;
            }
            try {
                json j = json::parse(response);
                if (j["symbol"] == symbol &&
                    j["countdownTime"] == std::to_string(countdown_time)) return OK;
            } catch(...) {}
            return INVALID_PARAMETER;
        }

        /** \brief Открыть маркет ордер
         * \param symbol Торговый символ
         * \param new_client_order_id Уникальный номер сделки
         * \param side Направление сделки
         * \param position_side Направление ордера
         * \param position_mode Режим хеджирования
         * \param quantity Размер ордера
         * \param recv_window Время ожидания ответа, в мс.
         * \return Код ошибки
         */
        int open_market_order(
                const std::string &symbol,
                const std::string &new_client_order_id,
                const TypesSide side,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const double quantity,
                const uint64_t recv_window = 15000) {
            std::string query_string;
            query_string += "symbol=";
            query_string += symbol;
            if(side == TypesSide::BUY) query_string += "&side=BUY";
            else if(side == TypesSide::SELL) query_string += "&side=SELL";
            else return INVALID_PARAMETER;
            if(position_mode == TypesPositionMode::Hedge_Mode) {
                if(position_side == TypesPositionSide::LONG) query_string += "&positionSide=LONG";
                else if(position_side == TypesPositionSide::SHORT) query_string += "&positionSide=SHORT";
                else return INVALID_PARAMETER;
            } else if(position_mode == TypesPositionMode::One_way_Mode) {
                query_string += "&positionSide=BOTH";
            } else return INVALID_PARAMETER;
            query_string += "&quantity=";
            query_string += std::to_string(quantity);
            query_string += "&type=MARKET";
            if(new_client_order_id.size() > 0) {
                query_string += "&newClientOrderId=";
                query_string += new_client_order_id;
            }
            query_string += "&recvWindow=";
            query_string += std::to_string(recv_window);
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));
            std::string url(point);
            url += "/fapi/v1/order?";
            url += query_string;
            url += "&signature=";
            url += signature;

            /* создаем заголовки */
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});

            const std::string body;
            std::string response;
            int err = 0;

            check_request_limit();
            if((err = post_request(url, body, http_headers.get(), response, false, false)) != OK) {
                return err;
            }
            std::cout << "-open_market_order-0"  << std::endl;
            try {
                json j = json::parse(response);
                std::cout << "json::parse(response) " << j.dump(4) << std::endl;
                if(j["status"] == "NEW") {
                    std::cout << "-open_market_order-1"  << std::endl;
                    return OK;
                }
            } catch(...) {
                std::cout << "-open_market_order-2"  << std::endl;
                return DATA_NOT_AVAILABLE;
            }
            std::cout << "-open_market_order-3"  << std::endl;
            return OK;
        }


        /** \brief Открыть маркет ордер
         * \param symbol Торговый символ
         * \param new_client_order_id Уникальный номер сделки
         * \param order_type Тип ордера
         * \param position_side Направление ордера
         * \param position_mode Режим хеджирования
         * \param quantity Размер ордера
         * \param stop_price Цена срабатывания
         * \param recv_window Время ожидания ответа, в мс.
         * \return Код ошибки
         */
        int open_stop_market_order(
                const std::string &symbol,
                const std::string &new_client_order_id,
                const TypesOrder order_type,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const double quantity,
                const double stop_price,
                const uint64_t recv_window = 5000) {
            std::cout << "-open_market_order-5"  << std::endl;
            std::string query_string;
            query_string += "symbol=";
            query_string += symbol;
            if(position_side == TypesPositionSide::LONG) {
                query_string += "&side=SELL";
                query_string += "&positionSide=LONG";
            } else if(position_side == TypesPositionSide::SHORT) {
                query_string += "&side=BUY";
                query_string += "&positionSide=SHORT";
            } else return INVALID_PARAMETER;
            std::cout << "-open_market_order-6"  << std::endl;
            query_string += "&quantity=";
            query_string += std::to_string(quantity);
            query_string += "&stopPrice=";
            query_string += std::to_string(stop_price);
            if(order_type == TypesOrder::TAKE_PROFIT_MARKET) {
                query_string += "&type=TAKE_PROFIT_MARKET";
            } else if(order_type == TypesOrder::STOP_MARKET) {
                query_string += "&type=STOP_MARKET";
            } else return INVALID_PARAMETER;
            query_string += "&workingType=CONTRACT_PRICE";
            std::cout << "-open_market_order-7"  << std::endl;

            if(new_client_order_id.size() > 0) {
                query_string += "&newClientOrderId=";
                query_string += new_client_order_id;
            }
            query_string += "&recvWindow=";
            query_string += std::to_string(recv_window);
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));
            std::cout << "-open_market_order-8"  << std::endl;

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));
            std::string url(point);
            url += "/fapi/v1/order?";
            url += query_string;
            url += "&signature=";
            url += signature;

            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});

            std::string response;
            int err = 0;
            check_request_limit();
            if((err = post_request(url, std::string(), http_headers.get(), response, false, false)) != OK) {
                std::cout << "-open_market_order-8-err"  << std::endl;
                std::cout << "response "  << response << std::endl;
                return err;
            }
            std::cout << "-open_market_order-9"  << std::endl;
            try {
                json j = json::parse(response);
                std::cout << "-open_market_order-10"  << std::endl;
                if(j["status"] == "NEW") return OK;
                std::cout << j.dump(4) << std::endl;
            } catch(...) {
                std::cout << "-open_market_order-11"  << std::endl;
            }
            std::cout << "-open_market_order-12"  << std::endl;
            return DATA_NOT_AVAILABLE;
        }


        std::string urlEncode(std::string str){
            std::string new_str = "";
            char c;
            int ic;
            const char* chars = str.c_str();
            char bufHex[10];
            int len = strlen(chars);

            for(int i=0;i<len;i++){
                c = chars[i];
                ic = c;
                // uncomment this if you want to encode spaces with +
                /*if (c==' ') new_str += '+';
                else */if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') new_str += c;
                else {
                    sprintf(bufHex,"%X",c);
                    if(ic < 16)
                        new_str += "%0";
                    else
                        new_str += "%";
                    new_str += bufHex;
                }
            }
            return new_str;
         }

        int open_order(
                const std::string &symbol,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const double quantity,
                const double take_profit,
                const double stop_loss,
                const uint64_t expiration) {
            std::array<std::string,3> order_ids = {get_uuid(), get_uuid(), get_uuid()};
            int err1 = 0, err2 = 0, err3 = 0;
            TypesSide side = position_side == TypesPositionSide::LONG ? TypesSide::BUY : position_side == TypesPositionSide::SHORT ? TypesSide::SELL : TypesSide::NONE;

            if((err1 = open_market_order(symbol, order_ids[0], side, position_side, position_mode, quantity)) != OK) return DATA_NOT_AVAILABLE;
            std::cout << "-open_market_order-4"  << std::endl;
            err2 = open_stop_market_order(symbol, order_ids[1], binance_api::TypesOrder::TAKE_PROFIT_MARKET, position_side, position_mode, quantity, take_profit, 10000);
            err3 = open_stop_market_order(symbol, order_ids[2], binance_api::TypesOrder::STOP_MARKET, position_side, position_mode, quantity, stop_loss, 10000);
            // добавить обработку
            // -2021 ORDER_WOULD_IMMEDIATELY_TRIGGER

            if(err1 != OK || err2 != OK || err3 != OK) {
                /* возника ошибка при открытии ордера
                 * закрываем все позиции и выходим
                 */
                if(err1 == OK) {

                }
                if(err2 == OK || err3 == OK) {

                }
                return std::min(err1,std::min(err2,err3));
            }
            return OK;
        }
#if(0)
        /** \brief Открыть ордер
         * \param symbol Имя символа
         * \param side Направление сделки, TypesSide::BUY или TypesSide::SELL
         * \param position_side Направление позиции сделки, TypesPositionSide::LONG или TypesPositionSide::SHORT
         * \param type Тип ордера
         * \param quantity Размер ордера, с учетом
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int new_order(
                const std::string &symbol,
                const TypesSide side,
                const TypesPositionSide position_side,
                const TypesOrder type,
                const TypesTimeInForce time_in_force,
                const double quantity,
                const bool reduce_only,
                const double price,
                const std::string &new_client_order_id,
                const double stop_price,
                const double close_position,
                const double activation_price) {
            if (type == TypesOrder::LIMIT &&
                (time_in_force == TypesTimeInForce::NONE ||
                 quantity == 0 ||
                 price == 0)) return INVALID_PARAMETER;
            if (type == TypesOrder::MARKET && quantity == 0) return INVALID_PARAMETER;
            if (type == TypesOrder::STOP &&
                (stop_price = 0 ||
                 quantity == 0 ||
                 price == 0 ||
                 time_in_force != TypesTimeInForce::NONE)) return INVALID_PARAMETER;
            if (type == TypesOrder::STOP_MARKET && stop_price == 0) return INVALID_PARAMETER;
            if (type == TypesOrder::TRAILING_STOP_MARKET && callback_rate == 0) return INVALID_PARAMETER

            std::string query_string;
            query_string += "symbol=";
            query_string += symbol;
            if(side == TypesSide::BUY) query_string += "&side=BUY";
            else if(side == TypesSide::SELL) query_string += "&side=SELL";
            else return INVALID_PARAMETER;

            if(type == TypesPositionSide::LONG) query_string += "&positionSide=LONG";
            else if(type == TypesPositionSide::SHORT) query_string += "&positionSide=SHORT";
            else return INVALID_PARAMETER;

            if(type == TypesOrder::LIMIT) query_string += "&type=LIMIT";
            else if(type == TypesOrder::MARKET) query_string += "&type=MARKET";
            else if(type == TypesOrder::STOP) query_string += "&type=STOP";
            else if(type == TypesOrder::STOP_MARKET) query_string += "&type=STOP_MARKET";
            else if(type == TypesOrder::TRAILING_STOP_MARKET) query_string += "&type=TRAILING_STOP_MARKET";
            else return INVALID_PARAMETER;

            if(time_in_force != TypesTimeInForce::NONE && type != TypesOrder::STOP_LIMIT) {
                if(time_in_force == TypesTimeInForce::GTC) query_string += "&timeInForce=GTC";
                else if(time_in_force == TypesTimeInForce::IOC) query_string += "&timeInForce=IOC";
                else if(time_in_force == TypesTimeInForce::FOK) query_string += "&timeInForce=FOK";
            }

            if(type == TypesOrder::LIMIT) query_string += "&type=LIMIT";
            else if(type == TypesOrder::MARKET) query_string += "&type=MARKET";
            else if(type == TypesOrder::STOP_LIMIT) query_string += "&type=STOP";
            else if(type == TypesOrder::STOP_MARKET) query_string += "&type=STOP_MARKET";
            else if(type == TypesOrder::TRAILING_STOP_MARKET) query_string += "&type=TRAILING_STOP_MARKET";
            else return INVALID_PARAMETER;



            if(type == TypesPositionMode::Hedge_Mode) query_string += "true";
            else if(type == TypesPositionMode::One_way_Mode) query_string += "false";
            query_string += "&recvWindow=60000"; // 5000
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(xtime::get_ftimestamp() * 1000));

            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));

            std::string url(point);
            url += "/fapi/v1/positionSide/dual?";
            url += query_string;
            url += "&signature=";
            url += signature;

            /* создаем заголовки */
            const std::string x_mbx_apikey("X-MBX-APIKEY: " + api_key);
            struct curl_slist *http_headers = nullptr;
            http_headers = curl_slist_append(http_headers, "Content-Type: application/json");
            http_headers = curl_slist_append(http_headers, x_mbx_apikey.c_str());

            const std::string body;
            std::string response;
            int err = 0;
            check_request_limit();
            if((err = post_request(url, body, http_headers, response, false, false)) != OK) {
                std::cout << "error response: " << response << std::endl;
                deinit_http_headers(http_headers);
                return err;
            }
            deinit_http_headers(http_headers);
            try {
                json j = json::parse(response);
                if(j["code"] != 200) {
                    return INVALID_PARAMETER;
                } else
                if(j["msg"] != "success") {
                    return INVALID_PARAMETER;
                }
            } catch(...) {}

            //std::cout << "response: " << response << std::endl;
            return OK;
        }
#endif

        /** \brief Получить список имен символов/валютных пар
         * \return список имен символов/валютных пар
         */
        std::vector<std::string> get_symbol_list() {
            std::vector<std::string> symbol_list(symbols_spec.size());
            uint32_t index = 0;
            std::lock_guard<std::mutex> lock(symbols_spec_mutex);
            for(auto &symbol : symbols_spec) {
                symbol_list[index] = symbol.first;
                ++index;
            }
            return symbol_list;
        }

        /** \brief Проверить наличие символа у брокера
         * \param symbol Имя символа
         * \return Вернет true, если символ есть у брокера
         */
        inline bool check_symbol(const std::string &symbol) {
            std::lock_guard<std::mutex> lock(symbols_spec_mutex);
            if(symbols_spec.find(symbol) == symbols_spec.end()) return false;
            return true;
        }

        /** \brief Проверить наличие периода символа у брокера
         * \param period Период символа
         * \return Вернет true, если период есть у брокера
         */
        inline bool check_period(const uint32_t period) {
            if(index_interval_to_str.find(period) == index_interval_to_str.end()) return false;
            return true;
        }

        /** \brief Получить количество знаков после запятой
         * \param symbol Имя символа
         * \return количество знаков после запятой
         */
        inline uint32_t get_precision(const std::string &symbol) {
            std::lock_guard<std::mutex> lock(symbols_spec_mutex);
            auto it_spec = symbols_spec.find(symbol);
            if(it_spec == symbols_spec.end()) return 0;
            return it_spec->second.precision;
        }

        BinanceHttpApi(
                const std::string &user_api_key,
                const std::string &user_secret_key,
                const std::string &user_sert_file = "curl-ca-bundle.crt",
                const std::string &user_cookie_file = "binance-api.cookie") {
            api_key = user_api_key;
            secret_key = user_secret_key;
            sert_file = user_sert_file;
            cookie_file = user_cookie_file;
            curl_global_init(CURL_GLOBAL_ALL);
            int err = get_exchange_info();
            if(err != OK) {
                std::cerr << "Error: BinanceHttpApi(), get_exchange_info()" << std::endl;
            }
            if(api_key.size() == 0 || secret_key.size() == 0) {
                err = get_position_mode(position_mode);
                if(err != OK) {
                    position_mode = TypesPositionMode::None;
                    std::cerr << "Error: BinanceHttpApi(), get_position_mode(position_mode)" << std::endl;
                }
            } else {
                position_mode = TypesPositionMode::None;
            }
        };

        ~BinanceHttpApi() {
        }
    };
}
#endif // BINANCE_CPP_API_HTTP_HPP_INCLUDED
