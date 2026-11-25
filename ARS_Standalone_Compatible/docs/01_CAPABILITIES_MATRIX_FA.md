# استخراج قابلیت‌های نسخه مرجع و ماتریس تطبیق

## روش استخراج

مرجع اصلی: `ARS_Integration_Training_Complete(1).zip`، مسیرهای `redis_service/central/server/Server.cpp`، `client/ArsClient.{h,cpp}`، `tests/`، `examples/all_features.cpp`، `lab/test_contract.py` و `test_apps/sender.cpp`, `receiver.cpp`. پروژه شروع بازطراحی: `ARS_Redis_Service_Production_Server_Only_v2.zip`، پوشه `redis_service/server` و `src/Redis`. PDF تاریخچه گفتگو برای الزامات داده گرم/سرد و نیازهای سناریوی کاربر بررسی شد، اما **سورس و تست‌های مرجع مبنای رفتار پیاده‌سازی‌اند**.

دو حالت را از هم جدا می‌کنیم: **وجود مسیر در سورس** و **اثبات موفقیت آزمون عملی**. ستون TCP/UDP نشان‌دهنده پیاده‌سازی مسیر در سورس جدید است و به معنی تأیید کامپایل یا تست شبکه نیست.

| قابلیت/دستور مرجع | مرجع | نسخه جدید TCP | نسخه جدید UDP | نحوه اعتبارسنجی |
|---|---|---|---|---|
| `hello` توکن مشترک | بله | کد موجود | کد مشترک + توکن هر درخواست | تست E2E لازم |
| `status` سلامت Redis/SQL/worker | بله | حفظ | حفظ | تست ایستا؛ E2E لازم |
| `save` و حفظ IDهای تکراری/sequence | بله | حفظ | حفظ + dedup درخواست موقت | E2E لازم |
| `publish`، تأیید ذخیره و انتشار live | بله | حفظ | حفظ، انتشار TCP/UDP | E2E لازم |
| `subscribe` / `unsubscribe` | بله | حفظ | اضافه شده با lease و Heartbeat | E2E لازم |
| `latest` آخرین داده ID | بله | حفظ | حفظ | E2E لازم |
| `history` کامل + `afterSequence` و `untilSequence` | بله | حفظ | حفظ + پاسخ چندبخشی | E2E لازم |
| `recent` بر اساس زمان دریافت سرور | بله | حفظ | حفظ + پاسخ چندبخشی | E2E لازم |
| `search_hot` روی آخرین/all records در Redis | بله | حفظ | حفظ | E2E لازم |
| فیلترها `eq/ne/gt/gte/lt/lte/contains` و AND | بله | حفظ | حفظ | E2E لازم |
| فیلتر زمانی `fromMs/toMs/timeField` | بله | حفظ | حفظ | E2E لازم |
| صفحه‌بندی `limit`, `offset`, `cursor` | بله | حفظ | حفظ | E2E لازم |
| `delete` از Redis+SQL | بله | حفظ | حفظ | آزمون تخریبی در lab لازم |
| `delete(..., removeFromDisk=false)` / evict | بله | حفظ | حفظ | آزمون lab لازم |
| `hydrate` / `recover` آرشیو به Redis | بله | حفظ | حفظ | E2E لازم |
| `recovery_status` روند بازیابی | بله | حفظ | حفظ | E2E لازم |
| `PostgresStore` تاریخچه و آرشیو سرد | بله | کد هسته حفظ | همان هسته | آزمون بانک واقعی لازم |
| `HotColdWorker`: inactive, checkpoint, recovery | بله | کد هسته حفظ | همان هسته | ۸ تست ایستای Worker موفق؛ runtime لازم |
| ثبت event time و `receivedAtMs` جداگانه | بله | حفظ | حفظ | E2E لازم |
| محدودیت `maxActiveIds` و خطای ظرفیت | بله | حفظ | حفظ | source-contract موفق؛ runtime لازم |
| برخورد با failure Redis / SQL، recovery pending | بله | حفظ | حفظ | source-contract موفق؛ runtime لازم |
| بکاپ PostgreSQL و snapshot Redis | بله | اسکریپت سرور | مستقل از پروتکل | تست mock موفق؛ runtime لازم |
| `purge_archive` با تأیید صریح | بله | اسکریپت سرور | مستقل از پروتکل | تست mock موفق |
| اتصال و ارسال داده آزمایشی مستقل | در مرجع همراه پروژه | `producer/` مستقل | `producer/` مستقل | Build لازم |
| دریافت live و CLI query مستقل | در مرجع همراه پروژه | `consumer/` مستقل | `consumer/` مستقل | Build لازم |
| **UDP همه فرمان‌های مرجع** | خیر / ناقص در جداشده | — | **پیاده‌سازی مسیر مشترک** | تست ایستا موفق؛ E2E لازم |
| **UDP fragmentation/reassembly** | خیر | — | پیاده‌سازی | تست ایستا موفق؛ تست loss لازم |
| **UDP retry/dedup response/request** | خیر | — | محدود به endpoint+requestId و ۱۲۰ثانیه | تست ایستا موفق؛ بار/خرابی لازم |
| **UDP live ACK/retry و lease** | خیر | — | پیاده‌سازی | تست ایستا موفق؛ loss test لازم |
| **TCP reconnect / UDP heartbeat** | TCP SDK اولیه قطع را گزارش می‌کرد | SDK تلاش مجدد | SDK heartbeat+تلاش مجدد | تست قطع شبکه لازم |

## قواعد مهمی که حفظ شده‌اند

1. `save` فقط ذخیره می‌کند؛ `publish` در صورت ذخیره موفق `event=record` به مشترک‌ها می‌فرستد.
2. `ok=true` برای `save/publish` به معنی ذخیره موفق است. اگر ID سرد باشد ابتدا بازیابی صف می‌شود و پاسخ تا رسیدن نتیجه نگه داشته می‌شود. اگر زمان پاسخ بگذرد `RECOVERY_PENDING` یعنی آن درخواست ذخیره نشده است؛ ولی در قطع ارتباط نتیجه می‌تواند نامعلوم باشد.
3. `latest` مبتنی بر timestamp رخداد است، `recent` مبتنی بر زمان ورود به **سرور**؛ `history` بر حسب توالی رکوردها است.
4. آرشیو حذف نمی‌شود تا checkpoint موفق انجام شده باشد. برای حذف کامل `removeFromDisk=true` لازم است.
5. `search_hot` پیمایش محدود و صفحه‌ای داده‌های موجود در RAM است، نه جست‌وجوی کامل PostgreSQL و نه ایندکس موتورهای Search.
6. فایل تست‌های مرجع برای مقایسه در `tests/reference_original` بدون تغییر نگهداری شده و نسخه پورت‌شده تست‌ها در `tests/ported` موجود است.

## نکته پذیرش

اجرای ۱۶ تست غیرشبکه‌ای پورت‌شده و ۴ بررسی جدید سورس موفق شد؛ ۲۸ تست مرجع به علت نبود Redis، سرور ساخته‌شده و Qt/SQL **Skip** شدند. **بدون Build موفق Qt و اجرای تست E2E روی Redis/PostgreSQL نمی‌توان برچسب «تأیید عملی کامل» به هیچ قابلیت شبکه‌ای زد.**
