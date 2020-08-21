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
#include "binance-cpp-fapi-http.hpp"
#include "binance-cpp-sapi-http.hpp"
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
        std::shared_ptr<BinanceHttpFApi> binance_http_fapi;
        std::shared_ptr<BinanceHttpSApi> binance_http_sapi;
        std::shared_ptr<UserDataStreams> user_data_streams;         /**< Поток пользовательских данных */
        std::shared_ptr<SimpleNamedPipe::NamedPipeServer> pipe_server;
        std::vector<std::shared_ptr<binance_api::MqlHst>> mql_history;
        std::mutex mql_history_mutex;
        std::atomic<bool> is_demo_candlestick_stream = ATOMIC_VAR_INIT(false);
        std::atomic<bool> is_futures_candlestick_stream = ATOMIC_VAR_INIT(false);
        std::atomic<bool> is_pipe_server = ATOMIC_VAR_INIT(false);

        template <typename T>
        struct atomwrapper {
            std::atomic<T> _a;

            atomwrapper()
            :_a() {}

            atomwrapper(const std::atomic<T> &a)
            :_a(a.load()) {}

            atomwrapper(const atomwrapper &other)
            :_a(other._a.load()) {}

            atomwrapper &operator=(const atomwrapper &other) {
                _a.store(other._a.load());
                return _a.load();
            }

            atomwrapper &operator=(const bool other) {
                _a.store(other);
                //return this;
            }

            bool operator==(const bool other) {
                return (_a.load() == other);
            }

            bool operator!=(const bool other) {
                return (!(_a.load() == other));
            }
        };

        std::vector<atomwrapper<bool>> is_init_mql_history;
        std::vector<atomwrapper<bool>> is_once_mql_history;

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
            binance_http_fapi = std::make_shared<BinanceHttpFApi>(
                    settings.api_key,
                    settings.secret_key,
                    settings.demo,
                    settings.sert_file,
                    settings.cookie_file);
            binance_http_fapi->set_candlestick_data_demo(settings.demo_candlestick_stream);
            binance_http_sapi = std::make_shared<BinanceHttpSApi>(
                    settings.demo_candlestick_stream,
                    settings.sert_file);
            //binance_http_sapi->set_demo(settings.demo_candlestick_stream);
            //binance_http_sapi->get_exchange_info();

            /* сначала проверяем соединение с сервером */
            if(!binance_http_fapi->ping()) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_fapi::ping()" << std::endl;
                return false;
            }
            if(!binance_http_sapi->ping()) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_sapi::ping()" << std::endl;
                return false;
            }

            /* устанавливаем режим хеджирования */
            int err = binance_http_fapi->change_position_mode(settings.position_mode);
            if(err != binance_api::OK) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_fapi::change_position_mode(), code: " << err << std::endl;
                return false;
            }

            /* устанавливаем типы маржи ля всех указанных символов */
            for(size_t i = 0; i < settings.margin_types.size(); ++i) {
                int err = binance_http_fapi->change_margin_type(
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
                int err = binance_http_fapi->change_initial_leverage(
                    settings.leverages[i].first,
                    settings.leverages[i].second);
                if(err != binance_api::OK) {
                    is_error = true;
                    std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::change_initial_leverage(), code: " << err << std::endl;
                    return false;
                }
            }
            /* получаем ключ для подключения вебсокета */
            err = binance_http_fapi->start_user_data_stream(listen_key);
            if(err != binance_api::OK) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_api::start_user_data_stream(), code: " << err << std::endl;
                return false;
            }

            /* создаем поток пользовательских данных */
            user_data_streams = std::make_shared<UserDataStreams>(listen_key, settings.demo);
            user_data_streams->on_balance = [&](const binance_api::BalanceSpec &balance){
                //std::cout << "on_balance, " << balance.asset << " balance: " << balance.wallet_balance << std::endl;
                if(is_pipe_server && pipe_server) {
                    pipe_server->send_all(
                            "{\"asset\":\"" +
                            balance.asset +
                            "\",\"wallet_balance\":" +
                            std::to_string(balance.wallet_balance) +
                            ",\"cross_wallet_balance\":" +
                            std::to_string(balance.cross_wallet_balance) +
                            "}");
                }
            };

            user_data_streams->on_position = [&](const binance_api::PositionSpec &position){
                //std::cout
                //    << "on_position, " << position.symbol
                //    << " position side: " << (int)position.position_side
                //    << " amount: " << position.position_amount << std::endl;
            };

            /*  запускаем поток пользовательских данных */
            user_data_streams->start();

            /* узнаем состояние позиций */
            err = binance_http_fapi->get_position_risk("",[&](const binance_api::PositionSpec &position) {
                user_data_streams->set_position(position);
            });
            if(err != binance_api::OK) {
                is_error = true;
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_fapi::get_position_risk(), code: " << err << std::endl;
                return false;
            }

            /* узнаем баланс */
            err = binance_http_fapi->get_balance([&](const binance_api::BalanceSpec &balance) {
                user_data_streams->set_balance(balance);
            });
            if(err != binance_api::OK) {
                std::cerr <<"Error: BinanceApi::init_main(), what: binance_http_fapi::get_balance(), code: " << err << std::endl;
                return false;
            }

            /* настраиваем замер смещения метки времени */
            CandlestickStreams::EndpointTypes endpoint_type = CandlestickStreams::EndpointTypes::FUTURES_DEMO;
            is_futures_candlestick_stream = settings.futures_candlestick_stream;
            is_demo_candlestick_stream = settings.demo_candlestick_stream;
            if(settings.demo_candlestick_stream) {
                if(settings.futures_candlestick_stream) endpoint_type = CandlestickStreams::EndpointTypes::FUTURES_DEMO;
                else endpoint_type = CandlestickStreams::EndpointTypes::SPOT_DEMO;
            } else {
                if(settings.futures_candlestick_stream) endpoint_type = CandlestickStreams::EndpointTypes::FUTURES_REAL;
                else endpoint_type = CandlestickStreams::EndpointTypes::SPOT_REAL;
            }
            timestamp_streams = std::make_shared<CandlestickStreams>(
                endpoint_type,
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
                xtime::timestamp_t last_time2 = xtime::get_timestamp();
                while(!is_future_shutdown && !is_user_data_streams_future_shutdown) {
                    //
                    xtime::timestamp_t cur_time = xtime::get_timestamp();
                    if((cur_time - last_time) > (xtime::SECONDS_IN_MINUTE * 30)) {
                        last_time = cur_time;
                        /* подписка на поток пользовательских данных */
                        if(binance_http_fapi) {
                            int err = binance_http_fapi->keepalive_user_data_stream();
                            if(err != binance_api::OK) {
                                is_error = true;
                                std::cerr <<"Error: BinanceApi::init_main, what: binance_http_fapi::keepalive_user_data_stream(), code: " << err << std::endl;
                                return;
                            }
                        }
                    }
                    //
                    cur_time = xtime::get_timestamp();
                    if((cur_time - last_time2) > xtime::SECONDS_IN_MINUTE) {
                        last_time2 = cur_time;
                        if(binance_http_fapi) {
                            int err = binance_http_fapi->get_exchange_info();
                            if(err != binance_api::OK) {
                                is_error = true;
                                std::cerr <<"Error: BinanceApi::init_main, what: binance_http_fapi::get_exchange_info(), code: " << err << std::endl;
                                return;
                            }
                        }
                        if(binance_http_sapi) {
                            int err = binance_http_sapi->get_exchange_info();
                            if(err != binance_api::OK) {
                                is_error = true;
                                std::cerr <<"Error: BinanceApi::init_main, what: binance_http_sapi::get_exchange_info(), code: " << err << std::endl;
                                return;
                            }
                        }
                    }

                    /* обновляем смещение метки времени доступынм образом */
                    if(binance_http_fapi) {
                        if(candlestick_streams && candlestick_streams->connected()) {
                            binance_http_fapi->set_server_offset_timestamp(candlestick_streams->get_offset_timestamp());
                        } else
                        if(timestamp_streams && timestamp_streams->connected()) {
                            binance_http_fapi->set_server_offset_timestamp(timestamp_streams->get_offset_timestamp());
                        }
                    }

                    /* отправляем пинг */
                    if(is_pipe_server && pipe_server) {
                        pipe_server->send_all("{\"ping\":1}");
                    }

                    /* ждем некоторое время, данный поток не должен быть быстрым */
                    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                };
            });
            return true;
        }

        bool init_candles_stream_mt4(Settings &settings) {
            if(!binance_http_fapi) return false;
            if(!binance_http_sapi) return false;
            if(is_error) return false;

            CandlestickStreams::EndpointTypes endpoint_type = CandlestickStreams::EndpointTypes::FUTURES_DEMO;
            if(settings.demo_candlestick_stream) {
                if(settings.futures_candlestick_stream) endpoint_type = CandlestickStreams::EndpointTypes::FUTURES_DEMO;
                else endpoint_type = CandlestickStreams::EndpointTypes::SPOT_DEMO;
            } else {
                if(settings.futures_candlestick_stream) endpoint_type = CandlestickStreams::EndpointTypes::FUTURES_REAL;
                else endpoint_type = CandlestickStreams::EndpointTypes::SPOT_REAL;
            }

            candlestick_streams = std::make_shared<CandlestickStreams>(
                endpoint_type,
                settings.sert_file);

            /* проверяем параметры символов */
            for(size_t i = 0; i < settings.symbols.size(); ++i) {
                if(settings.futures_candlestick_stream) {
                    if(!binance_http_fapi->check_symbol(settings.symbols[i].first)) {
                        std::cout << "Symbol " << settings.symbols[i].first << " does not exist!" << std::endl;
                        return false;
                    }
                    if(!binance_http_fapi->check_period(settings.symbols[i].second)) {
                        std::cout << "Period " << settings.symbols[i].second << " does not exist!" << std::endl;
                        return false;
                    }
                } else {
                    if(!binance_http_sapi->check_symbol(settings.symbols[i].first)) {
                        std::cout << "Symbol " << settings.symbols[i].first << " does not exist!" << std::endl;
                        return false;
                    }
                    if(!binance_http_sapi->check_period(settings.symbols[i].second)) {
                        std::cout << "Period " << settings.symbols[i].second << " does not exist!" << std::endl;
                        return false;
                    }
                }
            }

            /* узнаем точность символов */
            std::vector<uint32_t> precisions;
            for(size_t i = 0; i < settings.symbols.size(); ++i) {
                if(settings.futures_candlestick_stream) precisions.push_back(binance_http_fapi->get_precision(settings.symbols[i].first));
                else precisions.push_back(binance_http_sapi->get_precision(settings.symbols[i].first));
                std::cout
                    << "Symbol " << settings.symbols[i].first
                    << " period " << settings.symbols[i].second
                    << " precision " << precisions.back()
                    << std::endl;
            }

            /* инициализируем исторические данные MQL */
            for(size_t i = 0; i < settings.symbols.size(); ++i) {
                mql_history.push_back(std::make_shared<binance_api::MqlHst>(
                    settings.symbols[i].first,
                    settings.path,
                    settings.symbols[i].second,
                    precisions[i],
                    settings.timezone));
            }

            is_init_mql_history.resize(settings.symbols.size());
            is_once_mql_history.resize(settings.symbols.size());
            for(size_t i = 0; i < settings.symbols.size(); ++i) {
                is_init_mql_history.push_back(std::atomic<bool>(false));
                is_once_mql_history.push_back(std::atomic<bool>(false));
                //is_init_mql_history[i] = false;
                //is_once_mql_history[i] = false;
            }

            /* инициализируем callback функцию потока котировок */
            candlestick_streams->on_candle = [&](
                    const std::string &symbol,
                    const xquotes_common::Candle &candle,
                    const uint32_t period,
                    const bool close_candle) {

                /* получаем размер массива исторических даных MQL */
                size_t mql_history_size = 0;
                {
                    std::lock_guard<std::mutex> lock(mql_history_mutex);
                    mql_history_size = mql_history.size();
                }

                if(mql_history_size != 0) {
                    for(size_t i = 0; i < mql_history_size; ++i) {
                        /* проверяем соответствие параметров символа */
                        if(settings.symbols[i].first != symbol || settings.symbols[i].second != period) continue;

                        /* проверяем наличие инициализации исторических данных */
                        if(is_init_mql_history[i] == false) continue;

                        /* проверяем, была ли только что загрузка исторических данных */
                        if(is_once_mql_history[i] == false) {
                            /* получаем последнюю метку времени исторических данных */
                            xtime::timestamp_t last_timestamp = 0;
                            {
                                std::lock_guard<std::mutex> lock(mql_history_mutex);
                                last_timestamp = mql_history[i]->get_last_timestamp();
                            }

                            /* проверяем, не успел ли прийти новый бар,
                             * пока мы загружали исторические данных
                             */
                            if(candle.timestamp > last_timestamp) {
                                /* добавляем пропущенные исторические данные */
                                const xtime::timestamp_t step_time = period * xtime::SECONDS_IN_MINUTE;
                                for(xtime::timestamp_t t = last_timestamp; t < candle.timestamp; t += step_time) {
                                    xquotes_common::Candle old_candle = candlestick_streams->get_timestamp_candle(symbol, period, t);
                                    std::lock_guard<std::mutex> lock(mql_history_mutex);

                                    /* если бар есть в истории потока котировок, то загрузим данные. Иначе пропускаем */
                                    if(old_candle.close != 0 && old_candle.timestamp == t) mql_history[i]->add_new_candle(old_candle);
                                }
                            }
                            is_once_mql_history[i] = true;
                        }

                        /* если параметры соответствуют, обновляем исторические данные */
                        if(close_candle) {
                            std::lock_guard<std::mutex> lock(mql_history_mutex);
                            mql_history[i]->add_new_candle(candle);
                        } else {
                            std::lock_guard<std::mutex> lock(mql_history_mutex);
                            mql_history[i]->update_candle(candle);
                        }
                    }
                }
#               if(0)
                /* выводим сообщение о символе */
                std::cout
                    << symbol
                    //<< " o: " << candle.open
                    << " c: " << candle.close
                    //<< " h: " << candle.high
                    //<< " l: " << candle.low
                    << " v: " << candle.volume
                    << " p: " << period
                    << " t: " << xtime::get_str_time(candle.timestamp)
                    << " cc: " << close_candle
                    << std::endl;
#               endif
            };

            /* инициализируем потоки котировок */
            for(size_t i = 0; i < settings.symbols.size(); ++i) {
                candlestick_streams->add_symbol_stream(settings.symbols[i].first, settings.symbols[i].second);
            }
            candlestick_streams->start();
            candlestick_streams->wait();

            /* загружаем исторические данные */
            for(size_t i = 0; i < settings.symbols.size(); ++i) {
                const xtime::timestamp_t stop_date = xtime::get_first_timestamp_minute();
                const xtime::timestamp_t start_date = stop_date - (settings.symbols[i].second * xtime::SECONDS_IN_MINUTE * settings.candles);
                std::vector<xquotes_common::Candle> candles;

                if(settings.futures_candlestick_stream) binance_http_fapi->get_historical_data(candles, settings.symbols[i].first, settings.symbols[i].second, start_date, stop_date);
                else binance_http_sapi->get_historical_data(candles, settings.symbols[i].first, settings.symbols[i].second, start_date, stop_date);

                for(size_t c = 0; c < candles.size(); ++c) {
                    xquotes_common::Candle candle = candles[c];
                    /* добавляем все новые бары, за исключением последнего,
                     * так как он может быть изменен за время загрузки истории
                     */
                    if(c < (candles.size() - 1)) mql_history[i]->add_new_candle(candle);
                    else mql_history[i]->update_candle(candle);
#               if(0)
                    std::cout
                        << settings.symbols[i].first
                        //<< " o: " << candle.open
                        << " c: " << candle.close
                        //<< " h: " << candle.high
                        //<< " l: " << candle.low
                        << " v: " << candle.volume
                        << " t: " << xtime::get_str_date_time(candle.timestamp)
                        << std::endl;
#               endif
                }

                /* ставим флаг инициализации исторических данных */
                is_init_mql_history[i] = true;
                std::cout << settings.symbols[i].first << " init, date: " << xtime::get_str_date_time(start_date) << " - " << xtime::get_str_date_time(stop_date) << std::endl;
            }
            return true;
        }

        bool init_pipe_server(Settings &settings) {
            if(!binance_http_fapi) return false;
            if(!binance_http_sapi) return false;
            if(!user_data_streams) return false;
            if(is_error) return false;

            const size_t buffer_size = 1024*10;
            pipe_server = std::make_shared<SimpleNamedPipe::NamedPipeServer>(
                settings.named_pipe,
                buffer_size);

            pipe_server->on_open = [&](SimpleNamedPipe::NamedPipeServer::Connection* connection) {
                PrintThread{} << "named pipe open, handle: " << connection->get_handle() << std::endl;
                /* отправляем баланс при первом подключении */
                if(user_data_streams) {
                    std::vector<BalanceSpec> balances = user_data_streams->get_all_balance();
                    for(size_t i = 0; i < balances.size(); ++i) {
                        pipe_server->send_all(
                            "{\"asset\":\"" +
                            balances[i].asset +
                            "\",\"wallet_balance\":" +
                            std::to_string(balances[i].wallet_balance) +
                            ",\"cross_wallet_balance\":" +
                            std::to_string(balances[i].cross_wallet_balance) +
                            "}");
                    }
                }
                /* отправляем состояние "подключено" */
                pipe_server->send_all("{\"connection\":1}");
            };

            pipe_server->on_message = [&,settings](SimpleNamedPipe::NamedPipeServer::Connection* connection, const std::string &in_message) {
                /* обрабатываем входящие сообщения */
                //std::cout << "message " << in_message << ", handle: " << connection->get_handle() << std::endl;
                /* парисм */
                try {
                    json j = json::parse(in_message);
                    if(j.find("contract") != j.end()) {
                        /* пришел запрос на открытие сделки */
                        //std::cout << "contract" << std::endl;
                        json j_contract = j["contract"];
                        /* параметры опциона */
                        std::string symbol = j_contract["symbol"];
                        std::string order_version;
                        std::string strategy_name;
                        if(j_contract.find("strategy_name") != j_contract.end()) strategy_name = j_contract["strategy_name"];
                        if(j_contract.find("order_version") != j_contract.end()) order_version = j_contract["order_version"];

                        binance_api::TypesPositionSide position_side = binance_api::TypesPositionSide::NONE;
                        if(j_contract["position_side"] == "BUY" || j_contract["position_side"] == "LONG") position_side = binance_api::TypesPositionSide::LONG;
                        else if(j_contract["position_side"] == "SELL" || j_contract["position_side"] == "SHORT") position_side = binance_api::TypesPositionSide::SHORT;
                        uint32_t duration = 0;
                        xtime::timestamp_t date_expiry = 0;

                        if(j_contract.find("date_expiry") != j_contract.end() && j_contract["date_expiry"].is_number()) {
                            date_expiry = j_contract["date_expiry"];
                        } else
                        if(j_contract.find("duration") != j_contract.end() && j_contract["duration"].is_number()) {
                            duration = j_contract["duration"];
                        }

                        if(order_version.size() == 0 || order_version == "v1") {
                            const double quantity = j_contract["quantity"];
                            const double take_profit = j_contract["take_profit"];
                            const double stop_loss = j_contract["stop_loss"];

                            /* открываем ордер */
                            if (symbol.size() > 0 &&
                                take_profit > 0 && stop_loss > 0 &&
                                quantity > 0 &&
                                (position_side == binance_api::TypesPositionSide::LONG ||
                                 position_side == binance_api::TypesPositionSide::SHORT) &&
                                (duration > 0 || date_expiry > 0)) {

                                const bool is_use_data = date_expiry > 0 ? true : false;
                                const uint64_t expiration = is_use_data ? date_expiry : duration;
                                int err = open_order(
                                    symbol,
                                    position_side,
                                    settings.position_mode,
                                    quantity,
                                    take_profit,
                                    stop_loss,
                                    expiration,
                                    is_use_data,
                                    settings.recv_window,
                                    [&,symbol](
                                        const int status,
                                        const xtime::ftimestamp_t timestamp){
                                    switch(status) {
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_9:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_8:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_7:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_6:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_5:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_4:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_3:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_2:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_1:
                                            PrintThread{} << "order " << symbol << ": error opening" << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_OPEN:
                                            PrintThread{} << "order " << symbol << ": open " << xtime::get_str_date_time_ms(timestamp) << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_CLOSE_1:
                                            PrintThread{} << "order " << symbol << ": premature closure of the order" << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_CLOSE_2:
                                            PrintThread{} << "order " << symbol << ": Take profit or stop loss order triggered" << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_CLOSE_3:
                                            PrintThread{} << "order " << symbol << ": Expiration time is up" << std::endl;
                                            break;
                                    }
                                });
                                if(err != OK) {
                                    PrintThread{} << "order opening error, symbol: " << symbol << " code: " << err << std::endl;
                                }
                            } else {
                                PrintThread{} << "named pipe error: invalid parameters" << std::endl;
                            }
                        } else
                        if(order_version == "v2") {
                            const std::vector<double> quantitys = j_contract["quantitys"];
                            const uint64_t take_profit_pips = j_contract["take_profit_pips"];
                            const uint64_t stop_loss_pips = j_contract["stop_loss_pips"];

                            /* открываем ордер */
                            if (symbol.size() > 0 &&
                                quantitys.size() > 0 &&
                                (position_side == binance_api::TypesPositionSide::LONG ||
                                 position_side == binance_api::TypesPositionSide::SHORT) &&
                                (duration > 0 || date_expiry > 0)) {

                                const bool is_use_data = date_expiry > 0 ? true : false;
                                const uint64_t expiration = is_use_data ? date_expiry : duration;
                                int err = open_order_v2(
                                    symbol,
                                    position_side,
                                    settings.position_mode,
                                    quantitys,
                                    take_profit_pips,
                                    stop_loss_pips,
                                    expiration,
                                    is_use_data,
                                    settings.recv_window,
                                    [&,symbol](
                                        const int status,
                                        const xtime::ftimestamp_t timestamp){
                                    switch(status) {
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_9:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_8:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_7:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_6:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_5:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_4:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_3:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_2:
                                        case binance_api::OPEN_ORDER_STATUS_ERROR_1:
                                            PrintThread{} << "order-v2 " << symbol << ": error opening, code: " << status << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_OPEN:
                                            PrintThread{} << "order-v2 " << symbol << ": open " << xtime::get_str_date_time_ms(timestamp) << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_CLOSE_1:
                                            PrintThread{} << "order-v2 " << symbol << ": premature closure of the order" << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_CLOSE_2:
                                            PrintThread{} << "order-v2 " << symbol << ": Take profit or stop loss order triggered" << std::endl;
                                            break;
                                        case binance_api::OPEN_ORDER_STATUS_CLOSE_3:
                                            PrintThread{} << "order-v2 " << symbol << ": Expiration time is up" << std::endl;
                                            break;
                                    }
                                });
                                if(err != OK) {
                                    PrintThread{} << "order opening error, symbol: " << symbol << " code: " << err << std::endl;
                                }
                            } else {
                                PrintThread{} << "named pipe error: invalid parameters" << std::endl;
                            }
                        } else {
                            PrintThread{} << "named pipe error: invalid parameters" << std::endl;
                        }

                    }
                }
                catch(...) {
                    PrintThread{} << "named pipe error: json::parse" << std::endl;
                }
            };

            pipe_server->on_close = [&](SimpleNamedPipe::NamedPipeServer::Connection* connection) {
                PrintThread{} << "named pipe close, handle: " << connection->get_handle() << std::endl;
            };

            pipe_server->on_error = [&](SimpleNamedPipe::NamedPipeServer::Connection* connection, const std::error_code &ec) {
                PrintThread{} << "named pipe error, handle: " << connection->get_handle() << ", what " << ec.value() << std::endl;
            };

            /* запускаем сервер */
            pipe_server->start();
            is_pipe_server = true;
            return true;
        }

        /** \brief Получить метку времени сервера
         * \return Метка времени сервера
         */
        inline xtime::ftimestamp_t get_server_ftimestamp() {
            if(binance_http_fapi) return binance_http_fapi->get_server_ftimestamp();
            return xtime::get_ftimestamp();
        }

        BinanceApi() {

        };

        ~BinanceApi() {
            if(is_error) return;
            is_future_shutdown = true;
            /* отключаем поток пользовательских данных */
            if(binance_http_fapi) {
                int err = binance_http_fapi->delete_user_data_stream();
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
         * \param position_amount Размер позиции. Если не указан, то закрывается вся позиция
         * \return Код ошибки
         */
        int close_order(const std::string &symbol, const double position_amount = 0) {
            if(is_error) return DATA_NOT_AVAILABLE;
            if(!binance_http_fapi) return DATA_NOT_AVAILABLE;
            if(!binance_http_sapi) return DATA_NOT_AVAILABLE;
            if(!user_data_streams) return DATA_NOT_AVAILABLE;
            /* проверяем наличие позиции по данной паре */
            PositionSpec position;
            if(user_data_streams->get_position(symbol, TypesPositionSide::LONG, position) && position.position_amount != 0.0) {
                /* закрываем позицию */
                //std::cout << "LONG " << position.position_amount << std::endl;
                int err = binance_http_fapi->open_market_order(
                    symbol,
                    get_uuid(get_server_ftimestamp()),
                    TypesSide::SELL,
                    TypesPositionSide::LONG,
                    binance_api::TypesPositionMode::Hedge_Mode,
                    (position_amount == 0 ? std::abs(position.position_amount) : std::abs(position_amount)));
                if(err != OK) {
                    std::cerr <<"Error: BinanceApi::close_order(), what open_market_order(), code: " << err << std::endl;
                }
                /* закрывем стоп ордера, если они были открыты */
                int err1 = binance_http_fapi->cancel_all_order(symbol);
                if(err1 != OK) {
                    std::cerr <<"Error: BinanceApi::close_order(), what cancel_all_order(), code: " << err1 << std::endl;
                }
                if(err != OK || err1 != OK) return std::min(err, err1);
            } else
            if(user_data_streams->get_position(symbol, TypesPositionSide::SHORT, position) && position.position_amount != 0.0) {
                /* закрываем позицию */
                //std::cout << "SHORT " << position.position_amount << std::endl;
                int err = binance_http_fapi->open_market_order(
                    symbol,
                    get_uuid(get_server_ftimestamp()),
                    TypesSide::BUY,
                    TypesPositionSide::SHORT,
                    binance_api::TypesPositionMode::Hedge_Mode,
                    std::abs(position.position_amount));
                if(err != OK) {
                    std::cerr <<"Error: BinanceApi::close_order(), what open_market_order(), code: " << err << std::endl;
                }
                /* закрывем стоп ордера, если они были открыты */
                int err1 = binance_http_fapi->cancel_all_order(symbol);
                if(err1 != OK) {
                    std::cerr <<"Error: BinanceApi::close_order(), what cancel_all_order(), code: " << err1 << std::endl;
                }
                if(err != OK || err1 != OK) return std::min(err, err1);
            } else
            if(user_data_streams->get_position(symbol, TypesPositionSide::BOTH, position) && position.position_amount != 0.0) {
                /* закрываем позицию */
                //std::cout << "BOTH " << position.position_amount << std::endl;
                const TypesSide side = position.position_amount > 0 ? TypesSide::SELL : TypesSide::BUY;
                int err = binance_http_fapi->open_market_order(
                    symbol,
                    get_uuid(get_server_ftimestamp()),
                    side,
                    TypesPositionSide::BOTH,
                    binance_api::TypesPositionMode::One_way_Mode,
                    std::abs(position.position_amount));
                if(err != OK) {
                    std::cerr <<"Error: BinanceApi::close_order(), what open_market_order(), code: " << err << std::endl;
                }
                /* закрывем стоп ордера, если они были открыты */
                int err1 = binance_http_fapi->cancel_all_order(symbol);
                if(err1 != OK) {
                    std::cerr <<"Error: BinanceApi::close_order(), what cancel_all_order(), code: " << err1 << std::endl;
                }
                if(err != OK || err1 != OK) return std::min(err, err1);
            }
            return OK;
        }

        /** \brief Открыть ордер
         * \param symbol Символ
         * \param position_side
         * \param position_mode
         * \param quantity
         * \param take_profit
         * \param stop_loss
         * \param expiration
         * \param use_date
         * \param recv_window
         * \return Код ошибки
         */
        int open_order(
                const std::string &symbol,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const double quantity,
                const double take_profit,
                const double stop_loss,
                const uint64_t expiration,
                const bool use_date = false,
                const uint64_t recv_window = 60000,
                const std::function<void(
                    const int status,
                    const xtime::ftimestamp_t timestamp)> callback = nullptr) {
            if(position_side != TypesPositionSide::LONG && position_side != TypesPositionSide::SHORT) return INVALID_PARAMETER;
            if(is_error) return DATA_NOT_AVAILABLE;
            if(!binance_http_fapi) return DATA_NOT_AVAILABLE;
            if(!binance_http_sapi) return DATA_NOT_AVAILABLE;
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
                        recv_window,
                        callback] {
                    xtime::ftimestamp_t open_timestamp = 0; // здесь будет время открытия сделки

                    /* проверяем наличие позиции по данной паре и закрываем, если есть */
                    int err = close_order(symbol);
                    if(err != OK) {
                        std::cerr <<"Error: BinanceApi::open_order(), what close_order(), code: " << err << std::endl;
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
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
                    if((err1 = binance_http_fapi->open_market_order(
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
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                        return;
                    }

                    if(callback != nullptr) callback(OPEN_ORDER_STATUS_OPEN, open_timestamp);

                    /* открываем два стоп маркета */
                    err2 = binance_http_fapi->open_stop_market_order(
                        symbol,
                        order_ids[1],
                        binance_api::TypesOrder::TAKE_PROFIT_MARKET,
                        side_close,
                        real_position_side,
                        position_mode,
                        quantity,
                        take_profit,
                        false,
                        recv_window);

                    err3 = binance_http_fapi->open_stop_market_order(
                        symbol,
                        order_ids[2],
                        binance_api::TypesOrder::STOP_MARKET,
                        side_close,
                        real_position_side,
                        position_mode,
                        quantity,
                        stop_loss,
                        false,
                        recv_window);

                    /* проверяем ситуацию, когда уже сработал один из стоп маркет ордеров или была ошибка */
                    if(err2 != OK || err3 != OK) {
                        if(err2 == ORDER_WOULD_IMMEDIATELY_TRIGGER || err3 == ORDER_WOULD_IMMEDIATELY_TRIGGER) {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_3, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order(), what open_stop_market_order(), code: " << err2 << " code-2: " << err3 <<std::endl;
                        } else {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_4, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order(), what open_stop_market_order(), code: " << err2 << " code-2: " << err3 <<std::endl;
                        }
                        //std::cerr <<"Error: BinanceApi::open_order(), what open_stop_market_order(), code: " << err2 << " code-2: " << err3 <<std::endl;
                        //int err_code = std::min(err2,err3);

                        /* закрываем маркет ордер */
                        const size_t attempts = 10;
                        for(uint32_t i = 0; i < attempts; ++i) {
                            if((err1 = binance_http_fapi->open_market_order(
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
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_5, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order(), what open_market_order(), code: " << err1 << std::endl;
                        }

                        /* закрывем стоп ордера, если они были открыты */
                        err1 = binance_http_fapi->cancel_all_order(symbol);
                        if(err1 != OK) {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_6, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order(), what cancel_all_order(), code: " << err1 << std::endl;
                        }
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_1, get_server_ftimestamp());
                        //std::cout <<"order open and close " << xtime::get_str_date_time(open_timestamp) << std::endl;
                        return;
                    }

                    //std::cout <<"order open " << xtime::get_str_date_time(open_timestamp) << std::endl;

                    /* теперь ждем либо дату экспирации либо задержку на время экспирации */
                    xtime::ftimestamp_t stop_timestamp = use_date ? expiration : open_timestamp + expiration;
                    while(true) {
                        /* проверяем условия выхода из цикла */
                        PositionSpec position;
                        if(user_data_streams->get_position(symbol, real_position_side, position) &&
                            position.position_amount == 0.0) {
                            /* позиция была закрыта */

                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_fapi->cancel_all_order(symbol);
                            if(err1 != OK) {
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_7, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_2, get_server_ftimestamp());
                            break;
                        }

                        if(get_server_ftimestamp() >= stop_timestamp || is_future_shutdown) {
                            //std::cout <<"order expired" << std::endl;
                            /* закрываем маркет ордер */
                            const size_t attempts = 10;
                            for(uint32_t i = 0; i < attempts; ++i) {
                                if((err1 = binance_http_fapi->open_market_order(
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
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_8, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order(), what open_market_order(), code: " << err1 << std::endl;
                            }
                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_fapi->cancel_all_order(symbol);
                            if(err1 != OK) {
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_9, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_3, get_server_ftimestamp());
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    /* здесь официально становится известно, что сделка закрылась */
                    //std::cout <<"order close" << std::endl;
                });
            }
            clear_request_future();
            return OK;
        }

        /** \brief Открыть ордер
         * \param symbol Символ
         * \param position_side
         * \param position_mode
         * \param quantitys
         * \param take_profit_pips
         * \param stop_loss_pips
         * \param expiration
         * \param use_date
         * \param recv_window
         * \return Код ошибки
         */
        int open_order_v2(
                const std::string &symbol,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const std::vector<double> quantitys,
                const uint64_t take_profit_pips,
                const uint64_t stop_loss_pips,
                const uint64_t expiration,
                const bool use_date = false,
                const uint64_t recv_window = 60000,
                const std::function<void(
                    const int status,
                    const xtime::ftimestamp_t timestamp)> callback = nullptr) {
            if(quantitys.size() == 0) return DATA_NOT_AVAILABLE;
            if(position_side != TypesPositionSide::LONG && position_side != TypesPositionSide::SHORT) return INVALID_PARAMETER;
            if(is_error) return DATA_NOT_AVAILABLE;
            if(!binance_http_fapi) return DATA_NOT_AVAILABLE;
            if(!binance_http_sapi) return DATA_NOT_AVAILABLE;
            if(!user_data_streams) return DATA_NOT_AVAILABLE;

            double price = 0.0;
            /* инициализируем тейк профит и стоп лосс, если они указаны */
            if(take_profit_pips != 0 || stop_loss_pips != 0) {
                if(!candlestick_streams) return DATA_NOT_AVAILABLE;
                if((price = candlestick_streams->get_price(symbol)) == 0) return NO_PRICE_STREAM_SUBSCRIPTION;
            }

            /* запускаем асинхронное открытие сделки */
            {
                std::lock_guard<std::mutex> lock(request_future_mutex);
                request_future.resize(request_future.size() + 1);
                request_future.back() = std::async(std::launch::async,[&,
                        symbol,
                        position_side,
                        position_mode,
                        //quantity,
                        quantitys,
                        take_profit_pips,
                        stop_loss_pips,
                        //take_profit,
                        //stop_loss,
                        expiration,
                        use_date,
                        recv_window,
                        callback] {
                    xtime::ftimestamp_t open_timestamp = 0; // здесь будет время открытия сделки

                    /* проверяем наличие позиции по данной паре и закрываем, если есть */
                    int err = close_order(symbol);
                    if(err != OK) {
                        std::cerr <<"Error: BinanceApi::open_order_v2(), what close_order(), code: " << err << std::endl;
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
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

                    double quantity = 0.0;
                    /* открываем маркет ордер */
                    int err1 = OK, err2 = OK, err3 = OK;
                    for(size_t i = 0; i < quantitys.size(); ++i) {
                        if((err1 = binance_http_fapi->open_market_order(
                                symbol,
                                get_uuid(get_server_ftimestamp()),
                                side,
                                real_position_side,
                                position_mode,
                                quantitys[i],
                                recv_window,
                                [&](const xtime::ftimestamp_t timestamp){
                                    /* запоминаем время открытия ордера */
                                    open_timestamp = timestamp;
                                })) != OK) {
                            /* ордер открылся с ошибкой, дальшей нет смысла продолжать */
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_market_order(), code: " << err1 << std::endl;
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                            continue;
                        } else {
                            quantity += quantitys[i];
                        }
                    }

                    if(quantity == 0) {
                        std::cerr <<"Error: BinanceApi::open_order_v2(), quantity == 0, what open_market_order(), code: " << ((int)std::min(err1, (int)DATA_NOT_AVAILABLE)) << std::endl;
                        return;
                    }

                    if(callback != nullptr) callback(OPEN_ORDER_STATUS_OPEN, open_timestamp);

                    /* получаем последнюю цену актива */
                    double last_price = 0.0;
                    if((take_profit_pips != 0 || stop_loss_pips != 0) && (last_price = candlestick_streams->get_price(symbol)) == 0) {
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_10, get_server_ftimestamp());
                        /* закрываем маркет ордер, так как невозможно установить тейк профит и стоп лосс  */
                        int err = close_order(symbol);
                        std::cerr <<"Error: BinanceApi::open_order_v2(), what: candlestick_streams->get_price(), price: " << last_price << std::endl;
                        if(err != OK) {
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what: close_order(), code: " << err << std::endl;
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                            return;
                        }
                    }

                    /* инициализируем тейк профит и стоп лосс, если они указаны */
                    double step_price = 0.0;
                    double take_profit = 0.0;
                    double stop_loss = 0.0;
                    if(take_profit_pips != 0 || stop_loss_pips != 0) {
                        uint32_t mult = std::pow(10, binance_http_fapi->get_precision(symbol));
                        step_price = 1.0d/(double)mult;
                        take_profit = position_side == TypesPositionSide::LONG ? (last_price + step_price * (double)take_profit_pips) : (last_price - step_price * (double)take_profit_pips);
                        stop_loss = position_side == TypesPositionSide::LONG ? (last_price - step_price * (double)stop_loss_pips) : (last_price + step_price * (double)stop_loss_pips);
                        take_profit = (double)((uint64_t)((take_profit * (double)mult) + 0.5d)) / (double)mult;
                        stop_loss = (double)((uint64_t)((stop_loss * (double)mult) + 0.5d)) / (double)mult;
                        if(position_side == TypesPositionSide::LONG) std::cout << "LONG ";
                        else if(position_side == TypesPositionSide::SHORT) std::cout << "SHORT ";
                        if(side_close == TypesSide::BUY) std::cout << "BUY ";
                        else if(side_close == TypesSide::SELL) std::cout << "SELL ";
                        std::cout << " price: " << last_price << " take: " << take_profit << " stop: " << stop_loss << std::endl;
                    }

                    /* открываем два стоп маркета */
                    if(take_profit_pips != 0) {
                        err2 = binance_http_fapi->open_stop_market_order(
                            symbol,
                            order_ids[1],
                            binance_api::TypesOrder::TAKE_PROFIT_MARKET,
                            side_close,
                            real_position_side,
                            position_mode,
                            quantity,
                            take_profit,
                            true,
                            recv_window);
                    }

                    if(stop_loss_pips != 0) {
                        err3 = binance_http_fapi->open_stop_market_order(
                            symbol,
                            order_ids[2],
                            binance_api::TypesOrder::STOP_MARKET,
                            side_close,
                            real_position_side,
                            position_mode,
                            quantity,
                            stop_loss,
                            true,
                            recv_window);
                    }

                    /* проверяем ситуацию, когда уже сработал один из стоп маркет ордеров или была ошибка */
                    if(err2 != OK || err3 != OK) {
                        if(err2 == ORDER_WOULD_IMMEDIATELY_TRIGGER || err3 == ORDER_WOULD_IMMEDIATELY_TRIGGER) {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_3, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_stop_market_order(), take profit code: " << err2 << " stop loss code: " << err3 <<std::endl;
                        } else {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_4, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_stop_market_order(), take profit code: " << err2 << " stop loss code: " << err3 <<std::endl;
                        }

                        /* закрываем маркет ордер */
                        int err = close_order(symbol);
                        if(err != OK) {
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what close_order(), code: " << err << std::endl;
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                            return;
                        }

#                       if(0)
                        const size_t attempts = 10;
                        for(uint32_t i = 0; i < attempts; ++i) {
                            if((err1 = binance_http_fapi->open_market_order(
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
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_5, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_market_order(), code: " << err1 << std::endl;
                        }
#                       endif

                        /* закрывем стоп ордера, если они были открыты */
                        err1 = binance_http_fapi->cancel_all_order(symbol);
                        if(err1 != OK) {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_6, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what cancel_all_order(), code: " << err1 << std::endl;
                        }
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_1, get_server_ftimestamp());
                        return;
                    }

                    /* теперь ждем либо дату экспирации либо задержку на время экспирации */
                    xtime::ftimestamp_t stop_timestamp = use_date ? expiration : open_timestamp + expiration;
                    while(true) {
                        /* проверяем условия выхода из цикла */
                        PositionSpec position;
                        if(user_data_streams->get_position(symbol, real_position_side, position) &&
                            position.position_amount == 0.0) {
                            /* позиция была закрыта */

                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_fapi->cancel_all_order(symbol);
                            if(err1 != OK) {
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_7, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order_v2(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_2, get_server_ftimestamp());
                            break;
                        }

                        if(get_server_ftimestamp() >= stop_timestamp || is_future_shutdown) {
                            //std::cout <<"order expired" << std::endl;
                            /* закрываем маркет ордер */
                            const size_t attempts = 10;
                            for(uint32_t i = 0; i < attempts; ++i) {
                                if((err1 = binance_http_fapi->open_market_order(
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
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_8, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order_v2(), what open_market_order(), code: " << err1 << std::endl;
                            }
                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_fapi->cancel_all_order(symbol);
                            if(err1 != OK) {
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_9, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order_v2(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_3, get_server_ftimestamp());
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    /* здесь официально становится известно, что сделка закрылась */
                    //std::cout <<"order close" << std::endl;
                });
            }
            clear_request_future();
            return OK;
        }

        /** \brief Открыть ордер
         * \param symbol Символ
         * \param position_side
         * \param position_mode
         * \param quantitys
         * \param recv_window
         * \return Код ошибки
         */
        int open_order_v3(
                const std::string &symbol,
                const TypesPositionSide position_side,
                const TypesPositionMode position_mode,
                const std::vector<double> quantitys,
                const uint64_t recv_window = 60000,
                const std::function<void(
                    const int status,
                    const xtime::ftimestamp_t timestamp)> callback = nullptr) {
            if(quantitys.size() == 0) return DATA_NOT_AVAILABLE;
            if(position_side != TypesPositionSide::LONG && position_side != TypesPositionSide::SHORT) return INVALID_PARAMETER;
            if(is_error) return DATA_NOT_AVAILABLE;
            if(!binance_http_fapi) return DATA_NOT_AVAILABLE;
            if(!binance_http_sapi) return DATA_NOT_AVAILABLE;
            if(!user_data_streams) return DATA_NOT_AVAILABLE;

            double price = 0.0;

            /* запускаем асинхронное открытие сделки */
            {
                std::lock_guard<std::mutex> lock(request_future_mutex);
                request_future.resize(request_future.size() + 1);
                request_future.back() = std::async(std::launch::async,[&,
                        symbol,
                        position_side,
                        position_mode,
                        quantitys,
                        recv_window,
                        callback] {
                    xtime::ftimestamp_t open_timestamp = 0; // здесь будет время открытия сделки

                    /* проверяем наличие позиции по данной паре и закрываем, если есть */
                    int err = close_order(symbol);
                    if(err != OK) {
                        std::cerr <<"Error: BinanceApi::open_order_v2(), what close_order(), code: " << err << std::endl;
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                        return;
                    }



                    /* определяем состояния сделок */
                    TypesSide side = position_side == TypesPositionSide::LONG ? TypesSide::BUY : position_side == TypesPositionSide::SHORT ? TypesSide::SELL : TypesSide::NONE;
                    TypesSide side_close = position_side == TypesPositionSide::LONG ? TypesSide::SELL : position_side == TypesPositionSide::SHORT ? TypesSide::BUY : TypesSide::NONE;
                    TypesPositionSide real_position_side = position_mode == TypesPositionMode::One_way_Mode ? TypesPositionSide::BOTH : position_side;

                    double quantity = 0.0;
                    /* открываем маркет ордер */
                    int err1 = OK, err2 = OK, err3 = OK;
                    for(size_t i = 0; i < quantitys.size(); ++i) {
                        if((err1 = binance_http_fapi->open_market_order(
                                symbol,
                                get_uuid(get_server_ftimestamp()),
                                side,
                                real_position_side,
                                position_mode,
                                quantitys[i],
                                recv_window,
                                [&](const xtime::ftimestamp_t timestamp){
                                    /* запоминаем время открытия ордера */
                                    open_timestamp = timestamp;
                                })) != OK) {
                            /* ордер открылся с ошибкой, дальшей нет смысла продолжать */
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_market_order(), code: " << err1 << std::endl;
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                            continue;
                        } else {
                            quantity += quantitys[i];
                        }
                    }

                    if(quantity == 0) {
                        std::cerr <<"Error: BinanceApi::open_order_v2(), quantity == 0, what open_market_order(), code: " << ((int)std::min(err1, (int)DATA_NOT_AVAILABLE)) << std::endl;
                        return;
                    }

                    if(callback != nullptr) callback(OPEN_ORDER_STATUS_OPEN, open_timestamp);

                    /* получаем последнюю цену актива */
                    double last_price = 0.0;
                    if((take_profit_pips != 0 || stop_loss_pips != 0) && (last_price = candlestick_streams->get_price(symbol)) == 0) {
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_10, get_server_ftimestamp());
                        /* закрываем маркет ордер, так как невозможно установить тейк профит и стоп лосс  */
                        int err = close_order(symbol);
                        std::cerr <<"Error: BinanceApi::open_order_v2(), what: candlestick_streams->get_price(), price: " << last_price << std::endl;
                        if(err != OK) {
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what: close_order(), code: " << err << std::endl;
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                            return;
                        }
                    }

                    /* инициализируем тейк профит и стоп лосс, если они указаны */
                    double step_price = 0.0;
                    double take_profit = 0.0;
                    double stop_loss = 0.0;
                    if(take_profit_pips != 0 || stop_loss_pips != 0) {
                        uint32_t mult = std::pow(10, binance_http_fapi->get_precision(symbol));
                        step_price = 1.0d/(double)mult;
                        take_profit = position_side == TypesPositionSide::LONG ? (last_price + step_price * (double)take_profit_pips) : (last_price - step_price * (double)take_profit_pips);
                        stop_loss = position_side == TypesPositionSide::LONG ? (last_price - step_price * (double)stop_loss_pips) : (last_price + step_price * (double)stop_loss_pips);
                        take_profit = (double)((uint64_t)((take_profit * (double)mult) + 0.5d)) / (double)mult;
                        stop_loss = (double)((uint64_t)((stop_loss * (double)mult) + 0.5d)) / (double)mult;
                        if(position_side == TypesPositionSide::LONG) std::cout << "LONG ";
                        else if(position_side == TypesPositionSide::SHORT) std::cout << "SHORT ";
                        if(side_close == TypesSide::BUY) std::cout << "BUY ";
                        else if(side_close == TypesSide::SELL) std::cout << "SELL ";
                        std::cout << " price: " << last_price << " take: " << take_profit << " stop: " << stop_loss << std::endl;
                    }

                    /* открываем два стоп маркета */
                    if(take_profit_pips != 0) {
                        err2 = binance_http_fapi->open_stop_market_order(
                            symbol,
                            order_ids[1],
                            binance_api::TypesOrder::TAKE_PROFIT_MARKET,
                            side_close,
                            real_position_side,
                            position_mode,
                            quantity,
                            take_profit,
                            true,
                            recv_window);
                    }

                    if(stop_loss_pips != 0) {
                        err3 = binance_http_fapi->open_stop_market_order(
                            symbol,
                            order_ids[2],
                            binance_api::TypesOrder::STOP_MARKET,
                            side_close,
                            real_position_side,
                            position_mode,
                            quantity,
                            stop_loss,
                            true,
                            recv_window);
                    }

                    /* проверяем ситуацию, когда уже сработал один из стоп маркет ордеров или была ошибка */
                    if(err2 != OK || err3 != OK) {
                        if(err2 == ORDER_WOULD_IMMEDIATELY_TRIGGER || err3 == ORDER_WOULD_IMMEDIATELY_TRIGGER) {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_3, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_stop_market_order(), take profit code: " << err2 << " stop loss code: " << err3 <<std::endl;
                        } else {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_4, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_stop_market_order(), take profit code: " << err2 << " stop loss code: " << err3 <<std::endl;
                        }

                        /* закрываем маркет ордер */
                        int err = close_order(symbol);
                        if(err != OK) {
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what close_order(), code: " << err << std::endl;
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_1, get_server_ftimestamp());
                            return;
                        }

#                       if(0)
                        const size_t attempts = 10;
                        for(uint32_t i = 0; i < attempts; ++i) {
                            if((err1 = binance_http_fapi->open_market_order(
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
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_5, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what open_market_order(), code: " << err1 << std::endl;
                        }
#                       endif

                        /* закрывем стоп ордера, если они были открыты */
                        err1 = binance_http_fapi->cancel_all_order(symbol);
                        if(err1 != OK) {
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_6, get_server_ftimestamp());
                            std::cerr <<"Error: BinanceApi::open_order_v2(), what cancel_all_order(), code: " << err1 << std::endl;
                        }
                        if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_1, get_server_ftimestamp());
                        return;
                    }

                    /* теперь ждем либо дату экспирации либо задержку на время экспирации */
                    xtime::ftimestamp_t stop_timestamp = use_date ? expiration : open_timestamp + expiration;
                    while(true) {
                        /* проверяем условия выхода из цикла */
                        PositionSpec position;
                        if(user_data_streams->get_position(symbol, real_position_side, position) &&
                            position.position_amount == 0.0) {
                            /* позиция была закрыта */

                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_fapi->cancel_all_order(symbol);
                            if(err1 != OK) {
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_7, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order_v2(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_2, get_server_ftimestamp());
                            break;
                        }

                        if(get_server_ftimestamp() >= stop_timestamp || is_future_shutdown) {
                            //std::cout <<"order expired" << std::endl;
                            /* закрываем маркет ордер */
                            const size_t attempts = 10;
                            for(uint32_t i = 0; i < attempts; ++i) {
                                if((err1 = binance_http_fapi->open_market_order(
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
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_8, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order_v2(), what open_market_order(), code: " << err1 << std::endl;
                            }
                            /* закрывем стоп ордера, если они были открыты */
                            err1 = binance_http_fapi->cancel_all_order(symbol);
                            if(err1 != OK) {
                                if(callback != nullptr) callback(OPEN_ORDER_STATUS_ERROR_9, get_server_ftimestamp());
                                std::cerr <<"Error: BinanceApi::open_order_v2(), what cancel_all_order(), code: " << err1 << std::endl;
                            }
                            if(callback != nullptr) callback(OPEN_ORDER_STATUS_CLOSE_3, get_server_ftimestamp());
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    /* здесь официально становится известно, что сделка закрылась */
                    //std::cout <<"order close" << std::endl;
                });
            }
            clear_request_future();
            return OK;
        }
    };

}

#endif // BINANCE_CPP_API_HPP_INCLUDED
