package com.security.app;

import com.security.app.entity.CommandLog;
import com.security.app.entity.SecurityEvent;
import com.security.app.entity.Subscriber;
import com.security.app.repository.CommandLogRepository;
import com.security.app.repository.SecurityEventRepository;
import com.security.app.repository.SubscriberRepository;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;
import org.telegram.telegrambots.bots.TelegramLongPollingBot;
import org.telegram.telegrambots.meta.api.methods.send.SendMessage;
import org.telegram.telegrambots.meta.api.objects.Update;
import org.telegram.telegrambots.meta.api.objects.User;
import org.telegram.telegrambots.meta.api.objects.replykeyboard.ReplyKeyboardMarkup;
import org.telegram.telegrambots.meta.api.objects.replykeyboard.buttons.KeyboardRow;
import org.telegram.telegrambots.meta.exceptions.TelegramApiException;

import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

@Service
/**
 * Telegram Bot service for handling user commands and sending alerts.
 */
public class TelegramBotService extends TelegramLongPollingBot {

    private final AppState appState;
    private final String botUsername;
    private SerialService serialService;

    private final SubscriberRepository subscriberRepository;
    private final CommandLogRepository commandLogRepository;
    private final SecurityEventRepository securityEventRepository;

    // Сеты для отслеживания пользователей, находящихся в процессе изменения настроек
    private final Set<Long> awaitingPasswordChatIds = ConcurrentHashMap.newKeySet();
    private final Set<Long> awaitingDistanceChatIds = ConcurrentHashMap.newKeySet();
    private final Set<Long> awaitingLightChatIds = ConcurrentHashMap.newKeySet();

    public TelegramBotService(AppState appState,
                              @Value("${telegram.bot.token}") String botToken,
                              @Value("${telegram.bot.username}") String botUsername,
                              SubscriberRepository subscriberRepository,
                              CommandLogRepository commandLogRepository,
                              SecurityEventRepository securityEventRepository) {
        super(botToken);
        this.appState = appState;
        this.botUsername = botUsername;
        this.subscriberRepository = subscriberRepository;
        this.commandLogRepository = commandLogRepository;
        this.securityEventRepository = securityEventRepository;
    }

    public void setSerialService(SerialService serialService) {
        this.serialService = serialService;
    }

    @Override
    public void onUpdateReceived(Update update) {
        if (update.hasMessage() && update.getMessage().hasText()) {
            String text = update.getMessage().getText();
            Long chatId = update.getMessage().getChatId();

            // Если пришел клик по кнопкам меню, сбрасываем состояние ожидания ввода
            if (text.equals("🚨 Включить охрану") || text.equals("🔕 Выключить охрану") ||
                text.equals("👮‍♂️ Режим Сирены") || text.equals("🛑 Выкл Сирену") ||
                text.equals("📊 Статус") || text.equals("📜 История событий") ||
                text.equals("📈 Статистика") || text.equals("👥 Пользователи") ||
                text.equals("⚙️ Уст. дистанцию") || text.equals("⚙️ Уст. порог света") ||
                text.equals("🔑 Вход в админку") || text.equals("🔑 Выйти из админки")) {
                awaitingPasswordChatIds.remove(chatId);
                awaitingDistanceChatIds.remove(chatId);
                awaitingLightChatIds.remove(chatId);
            }

            // Обработка ввода пароля админки
            if (awaitingPasswordChatIds.contains(chatId)) {
                awaitingPasswordChatIds.remove(chatId);
                boolean success = false;
                if ("12345".equals(text.trim())) {
                    try {
                        Subscriber sub = subscriberRepository.findById(chatId).orElse(
                            Subscriber.builder()
                                .chatId(chatId)
                                .subscribedAt(LocalDateTime.now())
                                .active(true)
                                .build()
                        );
                        sub.setAdmin(true);
                        subscriberRepository.save(sub);
                        
                        sendMessage(chatId, "🔑 Авторизация успешна! Вы вошли в режим Администратора.\nТеперь вам доступны кнопки изменения порогов датчиков.");
                        success = true;
                    } catch (Exception e) {
                        sendMessage(chatId, "Ошибка при сохранении статуса администратора: " + e.getMessage());
                    }
                } else {
                    sendMessage(chatId, "🔴 Неверный пароль! Доступ в админку запрещен.");
                }
                logCommand(update, "[Ввод пароля админки]", success);
                return;
            }

            // Обработка ввода новой дистанции
            if (awaitingDistanceChatIds.contains(chatId)) {
                awaitingDistanceChatIds.remove(chatId);
                boolean success = false;
                try {
                    double dist = Double.parseDouble(text.trim());
                    appState.setDistanceThreshold(dist);
                    if (serialService != null) {
                        serialService.sendCommand("DIST:" + dist);
                    }
                    sendMessage(chatId, "⚙️ Порог дистанции успешно установлен на " + dist + " см.");
                    success = true;
                } catch (Exception e) {
                    sendMessage(chatId, "🔴 Ошибка: Неверный формат числа! Введите число (например, 7.5).");
                }
                logCommand(update, "[Установка дистанции: " + text + "]", success);
                return;
            }

            // Обработка ввода нового порога света
            if (awaitingLightChatIds.contains(chatId)) {
                awaitingLightChatIds.remove(chatId);
                boolean success = false;
                try {
                    int light = Integer.parseInt(text.trim());
                    appState.setLightThreshold(light);
                    if (serialService != null) {
                        serialService.sendCommand("LIGHT:" + light);
                    }
                    sendMessage(chatId, "⚙️ Порог освещенности успешно установлен на " + light + ".");
                    success = true;
                } catch (Exception e) {
                    sendMessage(chatId, "🔴 Ошибка: Неверный формат целого числа! Введите число (например, 900).");
                }
                logCommand(update, "[Установка света: " + text + "]", success);
                return;
            }

            // Загружаем статус пользователя из БД
            Optional<Subscriber> subOpt = subscriberRepository.findById(chatId);
            boolean isAdmin = subOpt.map(Subscriber::isAdmin).orElse(false);

            if (text.startsWith("/start")) {
                User from = update.getMessage().getFrom();
                String username = from.getUserName();
                String firstName = from.getFirstName();
                String lastName = from.getLastName();
                
                try {
                    Subscriber sub = subscriberRepository.findById(chatId).orElse(
                        Subscriber.builder()
                            .chatId(chatId)
                            .subscribedAt(LocalDateTime.now())
                            .build()
                    );
                    sub.setUsername(username);
                    sub.setFirstName(firstName);
                    sub.setLastName(lastName);
                    sub.setActive(true);
                    subscriberRepository.save(sub);
                } catch (Exception e) {
                    System.err.println("Ошибка сохранения подписчика: " + e.getMessage());
                }
                
                appState.addChatId(chatId);
                sendMessage(chatId, "Добро пожаловать в систему охраны! Вы подписаны на уведомления.\nИспользуйте кнопки меню для управления.");
                logCommand(update, "/start", true);

            } else if (text.startsWith("/stop")) {
                try {
                    subscriberRepository.findById(chatId).ifPresent(sub -> {
                        sub.setActive(false);
                        sub.setAdmin(false); // Сбрасываем админа при отписке
                        subscriberRepository.save(sub);
                    });
                } catch (Exception e) {
                    System.err.println("Ошибка деактивации подписчика: " + e.getMessage());
                }
                
                appState.removeChatId(chatId);
                sendMessage(chatId, "Вы отписались от уведомлений.");
                logCommand(update, "/stop", true);

            } else if (text.startsWith("/arm") || text.equals("🚨 Включить охрану")) {
                boolean success = false;
                if (serialService != null) {
                    serialService.sendCommand("ARM");
                    sendMessage(chatId, "Команда ARM отправлена на устройство.");
                    success = true;
                } else {
                    sendMessage(chatId, "Ошибка: Устройство не подключено.");
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/disarm") || text.equals("🔕 Выключить охрану")) {
                boolean success = false;
                if (serialService != null) {
                    serialService.sendCommand("DISARM");
                    sendMessage(chatId, "Команда DISARM отправлена на устройство.");
                    success = true;
                } else {
                    sendMessage(chatId, "Ошибка: Устройство не подключено.");
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/police") || text.equals("👮‍♂️ Режим Сирены")) {
                boolean success = false;
                if (serialService != null) {
                    serialService.sendCommand("POLICE");
                    sendMessage(chatId, "🚨 РЕЖИМ СИРЕНЫ АКТИВИРОВАН!");
                    success = true;
                } else {
                    sendMessage(chatId, "Ошибка: Устройство не подключено.");
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/nopolice") || text.equals("🛑 Выкл Сирену")) {
                boolean success = false;
                if (serialService != null) {
                    serialService.sendCommand("NOPOLICE");
                    sendMessage(chatId, "Режим Сирены отключен.");
                    success = true;
                } else {
                    sendMessage(chatId, "Ошибка: Устройство не подключено.");
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/status") || text.equals("📊 Статус")) {
                sendMessage(chatId, "Текущие пороги:\nДистанция: " + appState.getDistanceThreshold() + " см\nСвет: " + appState.getLightThreshold());
                logCommand(update, text, true);

            } else if (text.startsWith("/setdist ")) {
                if (!isAdmin) {
                    sendMessage(chatId, "🚫 Доступ запрещен! Эта команда доступна только администраторам. Авторизуйтесь по кнопке '🔑 Вход в админку'.");
                    logCommand(update, text, false);
                    return;
                }
                boolean success = false;
                try {
                    double dist = Double.parseDouble(text.replace("/setdist ", ""));
                    appState.setDistanceThreshold(dist);
                    if (serialService != null) {
                        serialService.sendCommand("DIST:" + dist);
                    }
                    sendMessage(chatId, "Порог дистанции установлен на " + dist + " см.");
                    success = true;
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка: неверный формат числа.");
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/setlight ")) {
                if (!isAdmin) {
                    sendMessage(chatId, "🚫 Доступ запрещен! Эта команда доступна только администраторам. Авторизуйтесь по кнопке '🔑 Вход в админку'.");
                    logCommand(update, text, false);
                    return;
                }
                boolean success = false;
                try {
                    int light = Integer.parseInt(text.replace("/setlight ", ""));
                    appState.setLightThreshold(light);
                    if (serialService != null) {
                        serialService.sendCommand("LIGHT:" + light);
                    }
                    sendMessage(chatId, "Порог света установлен на " + light + ".");
                    success = true;
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка: неверный формат числа.");
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/history") || text.equals("📜 История событий")) {
                boolean success = false;
                try {
                    List<SecurityEvent> events = securityEventRepository.findTop5ByOrderByTimestampDesc();
                    if (events.isEmpty()) {
                        sendMessage(chatId, "📭 Событий тревоги пока не зафиксировано.");
                    } else {
                        StringBuilder sb = new StringBuilder("📋 *Последние 5 тревожных событий:*\n\n");
                        DateTimeFormatter formatter = DateTimeFormatter.ofPattern("dd.MM.yyyy HH:mm:ss");
                        for (SecurityEvent event : events) {
                            sb.append("⏳ *").append(event.getTimestamp().format(formatter)).append("*\n")
                              .append("🔹 Тип: ").append(event.getEventType()).append("\n")
                              .append("📊 Значение: ").append(event.getValue()).append("\n")
                              .append("📝 ").append(event.getMessage()).append("\n\n");
                        }
                        sendMessageMarkdown(chatId, sb.toString());
                    }
                    success = true;
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка при получении истории: " + e.getMessage());
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/stats") || text.equals("📈 Статистика")) {
                boolean success = false;
                try {
                    long totalAlerts = securityEventRepository.count();
                    LocalDateTime startOfDay = LocalDateTime.now().withHour(0).withMinute(0).withSecond(0).withNano(0);
                    long alertsToday = securityEventRepository.countByTimestampAfter(startOfDay);
                    long totalUsers = subscriberRepository.count();
                    long activeUsers = subscriberRepository.findByActiveTrue().size();

                    String statsMessage = "📈 *Статистика системы охраны:*\n\n" +
                            "🚨 Зафиксировано тревог (всего): *" + totalAlerts + "*\n" +
                            "📅 Тревог за сегодня: *" + alertsToday + "*\n" +
                            "👥 Всего пользователей: *" + totalUsers + "*\n" +
                            "🟢 Активных подписчиков: *" + activeUsers + "*\n" +
                            "⚙️ Порог дистанции: *" + appState.getDistanceThreshold() + " см*\n" +
                            "💡 Порог света: *" + appState.getLightThreshold() + "*";
                    
                    sendMessageMarkdown(chatId, statsMessage);
                    success = true;
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка при формировании статистики: " + e.getMessage());
                }
                logCommand(update, text, success);

            } else if (text.startsWith("/users") || text.equals("👥 Пользователи")) {
                if (!isAdmin) {
                    sendMessage(chatId, "🚫 Доступ запрещен! Эта команда доступна только администраторам. Авторизуйтесь по кнопке '🔑 Вход в админку'.");
                    logCommand(update, text, false);
                    return;
                }
                boolean success = false;
                try {
                    List<Subscriber> subs = subscriberRepository.findAll();
                    if (subs.isEmpty()) {
                        sendMessage(chatId, "👥 В базе данных пока нет пользователей.");
                    } else {
                        StringBuilder sb = new StringBuilder("👥 *Зарегистрированные пользователи:*\n\n");
                        for (Subscriber sub : subs) {
                            sb.append(sub.isActive() ? "🟢 " : "🔴 ")
                              .append(sub.isAdmin() ? "👑 [Админ] " : "👤 [Юзер] ")
                              .append(sub.getFirstName() != null ? sub.getFirstName() : "").append(" ")
                              .append(sub.getLastName() != null ? sub.getLastName() : "")
                              .append(sub.getUsername() != null ? " (@" + sub.getUsername() + ")" : "")
                              .append("\nID: `").append(sub.getChatId()).append("`\n\n");
                        }
                        sendMessageMarkdown(chatId, sb.toString());
                    }
                    success = true;
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка при получении списка пользователей: " + e.getMessage());
                }
                logCommand(update, text, success);

            } else if (text.equals("🔑 Вход в админку")) {
                awaitingPasswordChatIds.add(chatId);
                sendMessage(chatId, "🔑 Введите пароль администратора для авторизации:");
                logCommand(update, text, true);

            } else if (text.equals("🔑 Выйти из админки")) {
                try {
                    subscriberRepository.findById(chatId).ifPresent(sub -> {
                        sub.setAdmin(false);
                        subscriberRepository.save(sub);
                    });
                    sendMessage(chatId, "👤 Вы вышли из режима Администратора. Доступ к настройкам закрыт.");
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка при выходе из админки: " + e.getMessage());
                }
                logCommand(update, text, true);

            } else if (text.equals("⚙️ Уст. дистанцию")) {
                if (!isAdmin) {
                    sendMessage(chatId, "🚫 Доступ запрещен! Эта функция доступна только администраторам.");
                    logCommand(update, text, false);
                    return;
                }
                awaitingDistanceChatIds.add(chatId);
                sendMessage(chatId, "⚙️ Введите новый порог дистанции в сантиметрах (например, 6.5):");
                logCommand(update, text, true);

            } else if (text.equals("⚙️ Уст. порог света")) {
                if (!isAdmin) {
                    sendMessage(chatId, "🚫 Доступ запрещен! Эта функция доступна только администраторам.");
                    logCommand(update, text, false);
                    return;
                }
                awaitingLightChatIds.add(chatId);
                sendMessage(chatId, "⚙️ Введите новый порог освещенности (целое число, например, 950):");
                logCommand(update, text, true);
            }
        }
    }

    public void sendAlertToAll(String messageText) {
        for (Long chatId : appState.getSubscribedChatIds()) {
            sendMessage(chatId, messageText);
        }
    }

    private void logCommand(Update update, String command, boolean success) {
        try {
            Long chatId = update.getMessage().getChatId();
            String username = update.getMessage().getFrom().getUserName();
            CommandLog log = CommandLog.builder()
                .timestamp(LocalDateTime.now())
                .chatId(chatId)
                .username(username)
                .command(command)
                .success(success)
                .build();
            commandLogRepository.save(log);
        } catch (Exception e) {
            System.err.println("Ошибка сохранения лога команды: " + e.getMessage());
        }
    }

    private boolean isUserAdmin(Long chatId) {
        try {
            return subscriberRepository.findById(chatId).map(Subscriber::isAdmin).orElse(false);
        } catch (Exception e) {
            return false;
        }
    }

    private void sendMessage(Long chatId, String text) {
        SendMessage message = new SendMessage();
        message.setChatId(String.valueOf(chatId));
        message.setText(text);
        message.setReplyMarkup(createKeyboard(isUserAdmin(chatId)));

        try {
            execute(message);
        } catch (TelegramApiException e) {
            e.printStackTrace();
        }
    }

    private void sendMessageMarkdown(Long chatId, String text) {
        SendMessage message = new SendMessage();
        message.setChatId(String.valueOf(chatId));
        message.setText(text);
        message.setParseMode("Markdown");
        message.setReplyMarkup(createKeyboard(isUserAdmin(chatId)));

        try {
            execute(message);
        } catch (TelegramApiException e) {
            e.printStackTrace();
        }
    }

    private ReplyKeyboardMarkup createKeyboard(boolean isAdmin) {
        ReplyKeyboardMarkup keyboardMarkup = new ReplyKeyboardMarkup();
        keyboardMarkup.setResizeKeyboard(true);
        List<KeyboardRow> keyboard = new ArrayList<>();
        
        KeyboardRow row1 = new KeyboardRow();
        row1.add("🚨 Включить охрану");
        row1.add("🔕 Выключить охрану");
        
        KeyboardRow row2 = new KeyboardRow();
        row2.add("👮‍♂️ Режим Сирены");
        row2.add("🛑 Выкл Сирену");
        
        KeyboardRow row3 = new KeyboardRow();
        row3.add("📊 Статус");
        row3.add("📜 История событий");
        
        KeyboardRow row4 = new KeyboardRow();
        row4.add("📈 Статистика");
        if (isAdmin) {
            row4.add("👥 Пользователи");
            
            KeyboardRow row5 = new KeyboardRow();
            row5.add("⚙️ Уст. дистанцию");
            row5.add("⚙️ Уст. порог света");
            
            KeyboardRow row6 = new KeyboardRow();
            row6.add("🔑 Выйти из админки");
            
            keyboard.add(row1);
            keyboard.add(row2);
            keyboard.add(row3);
            keyboard.add(row4);
            keyboard.add(row5);
            keyboard.add(row6);
        } else {
            row4.add("🔑 Вход в админку");
            
            keyboard.add(row1);
            keyboard.add(row2);
            keyboard.add(row3);
            keyboard.add(row4);
        }

        keyboardMarkup.setKeyboard(keyboard);
        return keyboardMarkup;
    }

    @Override
    public String getBotUsername() {
        return botUsername;
    }
}
