package com.security.app;

import com.fazecast.jSerialComm.SerialPort;
import com.security.app.entity.SecurityEvent;
import com.security.app.repository.SecurityEventRepository;
import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.time.LocalDateTime;
import java.util.Scanner;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

@Service
/**
 * Service for serial port communication with the hardware sensors.
 */
public class SerialService {

    private final AppState appState;
    private final TelegramBotService telegramBotService;
    private final SecurityEventRepository securityEventRepository;

    @Value("${serial.port:COM4}")
    private String portName;

    @Value("${serial.baudRate:115200}")
    private int baudRate;

    private SerialPort serialPort;
    private Thread readThread;
    private volatile boolean running = true;

    // Регулярное выражение для парсинга вывода ESP32
    // Пример: [ОХРАНА] Дист: 4.5 см | Свет: 1200 | Движ: 1
    private final Pattern regexPattern = Pattern.compile("\\[(.*?)\\] Дист:\\s*([0-9.]+)\\s*см \\| Свет:\\s*(\\d+) \\| Движ:\\s*(\\d+)");

    private long lastDistanceAlertTime = 0;
    private long lastLightAlertTime = 0;
    private static final long ALERT_COOLDOWN_MS = 10000; // 10 секунд задержки между алертами

    public SerialService(AppState appState, TelegramBotService telegramBotService, SecurityEventRepository securityEventRepository) {
        this.appState = appState;
        this.telegramBotService = telegramBotService;
        this.securityEventRepository = securityEventRepository;
        telegramBotService.setSerialService(this);
    }

    @PostConstruct
    public void init() {
        try {
            serialPort = SerialPort.getCommPort(portName);
            serialPort.setBaudRate(baudRate);
            serialPort.setComPortTimeouts(SerialPort.TIMEOUT_READ_SEMI_BLOCKING, 1000, 0);

            if (serialPort.openPort()) {
                System.out.println("COM порт " + portName + " успешно открыт.");
                readThread = new Thread(this::readLoop);
                readThread.start();
            } else {
                System.err.println("Не удалось открыть COM порт " + portName + ". Убедитесь, что устройство подключено.");
            }
        } catch (Exception e) {
            System.err.println("Ошибка при инициализации COM порта " + portName + ": " + e.getMessage());
            System.err.println("====== БЭКЕНД ЗАПУЩЕН В РЕЖИМЕ БЕЗ ПОДКЛЮЧЕНИЯ К ДАТЧИКАМ (эмуляция/Telegram-only) ======");
            serialPort = null;
        }
    }

    private void readLoop() {
        try (InputStream in = serialPort.getInputStream(); Scanner scanner = new Scanner(in, StandardCharsets.UTF_8.name())) {
            while (running && scanner.hasNextLine()) {
                String line = scanner.nextLine();
                System.out.println("SERIAL: " + line);
                processLine(line);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    private void processLine(String line) {
        Matcher matcher = regexPattern.matcher(line);
        if (matcher.find()) {
            String stateStr = matcher.group(1).trim();
            double distance = Double.parseDouble(matcher.group(2));
            int light = Integer.parseInt(matcher.group(3));
            
            // Если система на охране, проверяем пороги
            if ("ОХРАНА".equals(stateStr)) {
                long now = System.currentTimeMillis();
                
                // Проверка дистанции
                if (distance > 0.1 && distance < appState.getDistanceThreshold()) {
                    if (now - lastDistanceAlertTime > ALERT_COOLDOWN_MS) {
                        String alertMsg = "⚠️ ТРЕВОГА! Нарушение периметра! Дистанция: " + distance + " см";
                        telegramBotService.sendAlertToAll(alertMsg);
                        lastDistanceAlertTime = now;
                        saveSecurityEvent("DISTANCE", distance, alertMsg);
                    }
                }
                
                // Проверка света
                if (light > appState.getLightThreshold()) {
                    if (now - lastLightAlertTime > ALERT_COOLDOWN_MS) {
                        String alertMsg = "💡 ТРЕВОГА! Вспышка света! Уровень: " + light;
                        telegramBotService.sendAlertToAll(alertMsg);
                        lastLightAlertTime = now;
                        saveSecurityEvent("LIGHT", (double) light, alertMsg);
                    }
                }
            }
        }
    }

    private void saveSecurityEvent(String eventType, double value, String msg) {
        try {
            SecurityEvent event = SecurityEvent.builder()
                .timestamp(LocalDateTime.now())
                .eventType(eventType)
                .value(value)
                .message(msg)
                .build();
            securityEventRepository.save(event);
        } catch (Exception e) {
            System.err.println("Ошибка при сохранении события безопасности в БД: " + e.getMessage());
        }
    }

    public void sendCommand(String command) {
        if (serialPort != null && serialPort.isOpen()) {
            try {
                OutputStream out = serialPort.getOutputStream();
                out.write((command + "\n").getBytes(StandardCharsets.UTF_8));
                out.flush();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
    }

    @PreDestroy
    public void destroy() {
        running = false;
        if (readThread != null) {
            readThread.interrupt();
        }
        if (serialPort != null && serialPort.isOpen()) {
            serialPort.closePort();
            System.out.println("COM порт закрыт.");
        }
    }
}
