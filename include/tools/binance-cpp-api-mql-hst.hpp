#ifndef BINANCE_CPP_API_MQL_HST_HPP_INCLUDED
#define BINANCE_CPP_API_MQL_HST_HPP_INCLUDED

#include <xquotes_common.hpp>
#include <memory>

namespace binance_api {

    /** \brief Класс для записи потока котировок
     */
    class MqlHst {
    private:
        std::string symbol; /**< Символ */
        std::string path;   /**< Путь к файлам */
        std::fstream file;  /**< Файл данных */
        uint32_t period = 0;
        uint32_t digits = 0;
        int64_t timezone = 0;
        size_t offset = 0;
        xtime::timestamp_t last_timestamp = 0;
        bool is_open = false;

        inline void seek(const unsigned long offset, const std::ios::seekdir &origin = std::ios::beg) {
            file.clear();
            file.seekp(offset, origin);
            file.clear();
        }

        inline void write_u32(const uint32_t value) {
            file.write(reinterpret_cast<const char *>(&value), sizeof(value));
        }

        inline void write_double(const double value) {
            file.write(reinterpret_cast<const char *>(&value), sizeof(value));
        }

        inline void write_string(const std::string &value, const size_t length) {
            std::unique_ptr<char[]> buffer;
            buffer = std::unique_ptr<char[]>(new char[length]);
            char *temp = buffer.get();
            std::fill(temp, temp + length, '\0');
            const char *str_ptr = value.c_str();
            std::copy(str_ptr, str_ptr + std::min(value.size(), length - 1), temp);
            file.write(reinterpret_cast<char *>(temp), length);
        }

        template<class T>
        inline void write_array(const T *value, const size_t length) {
            file.write(reinterpret_cast<const char *>(value), length * sizeof(T));
        }

        bool create() {
            std::string file_name = path + "//" + symbol + std::to_string(period) + ".hst";
            file = std::fstream(file_name, std::ios_base::binary | std::ios::out | std::ios::trunc);
            if(!file.is_open()) return false;
            write_u32(400);
            write_string("Copyright © 2020, ELEKTRO YAR", 64);
            write_string(symbol, 12);
            write_u32(period);
            write_u32(digits);
            write_u32(0); // timesign
            write_u32(0); // last_sync
            uint32_t temp[13];
            write_array(temp, 13);
            file.flush();
            offset = file.tellp();
            return true;
        }

    public:

        MqlHst() {};

        MqlHst(
            const std::string &user_symbol,
            const std::string &user_path,
            const uint32_t user_period,
            const uint32_t user_digits,
            const int64_t user_timezone = 0) :
            symbol(user_symbol),
            path(user_path),
            period(user_period),
            digits(user_digits),
            timezone(user_timezone) {
            is_open = create();
        }

        ~MqlHst() {
            if(is_open) {
                file.flush();
                file.close();
            }
        };

        void update_candle(const xquotes_common::Candle &candle) {
            if(!is_open) return;
            seek(offset);
            write_u32((uint32_t)((int64_t)candle.timestamp + timezone));
            write_double(candle.open);
            write_double(candle.low);
            write_double(candle.high);
            write_double(candle.close);
            write_double(candle.volume);
            file.flush();
            last_timestamp = candle.timestamp;
        }

        void add_new_candle(const xquotes_common::Candle &candle) {
            if(!is_open) return;
            update_candle(candle);
            offset = file.tellp();
        }

        inline xtime::timestamp_t get_last_timestamp() {
            return last_timestamp;
        }

        inline void set_timezone(const int64_t user_timezone) {
            timezone = user_timezone;
        }
    };

}

#endif // BINANCE_CPP_API_MQL_HST_HPP_INCLUDED
