> **مرجع تاریخی پروژه اصلی**: ممکن است از INI یا توکن استفاده کند؛ نسخه اجرایی فعلی مطابق این متن نیست.

# وضعیت آزمون‌های نسخهٔ Redis-first

## اجراشده در محیط تهیهٔ این نسخه

- `python3 -m unittest discover -s central/tests -p 'test_*.py' -v` — چهار آزمون آماده‌سازی PostgreSQL: **موفق**.
- بررسی ساختار فایل‌های کد و کنترل‌های ابتدایی: انجام شده.

## آزمون‌هایی که باید در محیط دارای Qt / Redis / PostgreSQL اجرا شوند

در محیط تهیهٔ فایل، Qt SDK و سرویس‌های Redis/PostgreSQL موجود نبودند؛ بنابراین **کامپایل C++، آزمون یکپارچهٔ پروتکل و آزمون فشار واقعی این نسخه انجام نشده است**. ادعای تست‌شدن runtime نداریم.

1. اجرای `cmake -S . -B build` و `cmake --build build --parallel` در ریشهٔ پروژه
2. اجرای `python3 central/tests/integration.py` روی محیط آزمایشی
3. با `inactiveAfterSec=2` و `scanEveryMs=200` (محیط اختصاصی):
   `ARS_RUN_LIFECYCLE=1 python3 central/tests/integration.py`
4. قطع Redis، بازگشت PostgreSQL-only، **تطبیق تاریخچهٔ Redis/PG پس از restart** و تنظیم Redis AOF را به‌صورت عملی بررسی کنید. در این نسخه reconciliation دو مخزن پس از outage کاملاً خودکار نیست.
5. بنچمارک throughput و peak RAM با اندازهٔ واقعی داده‌های پروژه را حتماً انجام دهید.

گزارش آزمون موفق نسخهٔ قدیمی `TEST_REPORT_PREVIOUS_FA.md` نگهداری شده و **نباید** به نسخهٔ جدید تعمیم داده شود.

## به‌روزرسانی: برنامه‌های تست مستقل

در نسخه دارای `ars_test_sender` و `ars_test_receiver`، فایل `tests/hot_cold_end_to_end.py`
برای آزمون چرخه واقعی با دو کلاینت، ID تکراری، checkpoint، آرشیو ID ساکت،
rehydration، pagination و حذف اضافه شده است. این فایل در این محیط فقط از نظر
syntax بررسی شده و **بدون Redis/PostgreSQL واقعی اجرا نشده است**.
راهنمای اجرا: `TEST_SCENARIOS_FA.md`.
