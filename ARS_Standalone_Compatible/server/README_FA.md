# راهنمای Server — تنظیمات داخل C++

منبع تنظیمات **فقط** `server/ServerConfig.cpp` است. کلاس `ServerConfig` (با متد `instance()`) بین `Startup`، `Server`، `HotColdStore`، `HotColdWorker` و `PostgresStore` مشترک است. هیچ `QSettings` و فایل INI در زمان اجرا استفاده نمی‌شود.

1. `ARS_PG_PASSWORD` را در `server/ServerConfig.cpp` با رمز واقعی PostgreSQL Role هماهنگ کن.
2. اگر Redis رمز دارد، کلید `redis/password` را در همان فایل تغییر بده.
3. از ریشه بسته `cmake -S server -B build/server && cmake --build build/server -j4` را اجرا کن.
4. `./build/server/ars_server --check` را اجرا کن. اگر همه موارد `[OK]` شد، `./build/server/ars_server` را اجرا کن.
5. برای Release، `cmake --install build/server --prefix /your/release/path` اجرا می‌شود؛ فقط فایل اجرایی منتقل می‌شود.

**دقت کن:** برای هر تغییر Config باید دوباره Build بگیری؛ ری‌استارت تنها کافی نیست. `--config` دیگر پشتیبانی نمی‌شود. TCP پیش‌فرض 7090 و UDP پیش‌فرض 7092 و هر دو روی `127.0.0.1` هستند.

برای تست مجزا با Docker، در CMake گزینه `-DARS_LAB_PROFILE=ON` را روشن کن و از `tests/reference_lab/run_lab.sh` استفاده کن؛ نسخه LAB را با Production اشتباه نگیر.

نکته امنیتی: رمز در فایل اجرایی قابل استخراج است؛ برای سرور در شبکه ایزوله مناسب‌تر از انتشار باینری به کلاینت‌هاست. Producer و Consumer رمز PostgreSQL را ندارند و با سرور بدون توکن ارتباط می‌گیرند.
