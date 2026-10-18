# ESP32 Gamepad Bridge — C++ / Qt

Нативное Windows-приложение для приёма Bluetooth HID-отчётов от ESP32 по
USB/UART и отображения двух виртуальных DualShock 4 через ViGEm. Приложение
сохраняет бинарный протокол версии 2 и загружает JSON-профили геймпадов.

## Что перенесено

- Неблокирующая работа с COM-портом через `QSerialPort`; чтение запускается
  сигналом `readyRead`, без постоянного опроса таймером.
- Кадрирование, проверка CRC-16/CCITT-FALSE, обработка статусов двух слотов,
  pairing и HID-output-команды с теми же форматами, что и в Python-версии.
- Загрузка JSON-профилей; маппинг осей, кнопок и D-pad; построение output-отчётов
  подсветки и вибрации с CRC32.
- Два отдельных виртуальных DS4. Для Windows используется нативный
  `vigem_target_ds4_update_ex`, чтобы передавать 63-байтовый extended report,
  включая состояние и координаты сенсорной панели. Это исправляет пропуск
  extended report в прежнем Python-адаптере.
- HID-дамп JSONL по запросу. Чтобы не тратить процессор на GUI-обновления,
  каждый входящий пакет не добавляется в визуальный лог; показывается последний
  отчёт, а подробная запись включается отдельно.

## Сборка и запуск на Windows

1. Установите Qt 6 (модули **Qt Widgets** и **Qt Serial Port**) и CMake.
2. Установите ViGEmBus. Для приложения нужна совместимая `ViGEmClient.dll`;
   положите её рядом с `esp32-gamepad-bridge.exe` либо в системный путь.
3. Соберите:

   ```powershell
   cmake -S desktop/esp32-gamepad-bridge -B desktop/esp32-gamepad-bridge/build `
     -DCMAKE_PREFIX_PATH="C:\Qt\6.x.x\msvc2022_64"
   cmake --build desktop/esp32-gamepad-bridge/build --config Release
   ```

4. Разверните Qt DLL командой `windeployqt` и оставьте папку `profiles` рядом
   с EXE. Запустите приложение, выберите COM-порт ESP32 и подключитесь.

Поддерживаемый транспорт: 921600 бод, 8N1, без flow control. Не открывайте
тот же порт одновременно в Serial Monitor и приложении.

## Проверки

На машине с Qt 6 Core:

```sh
cmake -S desktop/esp32-gamepad-bridge -B desktop/esp32-gamepad-bridge/build
cmake --build desktop/esp32-gamepad-bridge/build
ctest --test-dir desktop/esp32-gamepad-bridge/build --output-on-failure
```

## Ограничения проверки оборудования

В переданном наборе не было исходника прошивки ESP32. Поэтому нельзя проверить,
что она принимает `HID_OUTPUT` и отправляет этот отчёт именно тому Bluetooth
устройству, а также подтвердить смещения световой панели для конкретной модели
и ревизии контроллера. В профиле оставлены значения из прежней версии; для
физической проверки цвета потребуется прошивка и точная модель геймпада.
