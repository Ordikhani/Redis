> **بایگانی تاریخی**: این راهنما متعلق به نسخه INI قبلی است؛ برای نسخه فعلی از `docs/13_COMPILED_CONFIG_FA.md` استفاده کنید.

# گزارش تغییرات و تست — نسخه بدون توکن + PostgreSQL INI

## مبنا

فایل ارسالی کاربر: `ARS_Standalone_TCP_UDP_Full_Source(1).zip`. تغییرات فقط در یک کپی مستقل اعمال شده‌اند و ZIP ورودی دست‌نخورده مانده است.

## اصلاحات

| بخش | تغییر | محل |
|---|---|---|
| Server API | حذف API Token، حذف Credentials، حذف بررسی token، استفاده از `hello` بدون رمز | `server/server/{main.cpp,Server.cpp,Server.h}` |
| Producer/Consumer | حذف آرگومان token از API اتصال، حذف env/اعتبارسنجی توکن؛ hello خودکار در هر دو پروتکل | `producer/`, `consumer/` |
| PostgreSQL | خواندن مستقیم رمز از `postgres/password` در INI، شامل Startup/Store/Worker | `server/config/server.ini`, `server/server/{Startup.cpp,PostgresStore.cpp}` |
| Redis | دریافت `redis/password` اختیاری از همان INI به‌جای env/file | `server/server/{Startup.cpp,HotColdStore.cpp,HotColdWorker.cpp}` |
| ابزارهای مدیریتی | backup، purge و init_database به‌جای فایل جداگانه از INI استفاده می‌کنند | `server/scripts/` |
| CMake | حذف Credentials و فایل رمز جداگانه از Build؛ محافظت از server.ini با chmod 600 در staging/installation | `server/CMakeLists.txt`, `server/cmake/StageRuntimeConfig.cmake` |
| تست | تبدیل کد تست ported به `hello`، به‌روزرسانی TCP/UDP e2e و افزودن test_noauth_ini_contract | `tests/ported`, `tests/udp_tcp_e2e.py`, `tests/test_noauth_ini_contract.py` |
| مستندات | به‌روزرسانی راهنمای اتصال و ۵ Mermaid Diagram | `docs/` |

## وضعیت واقعی تست در محیط تهیه

- `python -m compileall -q server/scripts tests/ported` — موفق (Syntax validation).
- `python -m unittest discover -s tests -p 'test_*.py' -v` — **۸ تست موفق، ۰ خطا** (ایستا/قرارداد سورس).
- `python -m unittest discover -s tests/ported -p 'test_*.py' -v` — **۱۶ تست موفق، ۲۸ Skip، ۰ خطا**.
- Skipها مربوط به نبود Redis Server محلی یا ARS binary و سرویس‌های آزمایشی است.
- **Qt 6.7 SDK در این محیط موجود نبود**؛ این بسته با Qt 6.7 کامپایل/لینک نشده است.
- **تست واقعی شبکه با Redis/PostgreSQL و سناریوهای Lifetime/Archive/Restore انجام نشده**؛ نتایج آنها هنوز باید روی سیستم هدف تأیید شوند.
- بعضی تست‌های مرجع مربوط به «توکن اشتباه رد شود» به‌دلیل حذف عمدی احراز هویت دیگر قابل اعمال نیستند؛ این یک تغییر قرارداد است، نه Pass شدن آن تست.

## اعمال تغییر و استقرار

1. رمز واقعی role `ars_app` را داخل کپی فعال `server/build/server.ini` در `[postgres] password=` جایگزین کنید؛ خود برنامه رمز PostgreSQL را ایجاد یا تغییر نمی‌دهد.
2. `chmod 600 server/build/server.ini` انجام دهید؛ دسترسی API چون بدون auth است باید توسط localhost/firewall محدود شود.
3. پس از build با Qt6.7، `./server/build/ars_server --check --config server/build/server.ini` را اجرا کنید؛ سپس اجرای عادی.
4. Producer و Consumer از `--transport tcp --port 7090` یا `--transport udp --port 7092` استفاده می‌کنند و هیچ `ARS_API_TOKEN` لازم نیست.
5. اگر `server/build/server.ini` از Build قبلی باقی مانده، CMake آن را بازنویسی نمی‌کند: کلید `passwordFile` را حذف و `[postgres] password=` را اضافه کنید یا بعد از پشتیبان‌گیری Config قبلی، staging جدید انجام دهید.

## هشدار

هر کلاینتی که به Listener دسترسی شبکه داشته باشد توانایی عملیات ذخیره، خواندن و حذف اطلاعات را خواهد داشت. نسخه بدون token فقط برای localhost یا LAN کنترل‌شده با Firewall مناسب است. متن همراه بسته حاوی رمز واقعی نیست؛ مقدار `password` یک نمونه است.
