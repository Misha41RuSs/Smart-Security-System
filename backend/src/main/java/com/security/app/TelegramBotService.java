package com.security.app;

import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;
import org.telegram.telegrambots.bots.TelegramLongPollingBot;
import org.telegram.telegrambots.meta.TelegramBotsApi;
import org.telegram.telegrambots.meta.api.methods.send.SendMessage;
import org.telegram.telegrambots.meta.api.objects.Update;
import org.telegram.telegrambots.meta.exceptions.TelegramApiException;

import org.telegram.telegrambots.meta.api.objects.replykeyboard.ReplyKeyboardMarkup;
import org.telegram.telegrambots.meta.api.objects.replykeyboard.buttons.KeyboardRow;
import java.util.ArrayList;
import java.util.List;

@Service
public class TelegramBotService extends TelegramLongPollingBot {

    private final AppState appState;
    private final String botUsername;
    private SerialService serialService;

    public TelegramBotService(AppState appState,
                              @Value("${telegram.bot.token}") String botToken,
                              @Value("${telegram.bot.username}") String botUsername) {
        super(botToken);
        this.appState = appState;
        this.botUsername = botUsername;
    }

    public void setSerialService(SerialService serialService) {
        this.serialService = serialService;
    }

    @Override
    public void onUpdateReceived(Update update) {
        if (update.hasMessage() && update.getMessage().hasText()) {
            String text = update.getMessage().getText();
            Long chatId = update.getMessage().getChatId();

            if (text.startsWith("/start")) {
                appState.addChatId(chatId);
                sendMessage(chatId, "Добро пожаловать в систему охраны! Вы подписаны на уведомления.\nИспользуйте кнопки меню для управления.");
            } else if (text.startsWith("/stop")) {
                appState.removeChatId(chatId);
                sendMessage(chatId, "Вы отписались от уведомлений.");
            } else if (text.startsWith("/arm") || text.equals("🚨 Включить охрану")) {
                if (serialService != null) {
                    serialService.sendCommand("ARM");
                    sendMessage(chatId, "Команда ARM отправлена на устройство.");
                }
            } else if (text.startsWith("/disarm") || text.equals("🔕 Выключить охрану")) {
                if (serialService != null) {
                    serialService.sendCommand("DISARM");
                    sendMessage(chatId, "Команда DISARM отправлена на устройство.");
                }
            } else if (text.startsWith("/police") || text.equals("👮‍♂️ Режим Сирены")) {
                if (serialService != null) {
                    serialService.sendCommand("POLICE");
                    sendMessage(chatId, "🚨 РЕЖИМ СИРЕНЫ АКТИВИРОВАН!");
                }
            } else if (text.startsWith("/nopolice") || text.equals("🛑 Выкл Сирену")) {
                if (serialService != null) {
                    serialService.sendCommand("NOPOLICE");
                    sendMessage(chatId, "Режим Сирены отключен.");
                }
            } else if (text.startsWith("/status") || text.equals("📊 Статус")) {
                sendMessage(chatId, "Текущие пороги:\nДистанция: " + appState.getDistanceThreshold() + " см\nСвет: " + appState.getLightThreshold());
            } else if (text.startsWith("/setdist ")) {
                try {
                    double dist = Double.parseDouble(text.replace("/setdist ", ""));
                    appState.setDistanceThreshold(dist);
                    if (serialService != null) {
                        serialService.sendCommand("DIST:" + dist);
                    }
                    sendMessage(chatId, "Порог дистанции установлен на " + dist + " см.");
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка: неверный формат числа.");
                }
            } else if (text.startsWith("/setlight ")) {
                try {
                    int light = Integer.parseInt(text.replace("/setlight ", ""));
                    appState.setLightThreshold(light);
                    if (serialService != null) {
                        serialService.sendCommand("LIGHT:" + light);
                    }
                    sendMessage(chatId, "Порог света установлен на " + light + ".");
                } catch (Exception e) {
                    sendMessage(chatId, "Ошибка: неверный формат числа.");
                }
            }
        }
    }

    public void sendAlertToAll(String messageText) {
        for (Long chatId : appState.getSubscribedChatIds()) {
            sendMessage(chatId, messageText);
        }
    }

    private void sendMessage(Long chatId, String text) {
        SendMessage message = new SendMessage();
        message.setChatId(String.valueOf(chatId));
        message.setText(text);

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

        keyboard.add(row1);
        keyboard.add(row2);
        keyboard.add(row3);
        keyboardMarkup.setKeyboard(keyboard);
        
        message.setReplyMarkup(keyboardMarkup);

        try {
            execute(message);
        } catch (TelegramApiException e) {
            e.printStackTrace();
        }
    }

    @Override
    public String getBotUsername() {
        return botUsername;
    }
}
