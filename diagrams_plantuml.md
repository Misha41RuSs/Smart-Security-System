# Программные коды диаграмм для Draw.io / PlantUML

Ниже приведены исходные коды диаграмм в формате **PlantUML** и **Mermaid**.  
Вы можете легко импортировать их в **Draw.io** (меню *Устроить -> Вставить -> Advanced -> PlantUML / Mermaid*) или воспользоваться онлайн-редактором [PlantText](https://www.planttext.com/) для мгновенной генерации картинок и последующей вставки в Draw.io!

---

## 1. Диаграмма прецедентов (Use Case Diagram)
Описывает сценарии использования системы пользователем и администратором. Отражает разделение ролей: доступ к настройкам порогов датчиков и списку пользователей открывается только после успешного ввода пароля администратора.

### Код PlantUML:
```plantuml
@startuml
left to right direction
skinparam packageStyle rectangle

actor "Пользователь (Telegram)" as User
actor "Администратор (Telegram)" as Admin
actor "Аппаратная кнопка BOOT" as PhysicalButton

' Наследование ролей: Администратор может выполнять все действия Пользователя
Admin -up-|> User

rectangle "Умная Охранная Система" {
  ' Базовые прецеденты пользователя
  usecase "Запуск бота (/start)" as UC_Start
  usecase "Постановка на охрану (ARM)" as UC_Arm
  usecase "Снятие с охраны (DISARM)" as UC_Disarm
  usecase "Просмотр статуса порогов" as UC_Status
  usecase "Просмотр истории тревог" as UC_History
  usecase "Просмотр статистики" as UC_Stats
  usecase "Получение уведомления о тревоге" as UC_Notification
  usecase "Авторизация в админке (пароль 12345)" as UC_Login
  usecase "Выход из админки" as UC_Logout

  ' Прецеденты администратора
  usecase "Настройка порога дистанции" as UC_SetDist
  usecase "Настройка порога света" as UC_SetLight
  usecase "Просмотр списка пользователей" as UC_Users
  usecase "Активация режима сирены" as UC_Police
  usecase "Отключение сирены" as UC_NoPolice
}

User --> UC_Start
User --> UC_Arm
User --> UC_Disarm
User --> UC_Status
User --> UC_History
User --> UC_Stats
User --> UC_Notification
User --> UC_Login
User --> UC_Logout

Admin --> UC_SetDist
Admin --> UC_SetLight
Admin --> UC_Users
Admin --> UC_Police
Admin --> UC_NoPolice

PhysicalButton --> UC_Arm
PhysicalButton --> UC_Disarm
@enduml
```

---

## 2. Диаграмма последовательности: Авторизация Администратора
Показывает процесс ввода пароля `12345` и динамическое обновление интерфейса.

### Код PlantUML:
```plantuml
@startuml
actor "Пользователь (Telegram)" as User
boundary "Telegram Bot API" as TG
participant "TelegramBotService" as Bot
database "PostgreSQL" as DB

User -> TG : Нажатие кнопки "🔑 Вход в админку"
TG -> Bot : onUpdateReceived(Update)
activate Bot

Bot -> Bot : Добавление chatId в awaitingPasswordChatIds
Bot -> TG : sendMessage("🔑 Введите пароль:")
deactivate Bot
TG -> User : Запрос ввода пароля

User -> TG : Ввод текстового сообщения "12345"
TG -> Bot : onUpdateReceived(Update)
activate Bot

alt Пароль верен ("12345")
    Bot -> DB : findById(chatId)
    activate DB
    DB --> Bot : Subscriber
    Bot -> DB : save(sub с is_admin=true)
    DB --> Bot : OK
    deactivate DB
    Bot -> Bot : logCommand("[Ввод пароля админки]", success=true)
    Bot -> TG : sendMessage("🔑 Авторизация успешна! Вы вошли в режим Администратора.", keyboard=AdminLayout)
else Неверный пароль
    Bot -> Bot : logCommand("[Ввод пароля админки]", success=false)
    Bot -> TG : sendMessage("🔴 Неверный пароль! Доступ запрещен.", keyboard=UserLayout)
end
deactivate Bot
TG -> User : Результат авторизации с обновленной клавиатурой
@enduml
```

---

## 3. Диаграмма последовательности: Изменение настроек администратором (через кастомные кнопки)
Показывает пошаговый процесс изменения порога дистанции через специальное меню админки.

### Код PlantUML:
```plantuml
@startuml
actor "Администратор (Telegram)" as Admin
boundary "Telegram Bot API" as TG
participant "TelegramBotService" as Bot
participant "AppState" as State
database "PostgreSQL" as DB
participant "SerialService" as Serial
boundary "UART/COM-порт" as COM
entity "ESP32 Board" as ESP

Admin -> TG : Нажатие кнопки "⚙️ Уст. дистанцию"
TG -> Bot : onUpdateReceived(Update)
activate Bot
Bot -> Bot : Добавление chatId в awaitingDistanceChatIds
Bot -> TG : sendMessage("⚙️ Введите новый порог дистанции...")
deactivate Bot
TG -> Admin : Запрос ввода числового значения

Admin -> TG : Отправка сообщения "6.5"
TG -> Bot : onUpdateReceived(Update)
activate Bot

alt Успешный парсинг (число "6.5")
    Bot -> Bot : Извлечение из awaitingDistanceChatIds
    Bot -> State : setDistanceThreshold(6.5)
    activate State
    State -> DB : saveSetting("distance_threshold", "6.5")
    State --> Bot : OK
    deactivate State
    
    Bot -> Serial : sendCommand("DIST:6.5")
    activate Serial
    Serial -> COM : Отправка "DIST:6.5\n"
    COM -> ESP : Прием нового лимита на устройстве
    deactivate Serial
    
    Bot -> Bot : logCommand("[Установка дистанции: 6.5]", success=true)
    Bot -> TG : sendMessage("⚙️ Порог дистанции успешно установлен на 6.5 см.")
else Ошибка парсинга (например, текст "abc")
    Bot -> Bot : Извлечение из awaitingDistanceChatIds
    Bot -> Bot : logCommand("[Установка дистанции: abc]", success=false)
    Bot -> TG : sendMessage("🔴 Ошибка: Неверный формат числа!")
end

deactivate Bot
TG -> Admin : Ответ бота
@enduml
```

---

## 4. Диаграмма последовательности: Постановка на охрану (Arming Sequence)
Показывает процесс отправки команды ARM из Telegram на физическое устройство.

### Код PlantUML:
```plantuml
@startuml
actor "Пользователь (Telegram)" as User
boundary "Telegram Bot API" as TG
participant "TelegramBotService" as Bot
participant "AppState" as State
participant "SerialService" as Serial
boundary "UART/COM-порт" as COM
entity "ESP32 Board" as ESP

User -> TG : Нажатие кнопки "🚨 Включить охрану"
TG -> Bot : onUpdateReceived(Update)
activate Bot

Bot -> Serial : sendCommand("ARM")
activate Serial
Serial -> COM : Отправка строки "ARM\n"
activate COM
COM -> ESP : UART Сигнал "ARM"
deactivate COM
activate ESP
ESP -> ESP : Установка is_armed = true
ESP -> ESP : Включение синего диода LED_BUILTIN
deactivate ESP

Bot -> Bot : logCommand("/arm", success=true)
Bot -> TG : sendMessage("Команда ARM отправлена...")
deactivate Serial
deactivate Bot
TG -> User : Сообщение в чате
@enduml
```

---

## 5. Диаграмма последовательности: Обработка сработки датчика (Alarm Sequence)
Показывает, как датчик на ESP32 вызывает тревогу, которая записывается в БД и прилетает в Telegram.

### Код PlantUML:
```plantuml
@startuml
entity "Физический объект" as Object
participant "ESP32 (sensor_task)" as ESP
boundary "UART/COM-порт" as COM
participant "SerialService" as Serial
database "PostgreSQL" as DB
participant "TelegramBotService" as Bot
boundary "Telegram Bot API" as TG
actor "Все Подписчики" as Users

Object -> ESP : Превышение порога (Дистанция < Threshold)
activate ESP
ESP -> COM : Отправка "[ОХРАНА] Дист: 4.2 см..."
deactivate ESP
activate COM
COM -> Serial : readLoop (Считывание строки)
deactivate COM
activate Serial

Serial -> Serial : processLine(line) (Парсинг данных)
Serial -> DB : saveSecurityEvent(DISTANCE, 4.2, ...)
activate DB
DB --> Serial : Подтверждение записи
deactivate DB

Serial -> Bot : sendAlertToAll("⚠️ ТРЕВОГА!...")
activate Bot
deactivate Serial

Bot -> TG : execute(SendMessage)
activate TG
TG -> Users : Push-уведомление в Telegram
deactivate TG
deactivate Bot
@enduml
```

---

## 6. Диаграмма последовательности: Запрос истории тревог (History Request)
Показывает, как бот вытягивает данные о тревогах из БД по нажатию кнопки.

### Код PlantUML:
```plantuml
@startuml
actor "Пользователь" as User
boundary "Telegram Bot API" as TG
participant "TelegramBotService" as Bot
database "PostgreSQL" as DB

User -> TG : Нажатие "📜 История событий"
TG -> Bot : onUpdateReceived()
activate Bot

Bot -> Bot : logCommand("/history", success=true)
Bot -> DB : findTop5ByOrderByTimestampDesc()
activate DB
DB --> Bot : Список последних 5 SecurityEvent
deactivate DB

Bot -> Bot : Форматирование Markdown-ответа
Bot -> TG : sendMessage(formattedText, markdown=true)
activate TG
TG -> User : Сообщение с таблицей последних тревог
deactivate TG
deactivate Bot
@enduml
```

---

## 7. Диаграмма сущностей БД (ER-Diagram в формате Mermaid)
Для отображения структуры связей базы данных. Отражает новое поле `is_admin`.

### Код Mermaid:
```mermaid
erDiagram
    SUBSCRIBER {
        bigint chat_id PK
        string username
        string first_name
        string last_name
        timestamp subscribed_at
        boolean active
        boolean is_admin
    }
    SECURITY_EVENT {
        bigint id PK
        timestamp timestamp
        string event_type
        double value
        string message
    }
    COMMAND_LOG {
        bigint id PK
        timestamp timestamp
        bigint chat_id
        string username
        string command
        boolean success
    }
    SYSTEM_SETTING {
        string key PK
        string value
    }
```

---

## 8. Диаграмма классов Java (Java Class Diagram)
Описывает структуру классов серверного приложения на базе Spring Boot, их атрибуты, методы, связи и зависимости.

### Код PlantUML:
```plantuml
@startuml
skinparam ClassAttributeIconSize 0

package "com.security.app" {
  class SecurityApp {
    + {static} void main(String[] args)
  }

  class AppState {
    - Double distanceThreshold
    - Integer lightThreshold
    - Set<Long> subscribedChatIds
    - SubscriberRepository subscriberRepository
    - SystemSettingRepository systemSettingRepository
    + void init()
    + Double getDistanceThreshold()
    + void setDistanceThreshold(Double value)
    + Integer getLightThreshold()
    + void setLightThreshold(Integer value)
    + Set<Long> getSubscribedChatIds()
    + void addSubscribedChatId(Long chatId)
    + void removeSubscribedChatId(Long chatId)
  }
}

package "com.security.app.entity" {
  class Subscriber {
    - Long chatId
    - String username
    - String firstName
    - String lastName
    - LocalDateTime subscribedAt
    - boolean active
    - boolean isAdmin
    + Subscriber()
    + Subscriber(Long chatId, String username, String firstName, String lastName, LocalDateTime subscribedAt, boolean active, boolean isAdmin)
    + Long getChatId()
    + void setChatId(Long id)
    + String getUsername()
    + void setUsername(String username)
    + String getFirstName()
    + void setFirstName(String firstName)
    + String getLastName()
    + void setLastName(String lastName)
    + LocalDateTime getSubscribedAt()
    + void setSubscribedAt(LocalDateTime subscribedAt)
    + boolean isActive()
    + void setActive(boolean active)
    + boolean isAdmin()
    + void setAdmin(boolean admin)
    + {static} Builder builder()
  }

  class SecurityEvent {
    - Long id
    - LocalDateTime timestamp
    - String eventType
    - Double value
    - String message
    + SecurityEvent()
    + SecurityEvent(Long id, LocalDateTime timestamp, String eventType, Double value, String message)
    + Long getId()
    + void setId(Long id)
    + LocalDateTime getTimestamp()
    + void setTimestamp(LocalDateTime timestamp)
    + String getEventType()
    + void setEventType(String eventType)
    + Double getValue()
    + void setValue(Double value)
    + String getMessage()
    + void setMessage(String message)
    + {static} Builder builder()
  }

  class CommandLog {
    - Long id
    - LocalDateTime timestamp
    - Long chatId
    - String username
    - String command
    - boolean success
    + CommandLog()
    + CommandLog(Long id, LocalDateTime timestamp, Long chatId, String username, String command, boolean success)
    + Long getId()
    + void setId(Long id)
    + LocalDateTime getTimestamp()
    + void setTimestamp(LocalDateTime timestamp)
    + Long getChatId()
    + void setChatId(Long chatId)
    + String getUsername()
    + void setUsername(String username)
    + String getCommand()
    + void setCommand(String command)
    + boolean isSuccess()
    + void setSuccess(boolean success)
    + {static} Builder builder()
  }

  class SystemSetting {
    - String key
    - String value
    + SystemSetting()
    + SystemSetting(String key, String value)
    + String getKey()
    + void setKey(String key)
    + String getValue()
    + void setValue(String value)
    + {static} Builder builder()
  }
}

package "com.security.app.repository" {
  interface SubscriberRepository <<interface>> {
    + List<Subscriber> findByActiveTrue()
  }
  interface SecurityEventRepository <<interface>> {
    + List<SecurityEvent> findTop5ByOrderByTimestampDesc()
  }
  interface CommandLogRepository <<interface>>
  interface SystemSettingRepository <<interface>>
}

package "com.security.app.service" {
  class SerialService {
    - String portName
    - SerialPort activePort
    - Thread readThread
    - AppState appState
    - SecurityEventRepository eventRepository
    - TelegramBotService botService
    + void init()
    + void sendCommand(String command)
    - void readLoop()
    - void processLine(String line)
  }

  class TelegramBotService {
    - String botUsername
    - String botToken
    - AppState appState
    - SubscriberRepository subscriberRepository
    - SecurityEventRepository eventRepository
    - CommandLogRepository commandLogRepository
    - Set<Long> awaitingPasswordChatIds
    - Set<Long> awaitingDistanceChatIds
    - Set<Long> awaitingLightChatIds
    + void onUpdateReceived(Update update)
    + void sendAlertToAll(String message)
    - void handleUserMessage(Long chatId, String username, String text)
    - void handleAdminMessage(Long chatId, String username, String text)
    - void logCommand(Long chatId, String username, String command, boolean success)
  }
}

' Relationships and dependencies
SubscriberRepository --|> "JpaRepository"
SecurityEventRepository --|> "JpaRepository"
CommandLogRepository --|> "JpaRepository"
SystemSettingRepository --|> "JpaRepository"

AppState --> SubscriberRepository : uses
AppState --> SystemSettingRepository : uses

SerialService --> AppState : reads state
SerialService --> SecurityEventRepository : saves events
SerialService --> TelegramBotService : triggers alerts

TelegramBotService --> AppState : reads state
TelegramBotService --> SubscriberRepository : manages
TelegramBotService --> SecurityEventRepository : queries history
TelegramBotService --> CommandLogRepository : audits logs

TelegramBotService --|> "TelegramLongPollingBot"

@enduml
```
