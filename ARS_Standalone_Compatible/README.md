# ARS — مستقل، TCP/UDP، بدون توکن و بدون فایل INI

سه پروژه مستقل `server/`, `producer/`, `consumer/` برای Qt 6.7 / C++17. این نسخه از ZIP ارسالی `ARS_Standalone_TCP_UDP_NoToken_INI_Qt67(1).zip` تبدیل شده است. منطق ذخیره‌سازی و پروتکل TCP/UDP در سرور باقی مانده، ولی **تنظیمات اجرایی تماماً در C++ کامپایل می‌شوند**.

## محل تنظیمات و تغییر رمز

فایل **`server/server/ServerConfig.cpp`** تنها نقطه تنظیمات سرور است. داخل همین فایل پورت TCP/UDP، میزبان‌ها، نام و رمز پایگاه داده، تنظیمات Redis، زمان عدم فعالیت ID، Checkpoint، ظرفیت صف و Recovery تعیین شده‌اند. مقدار پیش‌فرض `ARS_PG_PASSWORD` صرفاً مثال است؛ باید با رمز واقعی نقش PostgreSQL عوض شود. **تغییر تنظیمات به Build مجدد سرور نیاز دارد.**

هیچ INI در این بسته وجود ندارد، هیچ فایل تنظیماتی هنگام Build کپی نمی‌شود، و در Release هیچ فایل Config لازم نیست. `ars_server --config` حذف شده است؛ از `ars_server --check` برای عیب‌یابی استفاده کن.

## ساخت و اجرا (Qt 6.7)

```bash
cmake -S server -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server --parallel 4
cmake -S producer -B build/producer -DCMAKE_BUILD_TYPE=Release
cmake --build build/producer --parallel 4
cmake -S consumer -B build/consumer -DCMAKE_BUILD_TYPE=Release
cmake --build build/consumer --parallel 4

./build/server/ars_server --check
./build/server/ars_server
# در دو ترمینال دیگر:
./build/consumer/ars_consumer --transport tcp
./build/producer/ars_producer --transport tcp
# یا هر دو به جای tcp از udp استفاده کنند.
```

برای Release، سرور فقط باینری را نصب می‌کند:

```bash
cmake --install build/server --prefix "$PWD/ARS-Release"
# ARS-Release/bin/ars_server
```

Qt SQL و درایور QPSQL برای Server ضروری است؛ Redis و PostgreSQL نیز باید در دسترس باشند. Producer و Consumer به پایگاه داده اتصال مستقیم ندارند. هر دو پروتکل TCP و UDP فرمان‌های مشترک دارند و با `hello` بدون توکن آغاز می‌شوند.

## محیط آزمایش جداگانه

`tests/reference_lab/run_lab.sh` یک نسخه مجزای سرور با `-DARS_LAB_PROFILE=ON` می‌سازد که روی پورت‌های 7093 (TCP)، 7094 (UDP)، Redis 16379 و PostgreSQL 15432 کار می‌کند و به `ars_test` می‌نویسد. هیچ تست یکپارچه‌ای را روی پایگاه داده عملیاتی اجرا نکن.

## ابزارهای مدیریت

`server/scripts/` همچنان شامل Backup، Provisioning و Purge است. چون رمز کامپایل‌شده عمداً از بیرون قابل خواندن نیست، این **ابزارهای مستقل مدیریتی** باید رمز PostgreSQL را هنگام اجرا دریافت کنند (متغیر `PGPASSWORD` یا ورودی تعاملی)؛ هیچ‌یک فایل INI نمی‌خوانند. Purge نیاز به تأیید صریح و مشخص‌کردن تمام مقصدها دارد.

## محدودیت امنیتی مهم

**قرار دادن رمز در باینری به معنای رمزنگاری یا امنیت مطلق آن نیست.** کسی که به باینری یا حافظه فرایند دسترسی دارد، ممکن است رمز را با ابزارهای تحلیل استخراج کند. برای استقرار امن، شبکه سرور را ایزوله نگه دار، به دیتابیس مجوز محدود بده و باینری دارای رمز را به کاربران غیرقابل‌اعتماد منتشر نکن. با حذف توکن ARS، هر سیستم دارای دسترسی شبکه می‌تواند عملیات مدیریتی API از جمله حذف را فراخوانی کند.

راهنمای بیشتر: `docs/13_COMPILED_CONFIG_FA.md` و `docs/14_COMPILED_CHANGELOG_TEST_FA.md` و دیاگرام‌ها در `docs/sequences/`.
