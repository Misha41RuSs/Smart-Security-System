package com.security.app;

import com.security.app.repository.CommandLogRepository;
import com.security.app.repository.SecurityEventRepository;
import com.security.app.repository.SubscriberRepository;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;
import org.telegram.telegrambots.meta.api.objects.Message;
import org.telegram.telegrambots.meta.api.objects.Update;
import org.telegram.telegrambots.meta.api.objects.User;

import static org.junit.jupiter.api.Assertions.assertNotNull;

public class TelegramBotServiceTest {

    private TelegramBotService telegramBotService;

    @Mock
    private AppState appState;

    @Mock
    private SubscriberRepository subscriberRepository;

    @Mock
    private CommandLogRepository commandLogRepository;

    @Mock
    private SecurityEventRepository securityEventRepository;

    @Mock
    private Update update;

    @Mock
    private Message message;

    @Mock
    private User user;

    @BeforeEach
    public void setUp() {
        MockitoAnnotations.openMocks(this);
        telegramBotService = new TelegramBotService(
                appState,
                "fakeToken",
                "fakeUsername",
                subscriberRepository,
                commandLogRepository,
                securityEventRepository
        );
    }

    @Test
    public void testBotInitialization() {
        assertNotNull(telegramBotService);
    }
}
