> **توجه:** این گزارش مربوط به قبل از حذف توکن است؛ گزارش معتبر تغییرات جدید در `10_CHANGELOG_TEST_REPORT_FA.md` قرار دارد.

# گزارش اجرای واقعی تست در محیط تهیه سورس

تاریخ: ۲۰۲۶-۱۱-۱۸. هدف: جداسازی Server، Producer و Consumer و یکسان‌سازی دستورات در TCP/UDP.

| آزمون | نتیجه قابل تأیید | محدودیت |
|---|---|---|
| `python3 -m compileall` روی اسکریپت‌ها | موفق | فقط بررسی Syntax پایتون |
| `tests/ported`: TestSuite مرجع | **۴۴ تست: ۱۶ Pass، ۲۸ Skip، ۰ Fail** | Redis binary و ARS server ساخته‌شده/تنظیم‌شده حاضر نبودند |
| `tests/test_dual_source_contract.py` | **۴ Pass** | فقط بررسی وجود مسیر و API در سورس؛ runtime نیست |
| CMake configure روی server | **ناموفق: `Qt5Config.cmake` و `Qt6Config.cmake` وجود نداشتند** | نمی‌توان ادعای Build قابل اجرا کرد |
| اجرای Qt producer/consumer | انجام نشد | به Qt SDK و server واقعی نیاز دارد |
| Redis Lua tests | Skip | `redis-server` موجود نبود |
| PostgreSQL End-to-End | انجام نشد | PostgreSQL و پلاگین QPSQL قابل اجرا نبود |
| `tests/udp_tcp_e2e.py` | **فقط تهیه شد، اجرا نشد** | نیازمند بانک آزمایشی و server واقعی است |
| UDP packet-loss/reorder، fragmentation، subscribe lease، recovery | اجرا نشد | قبل از پذیرش Production الزامی است |

## معیار پذیرش پروژه

- [x] جداسازی سورس Server / Producer / Consumer و Build پروژه‌ای مستقل.
- [x] مسیر مشترک `handleRequest` برای فرمان‌های مرجع TCP و UDP در سورس.
- [x] انتخاب پروتکل توسط هر کلاینت از ابتدا (`--transport tcp|udp` یا enum `Transport`).
- [x] UDP subscribe/heartbeat/ack/fragmentation/retry/dedup در سورس.
- [x] حفظ کد Hot/Cold، Redis و PostgreSQL مرجع در هسته سرور.
- [x] ماتریس قابلیت‌ها و پنج Sequence Diagram مبتنی بر مسیرهای کد.
- [x] حفظ تست‌های اصلی و ارائه تست تطبیقی جدید.
- [ ] کامپایل Qt بدون خطا روی Ubuntu 22.04 یا محیط هدف.
- [ ] گذراندن تست‌های مرجع و E2E دو پروتکل روی Redis/PostgreSQL واقعی.
- [ ] تست فقدان packet و ارتباط طولانی و benchmark و بازیابی پس از crash.
- [ ] تأیید موارد امنیتی مانند حمله بازپخش و عدم افشای توکن در شبکه.

**وضعیت تحویل: کاندیدای سورس برای تکمیل اعتبارسنجی، نه نسخه عملیاتی تأییدشده.** عبارت «پشتیبانی در سورس» با «قبولی عملی در محیط هدف» برابر نیست. هیچ نتیجه تستی که انجام نشده باشد، Passed گزارش نشده است.
