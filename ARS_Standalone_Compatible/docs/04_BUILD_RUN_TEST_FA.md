# راهنمای عملی تنظیمات کامپایل‌شده (Qt 6.7)

## فایل‌های تغییرکرده

| فایل | تغییر |
|---|---|
| `server/server/ServerConfig.h` | کلاس Singleton فقط‌خواندنی |
| `server/server/ServerConfig.cpp` | **همه مقادیر پیش‌فرض** TCP، UDP، PostgreSQL، Redis، Lifecycle و Recovery |
| `server/server/Startup.cpp` | بررسی اتصال بدون خواندن فایل |
| `server/server/Server.cpp` | بازکردن Listenerها با همان کلاس |
| `server/server/HotColdStore.cpp` | دریافت مقدار مشترک و استفاده هنگام اتصال مجدد SQL |
| `server/server/HotColdWorker.cpp` | اتصال مستقل Worker با همان تنظیمات کامپایل‌شده |
| `server/server/PostgresStore.cpp` | تنظیم QPSQL با ServerConfig |
| `server/CMakeLists.txt` | حذف کامل کپی/نصب INI، امکان پروفایل آزمایشگاه |

## کجا مقادیر را عوض کنم؟

در `server/server/ServerConfig.cpp` مقادیر `ARS_PG_PASSWORD`, `ARS_PG_PORT`, `ARS_DB_NAME`, `ARS_TCP_PORT`, `ARS_UDP_PORT`, `ARS_REDIS_PORT`, `ARS_IDLE_SEC` و `ARS_CHECKPOINT_SEC` برای حالت عادی تعریف شده‌اند. مقادیر دیگر مانند `redis/password`، `lifecycle/batchSize` و `recovery/ackTimeoutMs` در جدول `defaults()` همین فایل هستند.

رمز `ARS_PG_PASSWORD` باید دقیقاً با PostgreSQL role `ars_app` یکسان باشد. تغییر آن در سورس، رمز واقعی Role را **عوض نمی‌کند**. اگر لازم بود، با حساب مدیر دیتابیس دستور `\password ars_app` را داخل `psql` اجرا کن و رمز را به مقدار جدید برسان. هنگام Build تغییرات C++ لازم است؛ بعد از Build باینری جدید را جایگزین کن و سرور را Restart کن.

## ساخت با Qt 6.7

```bash
cmake -S server -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server -j4
./build/server/ars_server --check
./build/server/ars_server
```

در Qt Creator فایل `server/CMakeLists.txt` را باز کن و Kit `Desktop Qt 6.7.x` را انتخاب کن. برای Producer و Consumer نیز CMake مجزا هست. برای اتصال از ماشین دیگر، `server/host` و `udp/host` را در همان CPP روی IP مشخص سرور تنظیم کن، پروژه را Build کن و در فایروال دسترسی را محدود کن.

## ساخت Release

```bash
cmake --install build/server --prefix "$PWD/release"
```

خروجی `release/bin/ars_server` است؛ **هیچ INI کنار برنامه نیاز نیست**. کتابخانه‌های Runtime Qt6 (از جمله QPSQL) و Redis/PostgreSQL باید در محیط مقصد فراهم باشند.

## تست خودکار بدون استفاده از دیتابیس واقعی

```bash
python3 -m unittest discover -s tests -p 'test_compiled_config_contract.py' -v
python3 -m unittest discover -s tests -p 'test_dual_source_contract.py' -v
```

این‌ها فقط تست‌های ساختار سورس هستند و **به معنی موفقیت Build یا اتصال واقعی نیستند**.

## تست آزمایشگاهی مجزا

در صورت نصب Qt 6.7 و Docker:

```bash
./tests/reference_lab/run_lab.sh up
./tests/reference_lab/run_lab.sh build
./tests/reference_lab/run_lab.sh check
./tests/reference_lab/run_lab.sh server
```

در ترمینال دیگر:

```bash
./tests/reference_lab/run_lab.sh tcp-test
```

پروفایل LAB با `ARS_LAB_PROFILE=ON` در `ServerConfig.cpp` تعریف شده و با کانتینرهای آزمایشی سازگار است. پورت‌های آن 7093/TCP، 7094/UDP، 15432/PostgreSQL و 16379/Redis هستند؛ TTL عدم فعالیت ۸ ثانیه است. **در تولید واقعی ARS_LAB_PROFILE را OFF بگذار.**

## ابزارهای مدیریتی

```bash
python3 server/scripts/backup_database.py --help
python3 server/scripts/backup_all.py --help
python3 server/scripts/init_database.py --help
python3 server/scripts/purge_archive.py --help
```

این ابزارها برای عمل مدیریتی به ورودی‌های جداگانه یا `PGPASSWORD` نیاز دارند؛ سرور از محیط/INI رمز نمی‌خواند و هیچ API برای بیرون‌دادن رمز باینری وجود ندارد. برای Purge باید همه مقصدهای SQL و Redis را صریح مشخص کنی؛ هیچ میان‌بر حذف دیتابیس اصلی وجود ندارد.

## امنیت

رمز ذخیره‌شده در CPP ممکن است در خروجی executable و حافظه قابل مشاهده باشد. همچنین نبود احراز هویت TCP/UDP برای شبکه عمومی مناسب نیست. رمز را در Git عمومی ثبت نکن و باینری سرور را به اشخاص غیرقابل اعتماد نده.
