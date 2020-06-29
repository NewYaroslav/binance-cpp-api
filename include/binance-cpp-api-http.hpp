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
        //std::string point = "https://fapi.binance.com";
        std::string point = "https://testnet.binancefuture.com";
        std::string api_key;
        std::string secret_key;
        std::string sert_file = "curl-ca-bundle.crt";   /**< Файл сертификата */
        std::string cookie_file = "binance-api.cookie"; /**< Файл cookie */

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

        void add_recv_window_and_timestamp(std::string &query_string, const uint64_t recv_window) {
            query_string += "&recvWindow=";
            query_string += std::to_string(recv_window);
            query_string += "&timestamp=";
            query_string += std::to_string((uint64_t)(get_server_ftimestamp() * 1000.0 + 1000.0));
        }

        int get_request_none_security(std::string &response, const std::string &url, const uint64_t weight = 1) {
            const std::string body;
            check_request_limit(weight);
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json"});
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
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});
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
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});
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
            HttpHeaders http_headers({"Accept-Encoding: gzip","Content-Type: application/json",std::string("X-MBX-APIKEY: " + api_key)});
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
            if(demo) point = "https://testnet.binancefuture.com";
            else point = "https://fapi.binance.com";
            is_demo = demo;
        }

        /** \brief Проверить соединение с сервером
         * \return Вернет true, если связь с сервером есть
         */
        bool ping() {
            std::string url(point);
            url += "/fapi/v1/ping";
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
            url += "/fapi/v1/exchangeInfo";
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
            std::string url(point);
            std::string response;
            url += "/fapi/v1/klines?";
            url += "symbol=";
            url += to_lower_case(symbol);
            url += "&interval=";
            url += it->second;
            url += "&limit=";
            url += std::to_string(limit);
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
        int get_historical_data(
                std::vector<xquotes_common::Candle> &candles,
                const std::string &symbol,
                const uint32_t period,
                const xtime::timestamp_t start_date,
                const xtime::timestamp_t stop_date,
                const uint32_t limit = 1500) {
            auto it = index_interval_to_str.find(period);
            if(it == index_interval_to_str.end()) return DATA_NOT_AVAILABLE;
            std::string url(point);
            std::string response;
            url += "/fapi/v1/klines?";
            url += "symbol=";
            url += to_lower_case(symbol);
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

        /** \brief Изменить начальное кредитное плечо
         * \param symbol Имя символа
         * \param leverage Кредитное плечо
         * \param recv_window Время ожидания реакции сервера
         * \return Код ошибки
         */
        int change_initial_leverage(
                const std::string &symbol,
                const uint32_t leverage,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;
            query_string += "&leverage=";
            query_string += std::to_string(leverage);
            url += "/fapi/v1/leverage?";
            int err = post_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
            try {
                json j = json::parse(response);
                if(j["symbol"] != symbol || j["leverage"] != leverage) {
                    return INVALID_PARAMETER;
                }
            } catch(...) {
                return INVALID_PARAMETER;
            }
            return OK;
        }

        /** \brief Изменить тип маржи
         * \param symbol Имя символа
         * \param margin_type Тип маржи
         * \param recv_window Время ожидания реакции сервера
         * \return Код ошибки
         */
        int change_margin_type(
                const std::string &symbol,
                const TypesMargin margin_type,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;
            if(margin_type == TypesMargin::ISOLATED) query_string += "&marginType=ISOLATED";
            else if(margin_type == TypesMargin::CROSSED) query_string += "&marginType=CROSSED";
            else return INVALID_PARAMETER;
            url += "/fapi/v1/marginType?";
            int err = post_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) {
                /* Нет необходимости изменения типа маржи */
                if(err == NO_NEED_TO_CHANGE_MARGIN_TYPE) return OK;
                return err;
            }
            try {
                json j = json::parse(response);
                if(j["code"] != 200) {
                    return INVALID_PARAMETER;
                }
            } catch(...) {
                return INVALID_PARAMETER;
            }
            return OK;
        }

        /** \brief Установить тип хеджирования
         * \param type Тип хеджирования (TypesPositionMode::Hedge_Mode или TypesPositionMode::One_way_Mode)
         * \param recv_window Время ожидания реакции сервера
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int change_position_mode(
                const TypesPositionMode type,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "dualSidePosition=";
            if(type == TypesPositionMode::Hedge_Mode) query_string += "true";
            else if(type == TypesPositionMode::One_way_Mode) query_string += "false";
            url += "/fapi/v1/positionSide/dual?";
            int err = post_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) {
                /* если ошибка говорит о том, что параметр уже был установлен, как надо */
                if(err == NO_NEED_TO_CHANGE_POSITION_SIDE) return OK;
                return err;
            }
            try {
                json j = json::parse(response);
                int code = j["code"];
                if(code != 200) {
                    return INVALID_PARAMETER;
                }
            } catch(...) {
                return INVALID_PARAMETER;
            }
            return OK;
        }

        /** \brief Получить тип хеджирования
         * \param position_mode Тип хеджирования (в случае успеза будет равен TypesPositionMode::Hedge_Mode или TypesPositionMode::One_way_Mode)
         * \param recv_window Время ожидания реакции сервера
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int get_position_mode(
                TypesPositionMode &position_mode,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            url += "/fapi/v1/positionSide/dual?";
            int err = get_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
            try {
                json j = json::parse(response);
                if(j.find("dualSidePosition") == j.end()) {
                    position_mode = TypesPositionMode::NONE;
                    return DATA_NOT_AVAILABLE;
                }
                if(j["dualSidePosition"]) position_mode = TypesPositionMode::Hedge_Mode;
                else position_mode = TypesPositionMode::One_way_Mode;
            } catch(...) {
                position_mode = TypesPositionMode::NONE;
                return DATA_NOT_AVAILABLE;
            }
            return OK;
        }

        /** \brief Начать поток пользовательских данных
         * \param listen_key Возвращаемое значение - ключ для вебсокета
         * \param recv_window Время ожидания реакции сервера
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int start_user_data_stream(
                std::string &listen_key,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            url += "/fapi/v1/listenKey?";
            int err = post_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
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

        /** \brief Продлить поток пользовательских данных, чтобы предотвратить тайм-аут.
         * \param recv_window Время ожидания реакции сервера
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int keepalive_user_data_stream(const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            url += "/fapi/v1/listenKey?";
            int err = put_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
            try {
                if(response == "{}") return OK;
            } catch(...) {
                return DATA_NOT_AVAILABLE;
            }
            return OK;
        }

        /** \brief Удалить поток пользовательских данных
         * \param recv_window Время ожидания реакции сервера
         * \return Код ошибки, вернет 0 если ошибок нет
         */
        int delete_user_data_stream(const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            url += "/fapi/v1/listenKey?";
            int err = delete_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
            try {
                if(response == "{}") return OK;
            } catch(...) {
                return DATA_NOT_AVAILABLE;
            }
            return OK;
        }

        int get_account_information(const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            url += "/fapi/v1/account?";
            int err = get_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
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
                const uint64_t countdown_time,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;
            query_string += "&countdownTime=";
            query_string += std::to_string(countdown_time);
            url += "/fapi/v1/countdownCancelAll?";
            int err = post_request_with_signature(response, query_string, url, recv_window, 10);
            if(err != OK) return err;
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
                const uint64_t recv_window = 60000,
                std::function<void(const xtime::ftimestamp_t timestamp)> callback = nullptr) {
            std::string url(point);
            std::string query_string;
            std::string response;
            //const bool is_open =
            //    ((position_side == TypesPositionSide::LONG && side == TypesSide::BUY) ||
            //    (position_side == TypesPositionSide::SHORT && side == TypesSide::SELL)) ? true : false;
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
            } else {
                return INVALID_PARAMETER;
            }
            query_string += "&quantity=";
            query_string += std::to_string(quantity);
            query_string += "&type=MARKET";
            if(new_client_order_id.size() > 0) {
                query_string += "&newClientOrderId=";
                query_string += new_client_order_id;
            };
            url += "/fapi/v1/order?";
            int err = post_request_with_signature(response, query_string, url, recv_window, 10);
            //std::cout << "response: " << response << std::endl;
            if(err != OK) {
                return err;
            }
            try {
                json j = json::parse(response);
                if(j["status"] == "NEW") {
                    xtime::ftimestamp_t timestamp = (double)((uint64_t)j["updateTime"]) / 1000.0;
                    if(callback != nullptr) callback(timestamp);
                    return OK;
                }
            } catch(...) {
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        /** \brief Открыть маркет ордер
         * \param symbol Торговый символ
         * \param new_client_order_id Уникальный номер сделки
         * \param order_type Тип ордера
         * \param side Направление сделки
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
                const TypesSide side,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const double quantity,
                const double stop_price,
                const bool close_position = false,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;

            if(position_side != TypesPositionSide::BOTH &&
                position_mode == TypesPositionMode::One_way_Mode) return INVALID_PARAMETER;

            if(side == TypesSide::SELL) {
                query_string += "&side=SELL";
            } else if(side == TypesSide::BUY) {
                query_string += "&side=BUY";
            } else return INVALID_PARAMETER;

            if(position_side == TypesPositionSide::LONG) {
                query_string += "&positionSide=LONG";
            } else
            if(position_side == TypesPositionSide::SHORT) {
                query_string += "&positionSide=SHORT";
            } else
            if(position_side == TypesPositionSide::BOTH) {
                query_string += "&positionSide=BOTH";
                if(!close_position) query_string += "&reduceOnly=true";
            } else return INVALID_PARAMETER;
            if(!close_position) {
                query_string += "&quantity=";
                query_string += std::to_string(quantity);
            }
            query_string += "&stopPrice=";
            query_string += std::to_string(stop_price);
            if(order_type == TypesOrder::TAKE_PROFIT_MARKET) {
                query_string += "&type=TAKE_PROFIT_MARKET";
            } else if(order_type == TypesOrder::STOP_MARKET) {
                query_string += "&type=STOP_MARKET";
            } else return INVALID_PARAMETER;
            query_string += "&workingType=CONTRACT_PRICE";
            if(new_client_order_id.size() > 0) {
                query_string += "&newClientOrderId=";
                query_string += new_client_order_id;
            }
            if(close_position) {
                query_string += "&closePosition=true";
            } else {
                query_string += "&closePosition=false";
            }
            url += "/fapi/v1/order?";
            int err = post_request_with_signature(response, query_string, url, recv_window);
            std::cout << "response: " << response << std::endl;
            if(err != OK) return err;
            try {
                json j = json::parse(response);
                if(j["status"] == "NEW") return OK;
            } catch(...) {
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        /** \brief Отменить ордер
         * \param symbol Торговый символ
         * \param orig_client_order_id Уникальный номер сделки
         * \param recv_window Время ожидания ответа, в мс.
         * \return Код ошибки
         */
        int cancel_order(
                const std::string &symbol,
                const std::string &orig_client_order_id,
                const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;
            query_string += "&origClientOrderId=";
            query_string += orig_client_order_id;
            url += "/fapi/v1/order?";
            int err = delete_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
            try {
                json j = json::parse(response);
                if(j["status"] == "CANCELED") return OK;
            } catch(...) {
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        /** \brief Отменить все открытие ордера
         * \param symbol Торговый символ
         * \param recv_window Время ожидания ответа, в мс.
         * \return Код ошибки
         */
        int cancel_all_order(
                const std::string &symbol,
                const uint64_t recv_window = 60000,
                std::function<void(
                    const bool is_open,
                    const bool is_error,
                    const int err_code,
                    const std::string &response,
                    const xtime::ftimestamp_t timestamp)> callback = nullptr) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;
            url += "/fapi/v1/allOpenOrders?";
            int err = delete_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) return err;
            try {
                json j = json::parse(response);
                //std::cout << j.dump(4) << std::endl;
                if(j["code"] == 200) {
                    return OK;
                }
            } catch(...) {
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        int check_order_status(
                const std::string &symbol,
                const std::string &orig_client_order_id,
                const uint64_t recv_window = 60000,
                std::function<void(
                    const int err_code,
                    const std::string &response,
                    const TypesOrderStatus order_status)> callback = nullptr) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;
            query_string += "&origClientOrderId=";
            query_string += orig_client_order_id;
            url += "/fapi/v1/order?";
            int err = get_request_with_signature(response, query_string, url, recv_window);
            std::cout << "response: " << response << std::endl;
            if(err != OK) {
                if(callback != nullptr) callback(err, response, TypesOrderStatus::NONE);
                return err;
            }
            try {
                json j = json::parse(response);
                /*
                 * Order status (status):
                 * NEW
                 * PARTIALLY_FILLED
                 * FILLED
                 * CANCELED
                 * REJECTED
                 * EXPIRED
                 */
                std::string status = j["status"];
                if(status == "NEW") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::NEW);
                    return OK;
                } else
                if(status == "PARTIALLY_FILLED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::PARTIALLY_FILLED);
                    return OK;
                } else
                if(status == "FILLED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::FILLED);
                    return OK;
                } else
                if(status == "CANCELED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::CANCELED);
                    return OK;
                } else
                if(status == "REJECTED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::REJECTED);
                    return OK;
                } else
                if(status == "EXPIRED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::EXPIRED);
                    return OK;
                }
            } catch(...) {
                if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::NONE);
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        int get_open_order(
                const std::string &symbol,
                const std::string &orig_client_order_id,
                const uint64_t recv_window = 60000,
                std::function<void(
                    const int err_code,
                    const std::string &response,
                    const TypesOrderStatus order_status)> callback = nullptr) {
            std::string url(point);
            std::string query_string;
            std::string response;
            query_string += "symbol=";
            query_string += symbol;
            query_string += "&origClientOrderId=";
            query_string += orig_client_order_id;
            url += "/fapi/v1/openOrder?";
            int err = get_request_with_signature(response, query_string, url, recv_window);
            std::cout << "response: " << response << std::endl;
            if(err != OK) {
                if(callback != nullptr) callback(err, response, TypesOrderStatus::NONE);
                return err;
            }
            try {
                json j = json::parse(response);
                std::string client_order_id = j["clientOrderId"];
                std::string status = j["status"];
                if(status == "NEW") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::NEW);
                    return OK;
                } else
                if(status == "PARTIALLY_FILLED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::PARTIALLY_FILLED);
                    return OK;
                } else
                if(status == "FILLED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::FILLED);
                    return OK;
                } else
                if(status == "CANCELED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::CANCELED);
                    return OK;
                } else
                if(status == "REJECTED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::REJECTED);
                    return OK;
                } else
                if(status == "EXPIRED") {
                    if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::EXPIRED);
                    return OK;
                }
            } catch(...) {
                if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::NONE);
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        int get_open_orders(
                const std::string &symbol,
                const uint64_t recv_window = 60000,
                std::function<void(
                    const int err_code,
                    const std::string &response,
                    const TypesOrderStatus order_status)> callback = nullptr) {
            std::string url(point);
            std::string query_string;
            std::string response;
            if(symbol.size() != 0) {
                query_string += "symbol=";
                query_string += symbol;
            }
            url += "/fapi/v1/openOrders?";
            const uint64_t weight = symbol.size() == 0 ? 40 : 1;
            int err = get_request_with_signature(response, query_string, url, recv_window, weight);
            std::cout << "response: " << response << std::endl;
            if(err != OK) {
                if(callback != nullptr) callback(err, response, TypesOrderStatus::NONE);
                return err;
            }
            try {
                json j = json::parse(response);
                return OK;
            } catch(...) {
                if(callback != nullptr) callback(PARSER_ERROR, response, TypesOrderStatus::NONE);
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        /** \brief Получить состояния всех позиций
         * \param symbol Символ, по которому мы получаем позицию. Если указать пустую строку, получим все позиции по всем символам.
         * \param callback Функция обратного вызова, которая нам возвращает все позиции или ошибку
         * \param recv_window Время ожидания ответа, в мс.
         * \return Код состояния ошибки
         */
        int get_position_risk(
                const std::string &symbol,
                std::function<void(
                    const PositionSpec &position)> callback,
            const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            if(symbol.size() != 0) {
                query_string += "symbol=";
                query_string += symbol;
            }
            url += "/fapi/v2/positionRisk?";
            int err = get_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) {
                return err;
            }
            try {
                json j = json::parse(response);
                //std::cerr << j.dump(4) << std::endl;
                for(size_t i = 0; i < j.size(); ++i) {
                    PositionSpec position;
                    position.symbol = j[i]["symbol"];
                    position.position_amount = std::atof(std::string(j[i]["positionAmt"]).c_str());
                    position.position_side = TypesPositionSide::NONE;
                    std::string str_position_side = j[i]["positionSide"];
                    if(str_position_side == "BOTH") position.position_side = TypesPositionSide::BOTH;
                    else if(str_position_side == "LONG") position.position_side = TypesPositionSide::LONG;
                    else if(str_position_side == "SHORT") position.position_side = TypesPositionSide::SHORT;
                    callback(position);
                    //if(position.symbol == "BTCUSDT") {
                    //    std::cerr << j[i].dump(4) << std::endl;
                    //}
                }
                return OK;
            } catch(...) {
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
        }

        /** \brief Получить баланс
         * \param callback Функция обратного вызова, которая нам возвращает баланс
         * \param recv_window Время ожидания ответа, в мс.
         * \return Код состояния ошибки
         */
        int get_balance(
                std::function<void(
                    const BalanceSpec &balance)> callback,
            const uint64_t recv_window = 60000) {
            std::string url(point);
            std::string query_string;
            std::string response;
            url += "/fapi/v2/balance?";
            int err = get_request_with_signature(response, query_string, url, recv_window);
            if(err != OK) {
                return err;
            }
            try {
                json j = json::parse(response);
                //std::cerr << j.dump(4) << std::endl;
                for(size_t i = 0; i < j.size(); ++i) {
                    BalanceSpec balance;
                    balance.asset = j[i]["asset"];
                    balance.wallet_balance = std::atof(std::string(j[i]["balance"]).c_str());
                    balance.cross_wallet_balance = std::atof(std::string(j[i]["crossWalletBalance"]).c_str());
                    callback(balance);
                }
                return OK;
            }
            catch(const json::parse_error& e) {
                std::cerr << "binance_api::BinanceHttpApi.get_balance() parser error (json::parse_error), what: " << std::string(e.what()) << std::endl;
                return PARSER_ERROR;
            }
            catch(const json::out_of_range& e) {
                std::cerr << "binance_api::BinanceHttpApi.get_balance() parser error (json::out_of_range), what: " << std::string(e.what()) << std::endl;
                return PARSER_ERROR;
            }
            catch(const json::type_error& e) {
                std::cerr << "binance_api::BinanceHttpApi.get_balance() parser error (json::type_error), what: " << std::string(e.what()) << std::endl;
                return PARSER_ERROR;
            }
            catch(...) {
                std::cerr << "binance_api::BinanceHttpApi.get_balance() parser error" << std::endl;
                return PARSER_ERROR;
            }
            return DATA_NOT_AVAILABLE;
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
        };

        /** \brief Конструктор класса Binance Api для http запросов
         * \param user_api_key API ключ
         * \param user_secret_key Секретный ключ
         * \param user_demo Флаг демо счета
         * \param user_sert_file Файл сертификата
         * \param user_cookie_file Cookie файлы
         */
        BinanceHttpApi(
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
                std::cerr << "Error: BinanceHttpApi(), get_exchange_info()" << std::endl;
            }
        };

        /** \brief Конструктор класса Binance Api для http запросов
         * \param user_sert_file Файл сертификата
         * \param user_cookie_file Cookie файлы
         */
        BinanceHttpApi(
                const std::string &user_sert_file = "curl-ca-bundle.crt") {
            sert_file = user_sert_file;
            curl_global_init(CURL_GLOBAL_ALL);
            int err = get_exchange_info();
            if(err != OK) {
                std::cerr << "Error: BinanceHttpApi(), what: get_exchange_info(), code: " << err << std::endl;
            }
        };

        ~BinanceHttpApi() {
        }
    };
}
#endif // BINANCE_CPP_API_HTTP_HPP_INCLUDED
