/*
* binance-cpp-api - C ++ API client for binance
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
#ifndef BINANCE_CPP_SAPI_HTTP_HPP_INCLUDED
#define BINANCE_CPP_SAPI_HTTP_HPP_INCLUDED

#include "binance-cpp-api-common.hpp"
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

    /** \brief Класс API брокера Binance для спотовой торговли
     */
    class BinanceHttpSApi {
    private:
        //std::string point = "https://api.binance.com";
        std::string point = "https://testnet.binance.vision";
        std::string api_key;
        std::string secret_key;
        std::string sert_file = "curl-ca-bundle.crt";   /**< Файл сертификата */
        std::string cookie_file = "binance-sapi.cookie"; /**< Файл cookie */

        std::atomic<bool> is_demo = ATOMIC_VAR_INIT(true);  /**< Флаг демо-аккаунта */
        char error_buffer[CURL_ERROR_SIZE];
        static const int TIME_OUT = 60; /**< Время ожидания ответа сервера для разных запросов */

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

        std::atomic<xtime::ftimestamp_t> offset_timestamp = ATOMIC_VAR_INIT(0);

    public:

        /** \brief Получить метку времени сервера
         * \return Метка времени сервера
         */
        inline xtime::ftimestamp_t get_server_ftimestamp() {
            return  xtime::get_ftimestamp() + offset_timestamp;
        }

        /** \brief Установить смещение метки времени
         * \param offset Смещение метки времени
         */
        inline void set_server_offset_timestamp(const double offset) {
            offset_timestamp = offset;
        }

    private:

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

        //TypesPositionMode position_mode;                /**< Режим хеджирования позиции */
        //std::vector<PositionSpec> positions;

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
            if (type_req == TypesRequest::REQ_POST ||
                type_req == TypesRequest::REQ_PUT ||
                type_req == TypesRequest::REQ_DELETE)
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
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
                const int timeout = TIME_OUT) {
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

        /** \brief PUT запрос
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
        int put_request(
                const std::string &url,
                const std::string &body,
                struct curl_slist *http_headers,
                std::string &response,
                const bool is_use_cookie = true,
                const bool is_clear_cookie = false,
                const int timeout = TIME_OUT) {
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
                TypesRequest::REQ_PUT);

            if(curl == NULL) return CURL_CANNOT_BE_INIT;
            int err = process_server_response(curl, headers, buffer, response);
            curl_easy_cleanup(curl);
            return err;
        }

        /** \brief DELETE запрос
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
        int delete_request(
                const std::string &url,
                const std::string &body,
                struct curl_slist *http_headers,
                std::string &response,
                const bool is_use_cookie = true,
                const bool is_clear_cookie = false,
                const int timeout = TIME_OUT) {
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
                TypesRequest::REQ_DELETE);

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
                const int timeout = TIME_OUT) {
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

        std::string to_upper_case(const std::string &s){
            std::string temp = s;
            std::transform(temp.begin(), temp.end(), temp.begin(), [](char ch) {
                return std::use_facet<std::ctype<char>>(std::locale()).toupper(ch);
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
                    const uint32_t precision = j_symbols[i]["quoteAssetPrecision"];
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

        void add_recv_window_and_timestamp(std::string &query_string, const uint64_t recv_window) {
            query_string += "&recvWindow=";
            query_string += std::to_string(recv_window);
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(get_server_ftimestamp() * 1000.0 + 1000.0));
        }

        int get_request_none_security(std::string &response, const std::string &url, const uint64_t weight = 1) {
            const std::string body;
            check_request_limit(weight);
            HttpHeaders http_headers({
                "Accept-Encoding: gzip",
                "Content-Type: application/json"});
            int err = get_request(url, body, http_headers.get(), response, false, false);
            if(err != OK) {
                try {
                    json j = json::parse(response);
                    return (int)j["code"];
                } catch(...) {
                    return err;
                }
            }
            return err;
        }

        int post_request_with_signature(
                std::string &response,
                std::string &query_string,
                std::string &url,
                const uint64_t recv_window,
                const uint64_t weight = 1) {
            add_recv_window_and_timestamp(query_string, recv_window);
            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));
            url += query_string;
            url += "&signature=";
            url += signature;
            HttpHeaders http_headers({
                "Accept-Encoding: gzip",
                "Content-Type: application/json",
                std::string("X-MBX-APIKEY: " + api_key)});
            const std::string body;
            check_request_limit(weight);
            int err = post_request(url, body, http_headers.get(), response, false, false);
            if(err != OK) {
                try {
                    json j = json::parse(response);
                    return (int)j["code"];
                } catch(...) {
                    return err;
                }
            }
            return err;
        }

        int put_request_with_signature(
                std::string &response,
                std::string &query_string,
                std::string &url,
                const uint64_t recv_window,
                const uint64_t weight = 1) {
            add_recv_window_and_timestamp(query_string, recv_window);
            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));
            url += query_string;
            url += "&signature=";
            url += signature;
            HttpHeaders http_headers({
                "Accept-Encoding: gzip",
                "Content-Type: application/json",
                std::string("X-MBX-APIKEY: " + api_key)});
            const std::string body;
            check_request_limit(weight);
            int err = put_request(url, body, http_headers.get(), response, false, false);
            if(err != OK) {
                try {
                    json j = json::parse(response);
                    return (int)j["code"];
                } catch(...) {
                    return err;
                }
            }
            return err;
        }

        int get_request_with_signature(
                std::string &response,
                std::string &query_string,
                std::string &url,
                const uint64_t recv_window,
                const uint64_t weight = 1) {
            add_recv_window_and_timestamp(query_string, recv_window);
            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));
            url += query_string;
            url += "&signature=";
            url += signature;
            HttpHeaders http_headers({
                "Accept-Encoding: gzip",
                "Content-Type: application/json",
                std::string("X-MBX-APIKEY: " + api_key)});
            const std::string body;
            check_request_limit(weight);
            int err = get_request(url, body, http_headers.get(), response, false, false);
            if(err != OK) {
                try {
                    json j = json::parse(response);
                    return (int)j["code"];
                } catch(...) {
                    return err;
                }
            }
            return err;
        }

        int delete_request_with_signature(
                std::string &response,
                std::string &query_string,
                std::string &url,
                const uint64_t recv_window,
                const uint64_t weight = 1) {
            add_recv_window_and_timestamp(query_string, recv_window);
            std::string signature(hmac::get_hmac(secret_key, query_string, hmac::TypeHash::SHA256));
            url += query_string;
            url += "&signature=";
            url += signature;
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});
            const std::string body;
            check_request_limit(weight);
            int err = delete_request(url, body, http_headers.get(), response, false, false);
            if(err != OK) {
                try {
                    json j = json::parse(response);
                    return (int)j["code"];
                } catch(...) {
                    return err;
                }
            }
            return err;
        }

    public:

        /** \brief Установить демо счет
         *
         * Данный метод влияет на выбор конечной точки подключения, а также
         * влияет на то, какое API будет использовано - для теста или для релаьной торговли
         * \param demo Флаг демо счета. По умолчанию true
         */
        void set_demo(const bool demo = true) {
            if(demo) point = "https://testnet.binance.vision";
            else point = "https://api.binance.com";
            is_demo = demo;
        }

        /** \brief Проверить соединение с сервером
         * \return Вернет true, если связь с сервером есть
         */
        bool ping() {
            std::string url(point);
            url += "/api/v3/ping";
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
            std::string response;
            url += "/api/v3/exchangeInfo";
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
        int get_historical_data_single_request(
                std::vector<xquotes_common::Candle> &candles,
                const std::string &symbol,
                const uint32_t period,
                const uint32_t limit) {
            auto it = index_interval_to_str.find(period);
            if(it == index_interval_to_str.end()) return DATA_NOT_AVAILABLE;
            std::string url(point);
            std::string response;
            url += "/api/v3/klines?";
            url += "symbol=";
            url += to_upper_case(symbol);
            url += "&interval=";
            url += it->second;
            url += "&limit=";
            url += std::to_string(limit);
            std::cout << url << std::endl;
            int err = get_request_none_security(response, url);
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
        int get_historical_data_single_request(
                std::vector<xquotes_common::Candle> &candles,
                const std::string &symbol,
                const uint32_t period,
                const xtime::timestamp_t start_date,
                const xtime::timestamp_t stop_date,
                const uint32_t limit = 1000) {
            auto it = index_interval_to_str.find(period);
            if(it == index_interval_to_str.end()) return DATA_NOT_AVAILABLE;
            std::string url(point);
            std::string response;
            url += "/api/v3/klines?";
            url += "symbol=";
            url += to_upper_case(symbol);
            url += "&interval=";
            url += it->second;
            url += "&startTime=";
            url += std::to_string(xtime::get_first_timestamp_minute(start_date)*1000);
            url += "&endTime=";
            url += std::to_string(xtime::get_first_timestamp_minute(stop_date)*1000);
            url += "&limit=";
            url += std::to_string(limit);
            int err = get_request_none_security(response, url);
            if(err != OK) return err;
            parse_history(candles, response);
            return OK;
        }

        /** \brief Получить исторические данные
         * Данная функция может получать неограниченное количество баров
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
                const xtime::timestamp_t stop_date) {

            const uint64_t limit = 1000;
            const uint64_t candle_time = period * xtime::SECONDS_IN_MINUTE;
            const uint64_t step = candle_time * limit;
            xtime::timestamp_t start_timestamp = start_date;
            uint32_t attempt = 0;
            uint32_t bars =  0;
            while(true) {
                std::vector<xquotes_common::Candle> temp;
                xtime::timestamp_t stop_timestamp = start_timestamp + step - candle_time;
                if(stop_timestamp > stop_date) stop_timestamp = stop_date;
                int err = get_historical_data_single_request(temp, symbol, period, start_timestamp, stop_timestamp, limit);
                if(err != OK) {
                    if(attempt < 3) {
                        ++attempt;
                        const uint64_t DELAY = 5000;
                        std::this_thread::sleep_for(std::chrono::milliseconds(DELAY));
                        continue;
                    } else {
                        attempt = 0;
                    }
                }

                if(temp.size() > 0) {
                    candles.reserve(candles.size() + temp.size());
                    //std::copy(candles.begin(), candles.end(), std::back_inserter(temp));
                    candles.insert(candles.end(), temp.begin(), temp.end());
                    bars += temp.size();
                }

                if(stop_timestamp >= stop_date) break;
                start_timestamp += step;
                if(start_timestamp > stop_date) start_timestamp = stop_date;
            }
            return bars > 0 ? OK : DATA_NOT_AVAILABLE;
        }

        /** \brief Получить список имен символов/валютных пар
         * \return Список имен символов/валютных пар
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

        /** \brief Конструктор класса Binance Api для http запросов
         * \param user_api_key API ключ
         * \param user_secret_key Секретный ключ
         * \param user_sert_file Файл сертификата
         * \param user_cookie_file Cookie файлы
         */
        BinanceHttpSApi(
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
                std::cerr << "Error: BinanceHttpSApi(), get_exchange_info()" << std::endl;
            }
        };

        /** \brief Конструктор класса Binance Api для http запросов
         * \param user_api_key API ключ
         * \param user_secret_key Секретный ключ
         * \param user_demo Флаг демо счета
         * \param user_sert_file Файл сертификата
         * \param user_cookie_file Cookie файлы
         */
        BinanceHttpSApi(
                const std::string &user_api_key,
                const std::string &user_secret_key,
                const bool user_demo,
                const std::string &user_sert_file = "curl-ca-bundle.crt",
                const std::string &user_cookie_file = "binance-api.cookie") {
            set_demo(user_demo);
            api_key = user_api_key;
            secret_key = user_secret_key;
            sert_file = user_sert_file;
            cookie_file = user_cookie_file;
            curl_global_init(CURL_GLOBAL_ALL);
            int err = get_exchange_info();
            if(err != OK) {
                std::cerr << "Error: BinanceHttpSApi(), get_exchange_info()" << std::endl;
            }
        };

        /** \brief Конструктор класса Binance Api для http запросов
         * \param user_demo Флаг демо счета
         * \param user_sert_file Файл сертификата
         * \param user_cookie_file Cookie файлы
         */
        BinanceHttpSApi(
                const bool user_demo,
                const std::string &user_sert_file = "curl-ca-bundle.crt") {
            set_demo(user_demo);
            sert_file = user_sert_file;
            curl_global_init(CURL_GLOBAL_ALL);
            int err = get_exchange_info();
            if(err != OK) {
                std::cerr << "Error: BinanceHttpSApi(), what: get_exchange_info(), code: " << err << std::endl;
            }
        };

        /** \brief Конструктор класса Binance Api для http запросов
         * \param user_sert_file Файл сертификата
         * \param user_cookie_file Cookie файлы
         */
        BinanceHttpSApi(
                const std::string &user_sert_file = "curl-ca-bundle.crt") {
            sert_file = user_sert_file;
            curl_global_init(CURL_GLOBAL_ALL);
            int err = get_exchange_info();
            if(err != OK) {
                std::cerr << "Error: BinanceHttpSApi(), what: get_exchange_info(), code: " << err << std::endl;
            }
        };

        ~BinanceHttpSApi() {}
    };
}
#endif // BINANCE_CPP_SAPI_HTTP_HPP_INCLUDED
