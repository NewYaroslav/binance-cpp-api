#ifndef BINANCE_CPP_API_COMMON_HPP_INCLUDED
#define BINANCE_CPP_API_COMMON_HPP_INCLUDED

namespace binance_api {
    namespace common {
        /// Варианты состояния ошибок
        enum ErrorType {
            OK = 0,                             ///< Ошибки нет
            CURL_CANNOT_BE_INIT = -1,           ///< CURL не может быть инициализирован
            AUTHORIZATION_ERROR = -2,           ///< Ошибка авторизации
            CONTENT_ENCODING_NOT_SUPPORT = -3,  ///< Тип кодирования контента не поддерживается
            DECOMPRESSOR_ERROR = -4,            ///< Ошибка декомпрессии
            JSON_PARSER_ERROR = -5,             ///< Ошибка парсера JSON
            NO_ANSWER = -6,                     ///< Нет ответа
            INVALID_ARGUMENT = -7,              ///< Неверный аргумент метода класса
            STRANGE_PROGRAM_BEHAVIOR = -8,      ///< Странное поведение программы (т.е. такого не должно быть, очевидно проблема в коде)
            BETTING_QUEUE_IS_FULL = -9,         ///< Очередь ставок переполнена
            ERROR_RESPONSE = -10,               ///< Сервер брокера вернул ошибку
            NO_DATA_IN_RESPONSE = -11,
            ALERT_RESPONSE = -12,               ///< Сервер брокера вернул предупреждение
            DATA_NOT_AVAILABLE = -13,           ///< Данные не доступны
            PARSER_ERROR = -14,                 ///< Ошибка парсера ответа от сервера
            CURL_REQUEST_FAILED = -15,          ///< Ошибка запроса на сервер. Любой код статуса, который не равен 200, будет возвращать эту ошибку
            DDOS_GUARD_DETECTED = -16,          ///< Обнаружена защита от DDOS атаки. Эта ошибка может возникнуть при смене IP адреса
        };
    }
}

#endif // BINANCE-CPP-API-COMMON_HPP_INCLUDED
