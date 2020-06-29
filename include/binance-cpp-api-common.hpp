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
#ifndef BINANCE_CPP_API_COMMON_HPP_INCLUDED
#define BINANCE_CPP_API_COMMON_HPP_INCLUDED

#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "tools/base36.h"
#include "xtime.hpp"

namespace binance_api {
    namespace common {
        using json = nlohmann::json;

        /// Типы маржи
        enum class TypesMargin {
            NONE = 0,       /**< Нет сделки */
            ISOLATED = 1,
            CROSSED = 2,
        };

        /// Типы хеджирования
        enum class TypesPositionMode {
            NONE = 0,
            Hedge_Mode = 1,     /**< режим хеджирования */
            One_way_Mode = 2,   /**< односторонний режим */
        };

        /// Типы направления сделок
        enum class TypesSide {
            NONE = 0,   /**< Нет сделки */
            SELL = -1,  /**< Продажа */
            BUY = 1,    /**< Покупка */
        };

        /// Типы направления позиции
        enum class TypesPositionSide {
            NONE = 0,   /**< Нет позиции */
            SHORT = -1, /**< Продажа */
            LONG = 1,   /**< Покупка */
            BOTH = 2,
        };

        /// Типы состояний ордра
        enum class TypesOrderStatus {
            NONE = 0,                   /**< Нет типа */
            NEW = 1,                    /**< Новый ордер */
            PARTIALLY_FILLED = 2,
            FILLED = 3,
            CANCELED = 4,
            REJECTED = 5,
            EXPIRED = 6,
        };

        /// Типы сделок
        enum class TypesOrder {
            NONE = 0,                   /**< Нет типа */
            LIMIT = 1,                  /**< Лимитный ордер */
            MARKET = 2,                 /**< Рыночное исполнение */
            STOP = 3,                   /**< Стоп Лимит */
            TAKE_PROFIT = 4,
            STOP_MARKET = 5,            /**< Стоп Маркет */
            TAKE_PROFIT_MARKET = 6,
            TRAILING_STOP_MARKET = 7,   /**< Трейлинг-Стоп */
        };

        /// Типы времени в силе
        enum class TypesTimeInForce {
            NONE = 0,   /**< Нет типа */
            GTC = 1,    /**< Хорошо до отмены */
            IOC = 2,    /**< Немедленно или Отмена */
            FOK = 3,    /**< Заполнить или убить */
            GTX = 4,    /**< Хорошо до пересечения (Post Only) */
        };

        /// Варианты состояния ошибок
        enum ErrorType {
            OK = 0,                             ///< Ошибки нет
            CURL_CANNOT_BE_INIT = -1,           ///< CURL не может быть инициализирован
            CONTENT_ENCODING_NOT_SUPPORT = -2,  ///< Тип кодирования контента не поддерживается
            PARSER_ERROR = -3,                  ///< Ошибка парсера ответа от сервера
            JSON_PARSER_ERROR = -4,             ///< Ошибка парсера JSON
            NO_ANSWER = -5,                     ///< Нет ответа
            DATA_NOT_AVAILABLE = -6,            ///< Данные не доступны
            CURL_REQUEST_FAILED = -7,           ///< Ошибка запроса на сервер. Любой код статуса, который не равен 200, будет возвращать эту ошибку
            LIMITING_NUMBER_REQUESTS = -8,      ///< Нарушение ограничения скорости запроса.
            IP_BLOCKED = -9,                    ///< IP-адрес был автоматически заблокирован для продолжения отправки запросов после получения 429 кодов.
            WAF_LIMIT = -10,                    ///< нарушении лимита WAF (брандмауэр веб-приложений).
            NO_RESPONSE_WAITING_PERIOD = -11,
            INVALID_PARAMETER = -12,
            INVALID_TIMESTAMP = -1021,                  /**< Временная метка для этого запроса находится за пределами recvWindow или Временная метка для этого запроса была на 1000 мс раньше времени сервера. */
            NO_SUCH_ORDER = -2013,                      /**< Заказ не существует */
            ORDER_WOULD_IMMEDIATELY_TRIGGER = -2021,    /**< Заказ сразу сработает. */
            NO_NEED_TO_CHANGE_MARGIN_TYPE = -4046,      /**< Нет необходимости изменения типа маржи */
            NO_NEED_TO_CHANGE_POSITION_SIDE = -4059,    /**< Нет необходимости менять положение стороны. */
            POSITION_SIDE_NOT_MATCH = -4061,            /**< Сторона позиции ордера не соответствует настройке пользователя. */
            POSITION_SIDE_CHANGE_EXISTS_QUANTITY = -4068,/**< Сторона позиции не может быть изменена, если существует позиция */
        };

        /*
            HTTP Return Codes
            HTTP 4XX return codes are used for for malformed requests; the issue is on the sender's side.
            HTTP 403 return code is used when the WAF Limit (Web Application Firewall) has been violated.
            HTTP 429 return code is used when breaking a request rate limit.
            HTTP 418 return code is used when an IP has been auto-banned for continuing to send requests after receiving 429 codes.
            HTTP 5XX return codes are used for internal errors; the issue is on Binance's side.
            HTTP 503 return code is used when the API successfully sent the message but not get a response within the timeout period.
            It is important to NOT treat this as a failure operation; the execution status is UNKNOWN and could have been a success.
        */

        /** \brief параметры символов
         */
        class SymbolSpec {
        public:
            bool is_active = false;
            uint32_t precision = 0;

            SymbolSpec() {};
        };

        /** \brief Параметры позиции
         */
        class PositionSpec {
        public:
            std::string symbol;         /**< Символ */
            TypesPositionSide position_side = TypesPositionSide::NONE;
            double position_amount = 0; /**< Размер позиции (если 0, то позиции нет) */
            PositionSpec() {};
        };

        /** \brief Параметры сбаланса
         */
        class BalanceSpec {
        public:
            std::string asset;                  /**< Актив */
            double wallet_balance = 0;          /**< Баланс кошелька */
            double cross_wallet_balance = 0;    /**< Баланс кошелька */
            BalanceSpec() {};
            BalanceSpec(
                const std::string &_asset,
                const double _wallet_balance,
                const double _cross_wallet_balance) :
                asset(_asset),
                wallet_balance(_wallet_balance),
                cross_wallet_balance(_cross_wallet_balance) {
            };
        };

        /** \brief Открыть файл JSON
         *
         * Данная функция прочитает файл с JSON и запишет данные в JSON структуру
         * \param file_name Имя файла
         * \param auth_json Структура JSON с данными из файла
         * \return Вернет true в случае успешного завершения
         */
        bool open_json_file(const std::string &file_name, json &auth_json) {
            std::ifstream auth_file(file_name);
            if(!auth_file) {
                std::cerr << "open file " << file_name << " error" << std::endl;
                return false;
            }
            try {
                auth_file >> auth_json;
            }
            catch (json::parse_error &e) {
                std::cerr << "json parser error: " << std::string(e.what()) << std::endl;
                auth_file.close();
                return false;
            }
            catch (std::exception e) {
                std::cerr << "json parser error: " << std::string(e.what()) << std::endl;
                auth_file.close();
                return false;
            }
            catch(...) {
                std::cerr << "json parser error" << std::endl;
                auth_file.close();
                return false;
            }
            auth_file.close();
            return true;
        }

        /** \brief Обработать аргументы
         *
         * Данная функция обрабатывает аргументы от командной строки, возвращая
         * результат как пара ключ - значение.
         * \param argc количество аргуметов
         * \param argv вектор аргументов
         * \param f лябмда-функция для обработки аргументов командной строки
         * \return Вернет true если ошибок нет
         */
        bool process_arguments(
            const int argc,
            char **argv,
            std::function<void(
                const std::string &key,
                const std::string &value)> f) noexcept {
            if(argc <= 1) return false;
            bool is_error = true;
            for(int i = 1; i < argc; ++i) {
                std::string key = std::string(argv[i]);
                if(key.size() > 0 && (key[0] == '-' || key[0] == '/')) {
                    uint32_t delim_offset = 0;
                    if(key.size() > 2 && (key.substr(2) == "--") == 0) delim_offset = 1;
                    std::string value;
                    if((i + 1) < argc) value = std::string(argv[i + 1]);
                    is_error = false;
                    if(f != nullptr) f(key.substr(delim_offset), value);
                }
            }
            return !is_error;
        }

        class PrintThread: public std::ostringstream {
        private:
            static inline std::mutex _mutexPrint;

        public:
            PrintThread() = default;

            ~PrintThread() {
                std::lock_guard<std::mutex> guard(_mutexPrint);
                std::cout << this->str();
            }
        };

        inline std::string get_uuid() {
            uint64_t timestamp1000 = xtime::get_ftimestamp() * 1000.0 + 0.5;
            std::string temp(CBase36::encodeInt(timestamp1000));
            temp += CBase36::randomString(10,11);
            std::transform(temp.begin(), temp.end(),temp.begin(), ::toupper);
            return temp;
        }

        inline std::string get_uuid(const double ftimestamp) {
            uint64_t timestamp1000 = ftimestamp * 1000.0 + 0.5;
            std::string temp(CBase36::encodeInt(timestamp1000));
            temp += CBase36::randomString(10,11);
            std::transform(temp.begin(), temp.end(),temp.begin(), ::toupper);
            return temp;
        }

        std::string url_encode(std::string str){
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
    }
}

#endif // BINANCE-CPP-API-COMMON_HPP_INCLUDED
