> **مستند قدیمی**: مسیرهای INI/Build در این سند منسوخ‌اند. به `tests/reference_lab/run_lab.sh` جدید و `docs/13_COMPILED_CONFIG_FA.md` مراجعه کن.

# راهنمای اتصال پروژه Qt مستقل به سرویس ARS (از صفر)

## ۱. اول سرویس مرکزی کجا اجرا می‌شود؟

ارسال/خواندن همه داده‌ها از `ars_server` انجام می‌شود. سرور فقط یک‌بار، روی ماشین مرکزی اجرا می‌شود و Redis و PostgreSQL در همان شبکه خصوصی در اختیار آن هستند. **پروژه Qt جدید هیچ‌وقت مستقیم به Redis یا SQL وصل نمی‌شود.** برای آزمایش روی یک ماشین، سرور ARS در پورت 7093 آماده است. در استقرار واقعی پورت پیش‌فرض فایل `central/config/server.ini` برابر 7090 است و باید دسترسی شبکه، token و Firewall کنترل شوند.

## ۲. برنامه نمونه کاملاً مستقل

پوشه `examples/independent_qt_app` را از ZIP کپی کن؛ این پوشه **به بقیه پروژه وابسته نیست**:

```text
independent_qt_app/
├── CMakeLists.txt
├── IndependentArsApp.pro
├── main.cpp
├── TelemetryBridge.h
├── TelemetryBridge.cpp
└── vendor/ars/
    ├── client/ArsClient.h
    ├── client/ArsClient.cpp
    ├── model/ArsDataRecord.cpp
    └── include/ARS/ArsDataRecord.h, ArsGlobal.h
```

در Qt Creator می‌توان `CMakeLists.txt` یا `IndependentArsApp.pro` را باز کرد. Qt5/Qt6 با C++17 و ماژول‌های Core و Network لازم است. Qt SQL برای برنامه مستقل **لازم نیست**.

## ۳. ساخت پروژه نمونه با CMake

ترمینال شماره ۱ باید `ars_server` آزمایش را اجرا کند. برای اجرای مستقل با توکن همان آزمایش:

```bash
cd redis_service
export ARS_API_TOKEN="$(cat central/lab/.lab-token)"
cd examples/independent_qt_app
cmake -S . -B build
cmake --build build --parallel
./build/independent_ars_app
```

**یا با qmake:**

```bash
cd redis_service
export ARS_API_TOKEN="$(cat central/lab/.lab-token)"
cd examples/independent_qt_app
qmake IndependentArsApp.pro
make -j"$(nproc)"
./independent_ars_app
```

انتظار: `Authenticated`، سپس `RESULT subscribe`، `RESULT publish`، `LIVE ...`، `RESULT latest`، `RESULT recent`، `RESULT search_hot` و `RESULT status`. برنامه بعد از ۸ ثانیه خارج می‌شود ولی ID آزمایشی را برای بررسی در DB نگه می‌دارد. تمام عملیات **غیرهمگام** هستند و ترتیب رسیدن پاسخ‌ها صرفاً با requestId مشخص می‌شود.

## ۴. می‌خواهم فقط SDK را به یک پروژه موجود Qt اضافه کنم

۱. داخل پروژه خودت یک پوشه مثل `third_party/ars/` بساز. از نمونه مستقل، `vendor/ars/client/`، `vendor/ars/model/` و `vendor/ars/include/` را در همین پوشه کپی کن. **لازم نیست** فایل‌های `Server`, `HotColdStore`, `PostgresStore` و `ArsRedisClient` را کپی کنی.

۲. در qmake این خطوط را به `.pro` فعلی اضافه کن:

```qmake
QT += core network
CONFIG += c++17
DEFINES += ARS_STATIC
INCLUDEPATH += $$PWD/third_party/ars/client $$PWD/third_party/ars/include
SOURCES += \
    $$PWD/third_party/ars/client/ArsClient.cpp \
    $$PWD/third_party/ars/model/ArsDataRecord.cpp
HEADERS += \
    $$PWD/third_party/ars/client/ArsClient.h \
    $$PWD/third_party/ars/include/ARS/ArsDataRecord.h \
    $$PWD/third_party/ars/include/ARS/ArsGlobal.h
```

بعد از ویرایش فایل `.pro` در Qt Creator دستور `Build > Run qmake` را اجرا کن؛ سپس `Clean` و `Rebuild`. حتماً kit با پشتیبانی C++17 انتخاب کن.

۳. اگر پروژه CMake است، در `CMakeLists.txt` بخش‌های زیر را با `target` واقعی خودت تطبیق بده:

```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)
target_sources(MyExistingApp PRIVATE
    third_party/ars/client/ArsClient.cpp
    third_party/ars/model/ArsDataRecord.cpp
    third_party/ars/client/ArsClient.h
)
target_include_directories(MyExistingApp PRIVATE
    third_party/ars/client third_party/ars/include)
target_compile_definitions(MyExistingApp PRIVATE ARS_STATIC)
target_link_libraries(MyExistingApp PRIVATE
    Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
```

۴. در کد اصلی، یک عضو `ArsClient` بساز، سیگنال‌ها را وصل کن و **تنها پس از سیگنال `ready()`** درخواست بفرست:

```cpp
#include "ArsClient.h"
#include <ARS/ArsDataRecord.h>
#include <QDebug>

// In one of your QObject's methods or constructor (e.g. MainWindow):
auto* ars = new ArsClient(this);
connect(ars, &ArsClient::ready, this, [ars] {
    ars->subscribeLive("sensors/live");
    ArsDataRecord r("RADAR-42");
    r.setSource("MyQtApp").setMotion(245.0,90.0).addFeature("quality",0.98);
    ars->publishLive("sensors/live",r);
    ars->recentHistory("RADAR-42",1800,50); // Previous 30 minutes
});
connect(ars, &ArsClient::responseReceived, this,
    [](QString requestId,QString operation,QJsonObject result) {
        qDebug()<<"ARS response"<<requestId<<operation<<result;
    });
connect(ars, &ArsClient::liveRecordDetailed, this,
    [](QString channel,ArsDataRecord record,QString sequence,qint64 receivedAtMs) {
        qDebug()<<"New live data"<<channel<<record.id<<sequence<<receivedAtMs;
    });
connect(ars, &ArsClient::requestFailed, this,
    [](QString id,QString op,QString error){qWarning()<<"ARS failed"<<id<<op<<error;});
ars->connectToServer("127.0.0.1",7093,qEnvironmentVariable("ARS_API_TOKEN"));
```

در مثال بالا `this` باید واقعاً `QObject` باشد؛ اگر پروژه تو کلاس Qt دیگری دارد، همان کلاس را به‌عنوان parent/context استفاده کن. `ArsClient` را در همان Thread Qt مالک خودش صدا بزن؛ از Thread دیگر از Queue/Signal استفاده کن. برای پروژه واقعی توکن را امن بارگذاری کن و داخل سورس Hardcode نکن.

## ۵. استفاده از همه APIها در پروژه جدید

```cpp
// Write (without live notification)
ArsDataRecord r("RADAR-42");
r.addFeature("frequency",2500).addFeature("power",10.5);
ars->saveRecord(r);

// Latest
ars->getLatest("RADAR-42");

// Last 30 minutes and 1 hour by SERVER receipt time
ars->recentHistory("RADAR-42",1800,50);
ars->recentHistory("RADAR-42",3600,50);

// Search latest statuses of all IDs in Redis where frequency >= 2500
QJsonArray conditions{QJsonObject{{"field","features.frequency"},{"op","gte"},{"value",2500}}};
ars->searchHot("latest",conditions,"", "0:0",20,120);
// To search historical records in RAM instead: mode="records".

// Full history with an event-time window (toMs=-1 means no upper bound)
ars->queryHistory("RADAR-42",0,-1,100,"timestamp");
// Use "receivedAt" as timeField to filter by receipt time.

// Evict Redis cache but retain SQL history
ars->deleteById("RADAR-42",false);

// Explicitly restore archived history from SQL
ars->hydrateToCache("RADAR-42");

// DESTRUCTIVE: remove the ID from both Redis and PostgreSQL
// ars->deleteById("RADAR-42",true);
ars->status();
```

**صفحه‌بندی:** در نتیجه `search_hot` مقادیر `hasMore`, `nextCursor` را می‌خوانی و همان cursor را برای درخواست بعدی می‌فرستی. برای `recent` مقدار `fromMs`, `toMs`, `nextOffset` را دقیقاً حفظ کن و `recentHistoryPage(id,from,to,offset,limit)` بزن؛ در غیر این صورت هر بار پنجره زمانی حرکت می‌کند. برای `history` `untilSequence` اولین صفحه را ثابت و `afterSequence` صفحه قبلی را ادامه بده.

## ۶. چرا من TelemetryBridge ساخته‌ام؟

`TelemetryBridge` داخل `examples/independent_qt_app/TelemetryBridge.h/.cpp` یک Wrapper آموزشی است؛ راه اتصال شبکه و پاسخ‌ها را از منطق دامنه اپلیکیشن جدا می‌کند. در MainWindow فقط با `writeMeasurement()`, `historyLast30Minutes()`, `searchActiveMeasurements()` کار می‌کنی. در پروژه واقعی می‌توانی این نام‌ها را متناسب با دامنه مثل `RadarDataService` تغییر دهی.

`TelemetryBridge` دارای سیگنال‌های `connected`, `liveMeasurement`, `result`, `failure` است و یک شیء `ArsClient` دارد. این فایل‌ها **جزء کتابخانه اصلی ARS نیستند** و تنها نمونه ادغام‌اند.

## ۷. استقرار روی یک ماشین دیگر

- در سرور اصلی ARS تنظیم `server/host` و `server/port` را با IP داخلی مناسب تنظیم کن؛ اگر `127.0.0.1` بماند، اتصال از دستگاه دیگر برقرار نمی‌شود.
- در پروژه مستقل `ars->connectToServer("SERVER_LAN_IP",7090,token)` را تنظیم کن. Token سرور و کلاینت باید یکسان باشد.
- Redis/PostgreSQL را به کاربران نهایی expose نکن؛ فقط سرور ARS به آن‌ها متصل شود و فایروال پورت ARS را فقط برای شبکه مورداعتماد مجاز کند.
- پروتکل فعلی TCP خام + Token مشترک است، TLS و RBAC داخلی ندارد؛ برای استفاده در شبکه خارج از دستگاه مورداعتماد، قبل از deploy باید TLS و کنترل دسترسی نقش‌ها/عملیات اضافه شود.
- کتابخانه و پروژه مستقل به اتصال اینترنت نیاز ندارند، اما Qt runtime/کتابخانه‌های موردنیاز باید به‌صورت آفلاین در دستگاه مقصد فراهم باشند.

## سازگاری نسخه جدید
متدهای قبلی ArsClient حفظ شده‌اند. SDK مثال مستقل با نسخه جدید همگام است. متد recentHistoryToday(id, limit) برای بازه امروزِ منطقه زمانی کلاینت اضافه شده است. خطای اتصال اکنون آدرس ARS و راهنمای --check را نشان می‌دهد. --check باید روی ماشین سرور اجرا شود. برای جزئیات central/STARTUP_AND_CHANGES_FA.md را بخوانید.
