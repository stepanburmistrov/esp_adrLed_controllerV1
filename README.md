# ESP32 LED Controller 

**Управление WS2812B через Web + HTTP API + Telegram (inline-меню + палитра).**
Все настройки — в NVS; конфиг через UART **без перепрошивки**.
![Веб-интерфейс (mobile)](images/web-ui-mobile.png)

## Структура
```
firmware/ESP32_LED_Controller.ino  # основная прошивка (Arduino IDE, ESP32 core 2.x)
docs/                              # картинки, гайды
tools/                             # примеры запросов и шпаргалки
```
Подробный учебник: см. [README в docs](docs/README.md).

## Быстрый старт
1. Загрузите `firmware/ESP32_LED_Controller.ino`.
2. В Serial (115200):
   ```
   SET WIFI_SSID MyWiFi
   SET WIFI_PASS MyPass
   SAVE
   ```
3. Откройте `http://ledcontroller.local/` или IP из лога.
4. (Опционально) Telegram:
   ```
   SET TOKEN 123456:ABC...
   CHAT ADD <мой_chat_id>   // иначе бот публичный
   ```


