package com.security.app;

import org.springframework.boot.CommandLineRunner;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.telegram.telegrambots.meta.TelegramBotsApi;
import org.telegram.telegrambots.updatesreceivers.DefaultBotSession;

@SpringBootApplication
/**
 * Main Spring Boot application class for the Smart Security System.
 */
public class SecurityApp {
    public static void main(String[] args) {
        SpringApplication.run(SecurityApp.class, args);
    }

    @Bean
    public CommandLineRunner registerBot(TelegramBotService telegramBotService) {
        return args -> {
            try {
                TelegramBotsApi botsApi = new TelegramBotsApi(DefaultBotSession.class);
                botsApi.registerBot(telegramBotService);
                System.out.println("====== TELEGRAM BOT REGISTERED SUCCESSFULLY! ======");
            } catch (Exception e) {
                System.err.println("====== ERROR REGISTERING TELEGRAM BOT ======");
                e.printStackTrace();
            }
        };
    }
}
