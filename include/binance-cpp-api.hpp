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
#ifndef BINANCE_CPP_API_HPP_INCLUDED
#define BINANCE_CPP_API_HPP_INCLUDED

#include "binance-cpp-api-settings.hpp"
#include "binance-cpp-api-http.hpp"
#include "binance-cpp-api-websocket.hpp"
#include "named-pipe-server.hpp"
#include "tools\binance-cpp-api-mql-hst.hpp"

namespace binance_api {
    using json = nlohmann::json;
    using namespace common;

    class BinanceApi {
    private:
        std::shared_ptr<CandlestickStreams> timestamp_streams;      /**< Поток котировок для определения смещения метки времени */
        std::shared_ptr<CandlestickStreams> candlestick_streams;    /**< Поток котировок */
        std::shared_ptr<BinanceHttpApi> binance_http_api;
        std::shared_ptr<UserDataStreams> user_data_streams;         /**< Поток пользовательских данных */
        std::shared_ptr<SimpleNamedPipe::NamedPipeServer> pipe_server;
        std::vector<std::shared_ptr<binance_api::MqlHst>> mql_history;
        std::mutex mql_history_mutex;

        std::string listen_key;

        std::mutex request_future_mutex;
        std::vector<std::future<void>> request_future;
        std::atomic<bool> is_future_shutdown = ATOMIC_VAR_INIT(false);


        std::future<void> user_data_streams_future;
        std::atomic<bool> is_user_data_streams_future_shutdown = ATOMIC_VAR_INIT(false);

        std::atomic<bool> is_error = ATOMIC_VAR_INIT(false);

        /** \brief Очистить список запросов
         */
        void clear_request_future() {
            std::lock_guard<std::mutex> lock(request_future_mutex);
            size_t index = 0;
            while(index < request_future.size()) {
                try {
                    if(request_future[index].valid()) {
                        std::future_status status = request_future[index].wait_for(std::chrono::milliseconds(0));
                        if(status == std::future_status::ready) {
                            request_future[index].get();
                            request_future.erase(request_future.begin() + index);
                            continue;
                        }
                    }
                }
                catch(const std::exception &e) {
                    std::cerr <<"Error: BinanceApi::clear_request_future(), what: " << e.what() << std::endl;
                }
                catch(...) {
                    std::cerr <<"Error: BinanceApi::clear_request_future()" << std::endl;
                }
                ++index;
            }
        }

    public:

        /** \brief Инициализация главных компонент API
         * \param settings Настройки API
         * \return Вернет true в случае успеха
         */
        bool init_main(Settings &settings) {
            binance_http_api = std::make_shared<BinanceHttpApi>(
                    settings.api_key,
                    settings.secret_key,
                    settings.demo,
                    settings.sert_file,
                    settings.cookie_file);

            /* сначала проверяем соединение с сервером */
            if(!binance_http_api->ping()) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::ping()" << std::endl;
                return false;
            }

            /* устанавливаем режим хеджирования */
            int err = binance_http_api->change_position_mode(settings.position_mode);
            if(err != binance_api::OK) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::change_position_mode(), code: " << err << std::endl;
                return false;
            }

            /* устанавливаем типы маржи ля всех указанных символов */
            for(size_t i = 0; i < settings.margin_types.size(); ++i) {
                int err = binance_http_api->change_margin_type(
                    settings.margin_types[i].first,
                    settings.margin_types[i].second);
                if(err != binance_api::OK) {
                    is_error = true;
                    std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::change_margin_type(), code: " << err << std::endl;
                    return false;
                }
            }

            /* устанавливаем кредитные плечи */
            for(size_t i = 0; i < settings.leverages.size(); ++i) {
                int err = binance_http_api->change_initial_leverage(
                    settings.leverages[i].first,
                    settings.leverages[i].second);
                if(err != binance_api::OK) {
                    is_error = true;
                    std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::change_initial_leverage(), code: " << err << std::endl;
                    return false;
                }
            }
            /* получаем ключ для подключения вебсокета */
            err = binance_http_api->start_user_data_stream(listen_key);
            if(err != binance_api::OK) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::start_user_data_stream(), code: " << err << std::endl;
                return false;
            }

            /* создаем поток пользовательских данных */
            user_data_streams = std::make_shared<UserDataStreams>(listen_key, settings.demo);
            user_data_streams->on_balance = [&](const binance_api::BalanceSpec &balance){
                std::cout << "on_balance, " << balance.asset << " balance: " << balance.wallet_balance << std::endl;
            };

            user_data_streams->on_position = [&](const binance_api::PositionSpec &position){
                std::cout
                    << "on_position, " << position.symbol
                    << " position side: " << (int)position.position_side
                    << " amount: " << position.position_amount << std::endl;
            };
            /*  запускаем поток пользовательских данных */
            user_data_streams->start();

            /* узнаем состояние позиций */
            err = binance_http_api->get_position_risk("",[&](const binance_api::PositionSpec &position) {
                user_data_streams->set_position(position);
            });
            if(err != binance_api::OK) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::get_position_risk(), code: " << err << std::endl;
                return false;
            }

            /* узнаем баланс */
            err = binance_http_api->get_balance([&](const binance_api::BalanceSpec &balance) {
                user_data_streams->set_balance(balance);
            });
            if(err != binance_api::OK) {
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::get_balance(), code: " << err << std::endl;
                return false;
            }

            /* настраиваем замер смещения метки времени */
            timestamp_streams = std::make_shared<CandlestickStreams>(
                settings.demo,
                settings.sert_file);

            const std::vector<std::string> timestamp_streams_symbol = {"BTCUSDT", "ETCUSDT", "LTCUSDT"};
            const uint32_t timestamp_streams_period = 1;
            timestamp_streams->add_symbol_stream(timestamp_streams_symbol[0], timestamp_streams_period);
            timestamp_streams->add_symbol_stream(timestamp_streams_symbol[1], timestamp_streams_period);
            timestamp_streams->add_symbol_stream(timestamp_streams_symbol[2], timestamp_streams_period);
            timestamp_streams->start();


            /* Запускаем очень медленный поток для продления работы потока user_data_streams.
             * Также в данном потоке обновляем смещение метки времени для http api.
             * Перед запуском также проверяем, не был ли поток запущен ранее.
             */
            is_user_data_streams_future_shutdown = true;
            if(user_data_streams_future.valid()) {
                try {
                    user_data_streams_future.wait();
                    user_data_streams_future.get();
                }
                catch(const std::exception &e) {
                    std::cerr <<"Error: BinanceApi::init_main(), what: user_data_streams_future.wait() or user_data_streams_future.get(), exception: " << e.what() << std::endl;
                }
                catch(...) {
                    std::cerr <<"Error: BinanceApi::init_main(), what: user_data_streams_future.wait() or user_data_streams_future.get()" << std::endl;
                }
            }
            is_user_data_streams_future_shutdown = false;

            user_data_streams_future = std::async(std::launch::async,[&] {
                xtime::timestamp_t last_time = xtime::get_timestamp();
                while(!is_future_shutdown && !is_user_data_streams_future_shutdown) {
                    xtime::timestamp_t cur_time = xtime::get_timestamp();
                    if((cur_time - last_time) > (xtime::SECONDS_IN_MINUTE * 30)) {
                        last_time = cur_time;
                        /* проделваем подписку на поток пользовательских данных */
                        if(binance_http_api) {
                            int err = binance_http_api->keepalive_user_data_stream();
                            if(err != binance_api::OK) {
                                is_error = true;
                                std::cerr <<"Error: BinanceApi::init_main, what: binance_http_api::keepalive_user_data_stream(), code: " << err << std::endl;
                                return;
                            }
                        }
                    }

                    /* обновляем смещение метки времени доступынм образом */
                    if(binance_http_api) {
                        if(candlestick_streams && candlestick_streams->connected()) {
                            binance_http_api->set_server_offset_timestamp(candlestick_streams->get_offset_timestamp());
                        } else
                        if(timestamp_streams && timestamp_streams->connected()) {
                            binance_http_api->set_server_offset_timestamp(timestamp_streams->get_offset_timestamp());
                        }
                    }

                    /* ждем некоторое время, данный поток не должен быть быстрым */
                    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                };
            });
            return true;
        }

        /** \brief Получить метку времени сервера
         * \return Метка времени сервера
         */
        inline xtime::ftimestamp_t get_server_ftimestamp() {
            if(binance_http_api) return binance_http_api->get_server_ftimestamp();
            return  xtime::get_ftimestamp();
        }


        BinanceApi() {
        };

        ~BinanceApi() {
            if(is_error) return;
            is_future_shutdown = true;
            /* отключаем поток пользовательских данных */
            if(binance_http_api) {
                int err = binance_http_api->delete_user_data_stream();
                if(err != binance_api::OK) {
                    std::cerr <<"Error: ~BinanceApi::init_main, what: binance_http_api::delete_user_data_stream(), code: " << err << std::endl;
                    return;
                }
            }

            /* закрываем все потоки */
            {
                std::lock_guard<std::mutex> lock(request_future_mutex);
                for(size_t i = 0; i < request_future.size(); ++i) {
                    if(request_future[i].valid()) {
                        try {
                            request_future[i].wait();
                            request_future[i].get();
                        }
                        catch(const std::exception &e) {
                            std::cerr <<"Error: BinanceApi::~BinanceApi(), waht: request_future, exception: " << e.what() << std::endl;
                        }
                        catch(...) {
                            std::cerr <<"Error: BinanceApi::~BinanceApi(), waht: request_future" << std::endl;
                        }
                    }
                }
            }

            if(user_data_streams_future.valid()) {
                try {
                    user_data_streams_future.wait();
                    user_data_streams_future.get();
                }
                catch(const std::exception &e) {
                    std::cerr <<"Error: BinanceApi::~BinanceApi(), what: user_data_streams_future.wait() or user_data_streams_future.get(), exception: " << e.what() << std::endl;
                }
                catch(...) {
                    std::cerr <<"Error: BinanceApi::~BinanceApi(), what: user_data_streams_future.wait() or user_data_streams_future.get()" << std::endl;
                }
            }
        }

        /** \brief Закрыть ордера
         * \param symbol Символ
         * \return Код ошибки
         */
        int close_order(const std::string &symbol) {
            if(is_error) return DATA_NOT_AVAILABLE;
            if(!binance_http_api) return DATA_NOT_AVAILABLE;
            if(!user_data_streams) return DATA_NOT_AVAILABLE;
            /* проверяем наличие позиции по данной паре */
            PositionSpec position;
            if(user_data_streams->get_position(symbol, TypesPositionSide::LONG, position) && position.position_amount != 0.0) {
                /* закрываем позицию */
                //std::cout << "LONG " << position.position_amount << std::endl;
                int err = binance_http_api->open_market_order(
                    symbol,
                    get_uuid(get_server_ftimestamp()),
                    TypesSide::SELL,
                    TypesPositionSide::LONG,
                    binance_api::TypesPositionMode::Hedge_Mode,
                    std::abs(position.position_amount));
                if(err != 0) return err;
            } else
            if(user_data_streams->get_position(symbol, TypesPositionSide::SHORT, position) && position.position_amount != 0.0) {
                /* закрываем позицию */
                //std::cout << "SHORT " << position.position_amount << std::endl;
                int err = binance_http_api->open_market_order(
                    symbol,
                    get_uuid(get_server_ftimestamp()),
                    TypesSide::BUY,
                    TypesPositionSide::SHORT,
                    binance_api::TypesPositionMode::Hedge_Mode,
                    std::abs(position.position_amount));
                if(err != 0) return err;
            } else
            if(user_data_streams->get_position(symbol, TypesPositionSide::BOTH, position) && position.position_amount != 0.0) {
                /* закрываем позицию */
                //std::cout << "BOTH " << position.position_amount << std::endl;
                const TypesSide side = position.position_amount > 0 ? TypesSide::SELL : TypesSide::BUY;
                int err = binance_http_api->open_market_order(
                    symbol,
                    get_uuid(get_server_ftimestamp()),
                    side,
                    TypesPositionSide::BOTH,
                    binance_api::TypesPositionMode::One_way_Mode,
                    std::abs(position.position_amount));
                if(err != 0) return err;
            }
            return OK;
        }

        int open_order(
                const std::string &symbol,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const double quantity,
                const double take_profit,
                const double stop_loss,
                const uint64_t expiration,
                const bool use_date = false,
                const uint64_t recv_window = 60000) {
            if(position_side != TypesPositionSide::LONG && position_side != TypesPositionSide::SHORT) return INVALID_PARAMETER;
            if(is_error) return DATA_NOT_AVAILABLE;
            if(!binance_http_api) return DATA_NOT_AVAILABLE;
            if(!user_data_streams) return DATA_NOT_AVAILABLE;
            /* запускаем асинхронное открытие сделки */
            {
                std::lock_guard<std::mutex> lock(request_future_mutex);
                request_future.resize(request_future.size() + 1);
                request_future.back() = std::async(std::launch::async,[&,
                        symbol,
                        position_side,
                        position_mode,
                        quantity,
                        take_profit,
                        stop_loss,
                        expiration,
                        use_date,
                        recv_window] {
                    xtime::ftimestamp_t open_timestamp = 0; // здесь будет время открытия сделки

                    /* проверяем наличие позиции по данной паре и закрываем, если есть */
                    int err = close_order(symbol);
                    if(err != OK) {
                        std::cerr <<"Error: BinanceApi::open_order(), what close_order(), code: " << err << std::endl;
                        return;
                    }

                    /* создаем уникальные номера сделок */
                    std::array<std::string, 4> order_ids = {
                        get_uuid(get_server_ftimestamp()),
                        get_uuid(get_server_ftimestamp()),
                        get_uuid(get_server_ftimestamp()),
                        get_uuid(get_server_ftimestamp())};

                    /* определяем состояния сделок */
                    TypesSide side = position_side == TypesPositionSide::LONG ? TypesSide::BUY : position_side == TypesPositionSide::SHORT ? TypesSide::SELL : TypesSide::NONE;
                    TypesSide side_close = position_side == TypesPositionSide::LONG ? TypesSide::SELL : position_side == TypesPositionSide::SHORT ? TypesSide::BUY : TypesSide::NONE;
                    TypesPositionSide real_position_side = position_mode == TypesPositionMode::One_way_Mode ? TypesPositionSide::BOTH : position_side;

                    /* открываем маркет ордер */
                    int err1 = 0, err2 = 0, err3 = 0;
                    if((err1 = binance_http_api->open_market_order(
                            symbol,
                            order_ids[0],
                            side,
                            real_position_side,
                            position_mode,
                            quantity,
                            recv_window,
                            [&](const xtime::ftimestamp_t timestamp){
                                /* запоминаем время открытия ордера */
                                open_timestamp = timestamp;
                            })) != OK) {
                        /* ордер открылся с ошибкой, дальшей нет смысла продолжать */
                        std::cerr <<"Error: BinanceApi::open_order(), what open_market_order(), code: " << err1 << std::endl;
                        return;
                    }

                    /* открываем два стоп маркета */
                    err2 = binance_http_api->open_stop_market_order(
                        symbol,
                        order_ids[1],
                        binance_api::TypesOrder::TAKE_PROFIT_MARKET,
                        side_close,
                        real_position_side,
                        position_mode,
                        quantity,
                        take_profit,
                        recv_window);

                    err3 = binance_http_api->open_stop_market_order(
                        symbol,
                        order_ids[2],
                        binance_api::TypesOrder::STOP_MARKET,
                        side_close,
                        real_position_side,
                        position_mode,
                        quantity,
                        stop_loss,
                        recv_window);

                    /* проверяем ситуацию, когда уже сработал один из стоп маркет ордеров или была ошибка */
                    if(err2 != OK || err3 != OK) {
                        if(err2 == ORDER_WOULD_IMMEDIATELY_TRIGGER || err3 == ORDER_WOULD_IMMEDIATELY_TRIGGER) {
                            std::cerr <<"Error: BinanceApi::open_order(), what open_stop_market_order(), code: " << err2 << " code-2: " << err3 <<std::endl;
                        }
                        std::cerr <<"Error: BinanceApi::open_order(), what open_stop_market_order(), code: " << err2 << " code-2: " << err3 <<std::endl;
                        //int err_code = std::min(err2,err3);

                        /* закрываем маркет ордер */
                        const size_t attempts = 10;
                        for(uint32_t i = 0; i < attempts; ++i) {
                            if((err1 = binance_http_api->open_market_order(
                                    symbol,
                                    order_ids[4],
                                    side_close,
                                    real_position_side,
                                    position_mode,
                                    quantity,
                                    recv_window)) != OK) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                                continue;
                            }
                            break;
                        }
                        if(err1 != OK) {
                            std::cerr <<"Error: BinanceApi::open_order(), what open_market_order(), code: " << err1 << std::endl;
                        }

                        /* закрывем стоп ордера, если они были открыты */
                        err1 = binance_http_api->cancel_all_order(symbol);
                        if(err1 != OK) {
                            std::cerr <<"Error: BinanceApi::open_order(), what cancel_all_order(), code: " << err1 << std::endl;
                        }
                        std::cout <<"order open and close " << xtime::get_str_date_time(open_timestamp) << std::endl;
                        return;
                    }

                    std::cout <<"order open " << xtime::get_str_date_time(open_timestamp) << std::endl;

                    /* теперь ждем либо дату экспирации либо задержку на время экспирации */
                    xtime::ftimestamp_t stop_timestamp = use_date ? expiration : open_timestamp + expiration;
                    while(true) {
                        /* проверяем условия выхода из цикла */
                        PositionSpec position;
                        if(user_data_streams->get_position(symbol, real_position_side, position) &&
                            position.position_amount == 0.0) {
                            /* позиция была закрыта */

                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_api->cancel_all_order(symbol);
                            if(err1 != OK) {
                                std::cerr <<"Error: BinanceApi::open_order(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            break;
                        }

                        if(get_server_ftimestamp() >= stop_timestamp || is_future_shutdown) {
                            std::cout <<"order expired" << std::endl;
                            /* закрываем маркет ордер */
                            const size_t attempts = 10;
                            for(uint32_t i = 0; i < attempts; ++i) {
                                if((err1 = binance_http_api->open_market_order(
                                        symbol,
                                        order_ids[4],
                                        side_close,
                                        real_position_side,
                                        position_mode,
                                        quantity,
                                        recv_window)) != OK) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                                    continue;
                                }
                                break;
                            }
                            if(err1 != OK) {
                                std::cerr <<"Error: BinanceApi::open_order(), what open_market_order(), code: " << err1 << std::endl;
                            }
                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_api->cancel_all_order(symbol);
                            if(err1 != OK) {
                                std::cerr <<"Error: BinanceApi::open_order(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    /* здесь официально становится известно, что сделка закрылась */
                    std::cout <<"order close" << std::endl;
                });
            }
            clear_request_future();
            return OK;
        }
    };

}

#endif // BINANCE_CPP_API_HPP_INCLUDED
